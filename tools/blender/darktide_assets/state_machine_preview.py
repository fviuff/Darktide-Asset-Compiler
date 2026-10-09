"""Viewport playback of the asset's state graph, the way the engine runs the compiled state machine.

Every layer runs on its own and starts in its first state; an event reaches every layer and takes the first
matching transition out of the layer's current state (a range transition only when its variable is inside the
range), cross-fading over the transition's blend time. A state plays one clip, a blend of clips (weights from the
engine's match_range / match_range_2d, normalized; every clip runs at the first clip's pace) or a random clip.
Timed events and the exit event go out like the engine sends them, so transitions on them run too. Layers above 0
replace (or, when additive, add to) the layers below on the bones their state's mask names.

Clips are the Blender actions the GLB export turns into clips, found by name from the last Inspect Asset. The rig's
own action and pose are put back when the preview stops.
"""
import math
import random
import re
import time

import bpy
from mathutils import Quaternion, Vector

from . import state_graph

STEP = 1.0 / 30.0


clip_names = state_graph.clip_names


def _match_range(v, lower, value, upper):
    a = 1.0 if value == lower else (v - lower) / (value - lower)
    b = 1.0 if upper == value else (upper - v) / (upper - value)
    return min(max(min(a, b), 0.0), 1.0)


def _pairs(text):
    out = []
    for part in text.split(","):
        part = part.strip()
        if part:
            out.append([float(x) for x in part.split(":")])
    return out


def blend_weights(clips, x, y=None):
    """Normalized weights of blend clips [(clip, value[, value2])] at the variable values (state_machine_writer)."""
    def axis(values, v, own):
        values = sorted(set(values))
        at = values.index(own)
        lower = values[at - 1] if at > 0 else own
        upper = values[at + 1] if at + 1 < len(values) else own
        return _match_range(v, lower, own, upper)
    xs = [c[1] for c in clips]
    ys = [c[2] for c in clips] if y is not None else None
    weights = []
    for clip in clips:
        w = axis(xs, x, clip[1])
        if y is not None:
            w *= axis(ys, y, clip[2])
        weights.append(w)
    total = sum(weights)
    return [w / total for w in weights] if total > 0 else [1.0 / len(weights)] * len(weights)


# ---------------------------------------------------------------------------------------------------------
# Clips sampled from Blender actions (local bone transforms relative to rest, like pose bones)


class Clip:
    def __init__(self, action, rig):
        self.action = action
        start, end = action.frame_range
        scene = bpy.context.scene
        self.fps = scene.render.fps / scene.render.fps_base
        self.start = start
        self.length = max((end - start) / self.fps, 1.0 / self.fps)
        self.channels = {}
        self.orders = {b.name: (b.rotation_mode if len(b.rotation_mode) == 3 else "XYZ") for b in rig.pose.bones}
        bag = None
        from bpy_extras import anim_utils
        for slot in action.slots:
            bag = anim_utils.action_get_channelbag_for_slot(action, slot)
            if bag is not None and len(bag.fcurves):
                break
        fcurves = bag.fcurves if bag is not None else []
        for fc in fcurves:
            match = re.match(r'pose\.bones\["(.+)"\]\.(location|rotation_quaternion|rotation_euler|scale)$', fc.data_path)
            if not match or match.group(1) not in rig.pose.bones:
                continue
            self.channels.setdefault(match.group(1), {}).setdefault(match.group(2), {})[fc.array_index] = fc

    def sample(self, seconds):
        """{bone: (location, quaternion, scale)} at a time in seconds from the clip start."""
        frame = self.start + seconds * self.fps
        out = {}
        for bone, channels in self.channels.items():
            loc = channels.get("location", {})
            scale = channels.get("scale", {})
            location = Vector([loc[i].evaluate(frame) if i in loc else 0.0 for i in range(3)])
            size = Vector([scale[i].evaluate(frame) if i in scale else 1.0 for i in range(3)])
            if "rotation_quaternion" in channels:
                q = channels["rotation_quaternion"]
                rotation = Quaternion([q[i].evaluate(frame) if i in q else (1.0 if i == 0 else 0.0) for i in range(4)])
                rotation.normalize()
            elif "rotation_euler" in channels:
                from mathutils import Euler
                e = channels["rotation_euler"]
                order = self.orders.get(bone, "XYZ")
                rotation = Euler([e[i].evaluate(frame) if i in e else 0.0 for i in range(3)], order).to_quaternion()
            else:
                rotation = Quaternion()
            out[bone] = (location, rotation, size)
        return out


def _mix(a, b, t):
    """Blend two local transforms."""
    if a is None:
        return b
    if b is None:
        return a
    return (a[0].lerp(b[0], t), a[1].slerp(b[1], t) if a[1].dot(b[1]) >= 0 else a[1].slerp(-b[1], t), a[2].lerp(b[2], t))


IDENTITY = (Vector((0, 0, 0)), Quaternion(), Vector((1, 1, 1)))


def _blend_poses(poses):
    """Weighted blend of [(weight, pose)]."""
    out = {}
    total = 0.0
    for weight, pose in poses:
        if weight <= 0:
            continue
        total += weight
        t = weight / total
        for bone in set(out) | set(pose):
            out[bone] = _mix(out.get(bone, IDENTITY), pose.get(bone, IDENTITY), t)
    return out


# ---------------------------------------------------------------------------------------------------------
# State machine


class StateRun:
    def __init__(self, machine, index):
        self.machine = machine
        self.index = index
        self.state = machine.states[index]
        self.time = 0.0
        self.loops = 0
        self.sent = set()
        self.pick = None
        if self.state.random:
            self.pick = machine.pick_random(self.state, None)

    def clips(self):
        """[(weight, clip)] of the state right now."""
        s = self.state
        if s.empty:
            return []
        if s.random:
            return [(1.0, self.machine.clip(self.pick))]
        if s.blend:
            pairs = _pairs(state_graph.clips_text(self.machine.options, s.blend_clips))
            if not pairs:
                return []
            x = self.machine.value(s.blend_variable)
            y = self.machine.value(s.blend_variable2) if s.blend_2d else None
            weights = blend_weights(pairs, x, y)
            return [(w, self.machine.clip(int(p[0]))) for w, p in zip(weights, pairs)]
        return [(1.0, self.machine.clip(s.clip_index))]

    def length(self):
        clips = [c for _, c in self.clips() if c is not None]
        return clips[0].length if clips else 1.0

    def speed(self):
        s = self.state
        return self.machine.value(s.speed_variable) if s.speed_from_variable else s.speed

    def step(self, dt):
        if self.state.empty:
            return
        length = self.length()
        before = self.time
        self.time += dt * self.speed()
        once = self.state.playback == "once"
        # timed events and the exit event, once per pass through the clip
        local_before, local_now = before - self.loops * length, self.time - self.loops * length
        for part in self.state.events_at.split(","):
            if ":" in part:
                at, event = part.split(":", 1)
                try:
                    at = float(at)
                except ValueError:
                    continue
                if local_before < at <= local_now and (self.loops, event) not in self.sent:
                    self.sent.add((self.loops, event))
                    self.machine.send(event.strip())
        exit_event = self.state.exit_event.strip()
        if exit_event and local_now >= length - self.state.exit_blend and (self.loops, "#exit") not in self.sent:
            self.sent.add((self.loops, "#exit"))
            self.machine.send(exit_event)
        if not once:
            while self.time - self.loops * length >= length:
                self.loops += 1
                if self.state.random and self.state.random_pick != "on_entry":
                    self.pick = self.machine.pick_random(self.state, self.pick)

    def pose(self):
        clips = self.clips()
        if not clips:
            return None
        first = next((c for _, c in clips if c is not None), None)
        if first is None:
            return None
        phase = self.time / first.length
        if self.state.playback == "once":
            phase = min(phase, 1.0)
        else:
            phase -= math.floor(phase)
        return _blend_poses([(w, c.sample(phase * c.length)) for w, c in clips if c is not None])


class Machine:
    def __init__(self, options, rig):
        self.options = options
        self.rig = rig
        self.states = list(options.state_graph_states)
        self.transitions = state_graph.expand(options)
        self.variables = list(options.state_graph_variables)
        self.names = clip_names(options)
        self.clips = {}
        self.problems = []
        self.queue = []
        self.layers = {}   # layer -> [(run, fade in seconds, faded so far)] newest last
        for index, state in enumerate(self.states):
            if state.layer not in self.layers:
                self.layers[state.layer] = [[StateRun(self, index), 0.0, 0.0]]
        self.masks = {}

    def value(self, index):
        return self.variables[index].preview_value if 0 <= index < len(self.variables) else 0.0

    def clip(self, index):
        if index not in self.clips:
            name = self.names.get(index)
            action = bpy.data.actions.get(name) if name else None
            if action is None:
                self.problems.append("clip %d: %s" % (index, "no action named " + name if name else "run Inspect Asset"))
                self.clips[index] = None
            else:
                self.clips[index] = Clip(action, self.rig)
        return self.clips[index]

    def pick_random(self, state, previous):
        pairs = _pairs(state_graph.clips_text(self.options, state.blend_clips))
        if not pairs:
            return 0
        choices = [p for p in pairs if not (state.random_pick == "no_repeat" and int(p[0]) == previous)] or pairs
        weights = [p[1] if len(p) > 1 else 1.0 for p in choices]
        return int(random.choices(choices, weights=weights)[0][0])

    def send(self, event):
        if event:
            self.queue.append(event)

    def current(self, layer):
        return self.layers[layer][-1][0]

    def _handle(self, event):
        for layer, runs in self.layers.items():
            run = runs[-1][0]
            for transition in self.transitions:
                if transition.from_state != run.index or transition.event_name.strip() != event:
                    continue
                if not 0 <= transition.to_state < len(self.states) or self.states[transition.to_state].layer != layer:
                    continue
                if transition.condition == "range":
                    v = self.value(transition.variable_index)
                    low_ok = v > transition.lower if transition.lower_exclusive else v >= transition.lower
                    high_ok = v < transition.upper if transition.upper_exclusive else v <= transition.upper
                    if not (low_ok and high_ok):
                        continue
                runs.append([StateRun(self, transition.to_state), transition.blend_seconds, 0.0])
                break

    def step(self, dt):
        for layer in self.layers.values():
            for entry in layer:
                entry[0].step(dt)
                entry[2] += dt
            # a state that is fully faded in drops the ones under it
            while len(layer) > 1 and layer[-1][2] >= layer[-1][1]:
                layer.pop(0)
        for _ in range(16):
            if not self.queue:
                break
            self._handle(self.queue.pop(0))

    def mask(self, state):
        """bone -> weight of a state's bone mask (None = every bone)."""
        key = state.bones
        if not key.strip():
            return None
        if key not in self.masks:
            weights = {}
            for part in key.split(","):
                part = part.strip()
                if not part:
                    continue
                name, _, weight = part.partition(":")
                weight = float(weight) if weight else 1.0
                bone = self.rig.data.bones.get(name.strip())
                if bone is None:
                    continue
                for b in [bone] + list(bone.children_recursive):
                    weights[b.name] = weight
            self.masks[key] = weights
        return self.masks[key]

    def layer_pose(self, runs):
        """(pose, weight per state) of one layer: the states cross-fading in it."""
        parts = []
        for k, (run, fade, done) in enumerate(runs):
            weight = 1.0 if k == len(runs) - 1 and (fade <= 0 or done >= fade) else (min(done / fade, 1.0) if fade > 0 else 1.0)
            parts.append([run, weight])
        # each newer state fades over the ones before it
        result = []
        remaining = 1.0
        for run, weight in reversed(parts):
            result.append((run, weight * remaining))
            remaining *= 1.0 - weight
        return result

    def pose(self):
        final = {}
        for layer in sorted(self.layers):
            for run, weight in self.layer_pose(self.layers[layer]):
                if weight <= 0 or run.state.empty:
                    continue
                pose = run.pose()
                if pose is None:
                    continue
                mask = self.mask(run.state)
                for bone in self.rig.pose.bones:
                    w = weight * (1.0 if mask is None else mask.get(bone.name, 0.0))
                    if w <= 0:
                        continue
                    local = pose.get(bone.name, IDENTITY)
                    below = final.get(bone.name, IDENTITY)
                    if run.state.additive:
                        target = (below[0] + local[0], below[1] @ local[1],
                                  Vector([below[2][i] * local[2][i] for i in range(3)]))
                    else:
                        target = local
                    final[bone.name] = _mix(below, target, w)
        return final


# ---------------------------------------------------------------------------------------------------------
# Playback


class Preview:
    def __init__(self, options, rig):
        self.rig_name = rig.name
        self.saved = {b.name: (b.location.copy(), b.rotation_quaternion.copy(), b.rotation_euler.copy(),
                               b.scale.copy()) for b in rig.pose.bones}
        self.saved_action = rig.animation_data.action if rig.animation_data else None
        if rig.animation_data:
            rig.animation_data.action = None
        for variable in options.state_graph_variables:
            variable.preview_value = variable.initial_value
        self.machine = Machine(options, rig)

    def apply(self, rig):
        pose = self.machine.pose()
        for bone in rig.pose.bones:
            location, rotation, scale = pose.get(bone.name, IDENTITY)
            bone.location = location
            if bone.rotation_mode == "QUATERNION":
                bone.rotation_quaternion = rotation
            elif bone.rotation_mode == "AXIS_ANGLE":
                axis, angle = rotation.to_axis_angle()
                bone.rotation_axis_angle = (angle, *axis)
            else:
                bone.rotation_euler = rotation.to_euler(bone.rotation_mode)
            bone.scale = scale

    def restore(self, rig):
        for bone in rig.pose.bones:
            saved = self.saved.get(bone.name)
            if saved:
                bone.location, bone.rotation_quaternion, bone.rotation_euler, bone.scale = saved
        if rig.animation_data is not None:
            rig.animation_data.action = self.saved_action


_preview = None
_last = None


def rig_of(collection):
    if collection is None:
        return None
    return next((o for o in collection.all_objects if o.type == "ARMATURE"), None)


def _tick():
    global _last, _preview
    if _preview is None:
        return None
    rig = bpy.data.objects.get(_preview.rig_name)
    if rig is None:
        _preview = None
        return None
    now = time.monotonic()
    dt = 0.0 if _last is None else min(now - _last, 0.25)
    _last = now
    try:
        _preview.machine.step(dt)
        _preview.apply(rig)
    except Exception as exc:  # a broken graph stops the preview, not Blender's timer
        print("Darktide state machine preview stopped: " + str(exc))
        stop()
        return None
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type in ("VIEW_3D", "PROPERTIES"):
                area.tag_redraw()
    return STEP


def start(options, rig):
    global _preview, _last
    stop()
    _preview = Preview(options, rig)
    _last = None
    bpy.app.timers.register(_tick)
    return _preview


def stop():
    global _preview
    if _preview is not None:
        rig = bpy.data.objects.get(_preview.rig_name)
        if rig is not None:
            _preview.restore(rig)
        _preview = None
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)


def playing():
    return _preview


@bpy.app.handlers.persistent
def _on_load(_):
    global _preview
    _preview = None
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)


class DARKTIDE_OT_sm_preview(bpy.types.Operator):
    bl_idname = "darktide.sm_preview"
    bl_label = "Play State Machine"
    bl_description = "Play the state graph on the rig like the game runs it (send events and set variables below)"

    def execute(self, context):
        from . import _asset_options
        if _preview is not None:
            stop()
            return {"FINISHED"}
        options = _asset_options(context.scene)
        rig = rig_of(context.scene.dt_asset.asset_collection)
        if rig is None:
            self.report({"ERROR"}, "The asset has no armature to play the state machine on")
            return {"CANCELLED"}
        if not options.state_graph_states:
            self.report({"ERROR"}, "Add states first")
            return {"CANCELLED"}
        if not clip_names(options):
            result = bpy.ops.darktide.inspect_asset()
            if result != {"FINISHED"} or not clip_names(options):
                self.report({"ERROR"}, "Inspect Asset found no clips; the preview needs the clip list")
                return {"CANCELLED"}
        start(options, rig)
        return {"FINISHED"}


class DARKTIDE_OT_sm_preview_event(bpy.types.Operator):
    bl_idname = "darktide.sm_preview_event"
    bl_label = "Send Event"
    bl_description = "Send this event to the playing state machine, like Unit.animation_event"

    event: bpy.props.StringProperty()

    def execute(self, context):
        if _preview is not None:
            _preview.machine.send(self.event.strip())
        return {"FINISHED"}


def _search_events(self, context, edit_text):
    """Events a transition of the playing graph reacts to (what the game's scripts would switch states with)."""
    if _preview is None:
        return []
    text = edit_text.lower()
    return sorted({t.event_name for t in _preview.machine.transitions if t.event_name and text in t.event_name.lower()})


def draw(layout, options):
    """Preview controls under the state graph."""
    preview = _preview
    row = layout.row()
    row.operator(DARKTIDE_OT_sm_preview.bl_idname, text="Stop State Machine" if preview else "Play State Machine",
                 icon="PAUSE" if preview else "PLAY", depress=preview is not None)
    if preview is None:
        return
    box = layout.box()
    machine = preview.machine
    for layer in sorted(machine.layers):
        run = machine.current(layer)
        box.label(text="Layer %d: %s" % (layer, run.state.state_name))
    events = sorted({t.event_name for t in machine.transitions if t.event_name})
    if len(events) > 24:
        # many events (patterns, game events): the ones typed out on transitions, and a search over all of them
        box.label(text="%d events switch a state; search one to send it:" % len(events))
        events = sorted({t.event_name.strip() for t in options.state_graph_transitions
                         if t.event_name.strip() and "*" not in t.event_name})
        row = box.row(align=True)
        row.prop(bpy.context.window_manager, "dt_sm_preview_event", text="")
        row.operator(DARKTIDE_OT_sm_preview_event.bl_idname, text="Send").event = \
            bpy.context.window_manager.dt_sm_preview_event
    if events:
        flow = box.grid_flow(columns=3, align=True)
        for event in events:
            flow.operator(DARKTIDE_OT_sm_preview_event.bl_idname, text=event).event = event
    for variable in options.state_graph_variables:
        box.prop(variable, "preview_value", text=variable.variable_name)
    for problem in sorted(set(machine.problems)):
        box.label(text=problem[:120], icon="INFO")


classes = (DARKTIDE_OT_sm_preview, DARKTIDE_OT_sm_preview_event)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.WindowManager.dt_sm_preview_event = bpy.props.StringProperty(
        name="Event", description="An event to send to the previewed state machine (type part of a name to search)",
        search=_search_events)
    bpy.app.handlers.load_pre.append(_on_load)


def unregister():
    stop()
    if _on_load in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.remove(_on_load)
    del bpy.types.WindowManager.dt_sm_preview_event
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
