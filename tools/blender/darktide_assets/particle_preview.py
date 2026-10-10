"""Viewport preview of a particle effect: runs the effect the way the game does and draws its billboards.

The simulation follows the engine's particle runtime: an effect ages each frame and stops emitting after its
duration; every system runs its simulators in order (emitters queue spawns, which run the initializers after
the simulators, so a new particle is first simulated the frame after it is born); billboards get their vertex
data from the vertex writers and are drawn like the particle billboard shader does (texcoord7 = size, texcoord1 =
rotation, tangent = locked up axis, binormal + tangent = full orientation, colour squared).

Billboards are drawn with their material's own game shader (shader_preview: the engine's shader choice for the
billboard, translated to GLSL, with the material's textures and values); a material that can't be loaded (no
game files extract, or a shader the translation doesn't handle) is drawn as soft dots in the particle's colour.
Mesh, light, ribbon and GPU visualizers are not drawn. Components the preview does not run are listed in the panel.
"""
import json
import math
import time

import bpy
import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader

from . import particle_effect, shader_preview

# components the preview runs; anything else is reported as not previewed
INITIALIZERS = {"zero", "random_float", "random_int", "float", "vector", "velocity_cone", "position_sphere",
                "position_box", "position_cylinder", "zero_velocity", "copy", "velocity_box", "velocity_cylinder",
                "tangent_box", "normal_box", "multiply_by_variable"}
SIMULATORS = {"age_age", "position_integrate", "position_integrate_scaled", "velocity_accelerate", "advance_frame",
              "integrate_float_scaled", "integrate_float", "plane_collision", "rate_spawn", "trail_spawn",
              "rate_emitter", "burst_emitter", "local_space", "copy_variable_to_float", "scale_float"}
WRITERS = {"copy_vector3", "write_float", "size", "color", "hdr_color", "copy_float", "rotate_by_velocity",
           "distance_fade", "opacity_kill", "stretch_by_velocity", "rotation_align_to_velocity", "local_age"}

STEP = 1.0 / 30.0
_rng = np.random.default_rng()


def _uniform(low, high, count):
    return low + _rng.random(count, dtype=np.float32) * (high - low)


# ---------------------------------------------------------------------------------------------------------
# Curves as the engine evaluates them: 10 slots, unused x = 10000 + 10n, unused y = the last value; below the
# first point the first value, past the ninth segment the last; a repeated x is a step.
def _padded(curve):
    xs, ys = list(curve["x"]), list(curve["y"])
    n = len(xs)
    last = ys[-1] if ys else 0.0
    return (np.array(xs + [10000.0 + 10 * n] * (10 - n), dtype=np.float32),
            np.array(ys + [last] * (10 - n), dtype=np.float32))


def _padded_gradient(gradient):
    xs, rgb = list(gradient["x"]), [list(c) for c in gradient["rgb"]]
    n = len(xs)
    last = rgb[-1] if rgb else [0.0, 0.0, 0.0]
    return (np.array(xs + [10000.0 + 10 * n] * (10 - n), dtype=np.float32),
            np.array(rgb + [last] * (10 - n), dtype=np.float32))


def _evaluate(padded, t):
    xs, ys = padded
    t = np.asarray(t, dtype=np.float32)
    i = np.minimum(np.sum(t[..., None] > xs[1:10], axis=-1), 9)
    j = np.minimum(i + 1, 9)
    x0, x1 = xs[i], xs[j]
    span = np.where(x1 == x0, 1.0, x1 - x0)
    f = np.where(x1 == x0, 0.0, (t - x0) / span)
    if ys.ndim > 1:
        f = f[..., None]
    value = (1.0 - f) * ys[i] + f * ys[j]
    first = (t < xs[0])
    if ys.ndim > 1:
        first = first[..., None]
    value = np.where(first, ys[0], value)
    last = (i == 9)
    if ys.ndim > 1:
        last = last[..., None]
    return np.where(last, ys[9], value)


def _ratio(age, life):
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(life != 0, age / np.where(life != 0, life, 1), 0).astype(np.float32)


# ---------------------------------------------------------------------------------------------------------
# Matrices follow the engine: rows, points as p @ m[:3, :3] + m[3, :3]
def _engine_matrix(matrix):
    return np.array(matrix, dtype=np.float32).reshape(4, 4).T  # Blender (columns) -> engine (rows)


class System:
    def __init__(self, data, index):
        self.data = data
        self.index = index
        self.capacity = max(0, int(data["capacity"]))
        self.channels = {}
        for channel in data["channels"]:
            self.channels[channel["name"]] = np.zeros((self.capacity, max(1, channel["size"] // 4)), dtype=np.float32)
        self.n = 0
        self.offset = np.array(data["transform"], dtype=np.float32).reshape(4, 4)
        self.state = [{} for _ in data["simulators"]]
        self.curves = {}
        self.billboards = [v for v in data["visualizers"] if v["type"] == "billboard"]

    def channel(self, name):
        return self.channels.get(name) if isinstance(name, str) else None

    def curve(self, key, value):
        if key not in self.curves:
            self.curves[key] = _padded_gradient(value) if "rgb" in value else _padded(value)
        return self.curves[key]

    # -- particles
    def kill(self, keep):
        count = int(keep.sum())
        for array in self.channels.values():
            array[:count] = array[:self.n][keep]
        self.n = count

    def emit(self, count, tm, position, velocity, variables):
        count = min(int(count), self.capacity - self.n)
        if count <= 0:
            return
        start, self.n = self.n, self.n + count
        rows = slice(start, self.n)
        for array in self.channels.values():
            array[rows] = 0
        tm = tm.copy()
        tm[3, :3] = position
        for index, item in enumerate(self.data["initializers"]):
            self.initialize(index, item, rows, count, tm, velocity, variables)

    def initialize(self, index, item, rows, count, tm, velocity, variables):
        kind = item["type"]
        rotation, translation = tm[:3, :3], tm[3, :3]
        if kind == "zero":
            target = self.channel(item["channel"])
            if target is not None:
                target[rows, :max(1, item["size"] // 4)] = 0
        elif kind == "random_float":
            target = self.channel(item["channel"])
            if target is not None:
                target[rows, 0] = _uniform(item["min"], item["max"], count)
        elif kind == "random_int":
            target = self.channel(item["channel"])
            if target is not None:
                target[rows, 0] = np.floor(_uniform(0, 1, count) * (item["max"] - item["min"] + 1) + item["min"])
        elif kind == "float":
            target = self.channel(item["channel"])
            if target is not None:
                target[rows, 0] = item["value"]
        elif kind == "vector":
            target = self.channel(item["channel"])
            if target is not None:
                target[rows, :3] = item["value"]
        elif kind == "velocity_cone":
            target = self.channel(item["velocity"])
            if target is None:
                return
            direction = np.array(item["direction"], dtype=np.float32)
            # a vector across the direction, then the third axis
            helper = np.array([0, 0, 1] if abs(direction[2]) < 0.5 else [1, 0, 0], dtype=np.float32)
            u = np.cross(direction, helper)
            u /= max(np.linalg.norm(u), 1e-4)
            w = np.cross(direction, u)
            speed = _uniform(item["speed_min"], item["speed_max"], count)
            theta = _uniform(item["theta_min"], item["theta_max"], count)
            phi = _uniform(0, 2 * math.pi, count)
            local = (direction[None] * np.cos(theta)[:, None] +
                     (u[None] * np.cos(phi)[:, None] + w[None] * np.sin(phi)[:, None]) * np.sin(theta)[:, None])
            target[rows, :3] = (local * speed[:, None]) @ rotation + velocity
        elif kind == "position_sphere":
            target = self.channel(item["position"])
            if target is None:
                return
            radius = _uniform(item["radius_min"], item["radius_max"], count)
            a = _uniform(-math.pi, math.pi, count)
            b = _uniform(0, 2 * math.pi, count)
            # the engine offsets by the system position only (no rotation)
            target[rows, :3] = np.stack([np.cos(b) * np.sin(a), np.sin(b) * np.sin(a), np.cos(a)], 1) * radius[:, None] + translation
        elif kind in ("position_box", "velocity_box", "tangent_box"):
            target = self.channel(item["position" if kind == "position_box" else "velocity" if kind == "velocity_box" else "tangent"])
            if target is None:
                return
            low, high = np.array(item["min"], dtype=np.float32), np.array(item["max"], dtype=np.float32)
            local = low + _rng.random((count, 3), dtype=np.float32) * (high - low)
            add = translation if kind == "position_box" else velocity if kind == "velocity_box" else 0
            target[rows, :3] = local @ rotation + add
        elif kind == "normal_box":
            # a random normal in the box; tangent = normal x (1,0,0) (or (0,0,1) for a mostly flat normal),
            # binormal = normal x tangent, both turned by the system rotation
            tangent_out, binormal_out = self.channel(item["tangent"]), self.channel(item["binormal"])
            low, high = np.array(item["min"], dtype=np.float32), np.array(item["max"], dtype=np.float32)
            normal = low + _rng.random((count, 3), dtype=np.float32) * (high - low)
            length = np.linalg.norm(normal, axis=1)
            normal = np.where(length[:, None] >= 1e-4, normal / np.maximum(length, 1e-4)[:, None], 0)
            steep = np.abs(normal[:, 2]) >= 0.5
            tangent = np.where(steep[:, None],
                               np.stack([np.zeros(count, np.float32), normal[:, 2], -normal[:, 1]], 1),
                               np.stack([-normal[:, 1], normal[:, 0], np.zeros(count, np.float32)], 1))
            length = np.linalg.norm(tangent, axis=1)
            tangent = np.where(length[:, None] >= 1e-4, tangent / np.maximum(length, 1e-4)[:, None], 0)
            binormal = np.cross(normal, tangent)
            if tangent_out is not None:
                tangent_out[rows, :3] = tangent @ rotation
            if binormal_out is not None:
                binormal_out[rows, :3] = binormal @ rotation
        elif kind == "position_cylinder":
            target = self.channel(item["position"])
            if target is None:
                return

            def ranged(name, variable):
                index = item[variable]
                if 0 <= index < len(variables):
                    return _uniform(variables[index][0], variables[index][1], count)
                return _uniform(item[name + "_min"], item[name + "_max"], count)
            radius, height, angle = ranged("radius", "radius_variable"), ranged("height", "height_variable"), ranged("angle", "angle_variable")
            local = np.stack([np.cos(angle) * radius, np.sin(angle) * radius, height], 1)
            target[rows, :3] = local @ rotation + translation
        elif kind == "zero_velocity":
            target = self.channel(item["velocity"])
            if target is not None:
                target[rows, :3] = velocity
        elif kind == "copy":
            source, target = self.channel(item["source"]), self.channel(item["destination"])
            if source is not None and target is not None:
                width = min(max(1, item["size"] // 4), source.shape[1], target.shape[1])
                target[rows, :width] = source[rows, :width]
        elif kind == "velocity_cylinder":
            position, target = self.channel(item["position"]), self.channel(item["velocity"])
            if position is None or target is None:
                return
            axis = rotation[2]
            offset = position[rows, :3] - translation
            radial = offset - axis[None] * (offset @ axis)[:, None]
            length = np.linalg.norm(radial, axis=1)
            radial = np.where(length[:, None] >= 1e-4, radial / np.maximum(length, 1e-4)[:, None], 0)
            tangent = np.cross(axis[None], radial)
            target[rows, :3] = (radial * _uniform(item["radial_min"], item["radial_max"], count)[:, None] +
                                axis[None] * _uniform(item["z_min"], item["z_max"], count)[:, None] +
                                tangent * _uniform(item["angular_min"], item["angular_max"], count)[:, None] + velocity)
        elif kind == "multiply_by_variable":
            target = self.channel(item["channel"])
            if target is not None and 0 <= item["variable"] < len(variables):
                width = min(3, max(1, item["channel_size"]), target.shape[1])
                target[rows, :width] *= np.array(variables[item["variable"]][:width], dtype=np.float32)

    # -- simulators
    def simulate(self, dt, system_t, tm, variables, events):
        for index, item in enumerate(self.data["simulators"]):
            if item["type"] in SIMULATORS:
                self.step(index, item, dt, system_t, tm, variables, events)

    def step(self, index, item, dt, system_t, tm, variables, events):
        kind, state, n = item["type"], self.state[index], self.n
        rows = slice(0, n)
        if kind == "age_age":
            age, life = self.channel(item["age"]), self.channel(item["life"])
            if age is None or life is None:
                return
            age[rows, 0] += dt
            self.kill(~(age[rows, 0] > life[rows, 0]))
        elif kind == "position_integrate":
            position, velocity = self.channel(item["position"]), self.channel(item["velocity"])
            if position is not None and velocity is not None:
                position[rows, :3] += velocity[rows, :3] * dt
        elif kind == "position_integrate_scaled":
            position, velocity = self.channel(item["position"]), self.channel(item["velocity"])
            age, life = self.channel(item["age"]), self.channel(item["life"])
            if position is None or velocity is None:
                return
            scale = 1.0
            if 0 <= item["scale_variable"] < len(variables):
                scale = variables[item["scale_variable"]][0]
            curve = self.curve((index, "scale"), item["scale"])
            if item["over_system_lifetime"] or age is None or life is None:
                factor = np.full(n, _evaluate(curve, system_t), dtype=np.float32)
            else:
                factor = _evaluate(curve, _ratio(age[rows, 0], life[rows, 0]))
            position[rows, :3] += velocity[rows, :3] * (factor * scale * dt)[:, None]
        elif kind == "velocity_accelerate":
            velocity = self.channel(item["velocity"])
            if velocity is not None:
                velocity[rows, :3] += np.array(item["acceleration"], dtype=np.float32) * dt
        elif kind == "advance_frame":
            frame = self.channel(item["channel"])
            if frame is None:
                return
            frame[rows, 0] += item["speed"] * dt
            past = frame[rows, 0].astype(np.int64) > item["end"]
            if item["loop"]:
                frame[rows, 0] = np.where(past, frame[rows, 0] - (item["end"] - item["start"] + 1), frame[rows, 0])
            else:
                frame[rows, 0] = np.where(past, item["end"], frame[rows, 0])
        elif kind == "integrate_float_scaled":
            value, velocity = self.channel(item["value"]), self.channel(item["velocity"])
            age, life = self.channel(item["age"]), self.channel(item["life"])
            if value is None or velocity is None or age is None or life is None:
                return
            factor = _evaluate(self.curve((index, "scale"), item["scale"]), _ratio(age[rows, 0], life[rows, 0]))
            value[rows, 0] += factor * velocity[rows, 0] * dt
        elif kind == "integrate_float":
            value, velocity = self.channel(item["value"]), self.channel(item["velocity"])
            if value is not None and velocity is not None:
                value[rows, 0] += velocity[rows, 0] * dt
        elif kind == "plane_collision":
            position, velocity = self.channel(item["position"]), self.channel(item["velocity"])
            if position is None or velocity is None:
                return
            hit = (position[rows, 2] < tm[3, 2] + item["offset"]) & (velocity[rows, 2] < 0)
            keep = 1.0 - item["friction"]
            bounced = velocity[rows].copy()
            bounced[:, 0] *= keep
            bounced[:, 1] *= keep
            bounced[:, 2] = keep * -(bounced[:, 2] * item["restitution"])
            velocity[rows] = np.where(hit[:, None], bounced, velocity[rows])
        elif kind == "rate_spawn":
            position, spawn = self.channel(item["position"]), self.channel(item["spawn"])
            velocity = self.channel(item["velocity"])
            if position is None or spawn is None:
                return
            spawn[rows, 0] += item["particles_per_second"] * dt
            for i in np.nonzero(spawn[rows, 0] > 1.0)[0]:
                whole = int(spawn[i, 0])
                inherited = velocity[i, :3] * item["inherit_velocity"] if velocity is not None else np.zeros(3, np.float32)
                events.append((item["target_system"], position[i, :3].copy(), inherited, whole))
                spawn[i, 0] -= whole
        elif kind == "trail_spawn":
            position, last = self.channel(item["position"]), self.channel(item["last_position"])
            velocity = self.channel(item["velocity"])
            if position is None or last is None or item["particles_per_meter"] <= 0:
                return
            step = 1.0 / item["particles_per_meter"]
            for i in range(n):
                distance = float(np.linalg.norm(position[i, :3] - last[i, :3]))
                while step < distance:
                    f = step / distance
                    point = (1 - f) * last[i, :3] + f * position[i, :3]
                    inherited = velocity[i, :3] * item["inherit_velocity"] if velocity is not None else np.zeros(3, np.float32)
                    events.append((item["target_system"], point.copy(), inherited, 1))
                    last[i, :3] = point
                    distance -= step
        elif kind == "rate_emitter":
            if state.get("rate", 0.0) <= 0.0:
                state["rate"] = float(_uniform(item["rate_min"], item["rate_max"], 1)[0])
            state["total"] = float(_evaluate(self.curve((index, "rate"), item["rate"]), system_t)) * state["rate"] * dt + state.get("total", 0.0)
            count = int(state["total"] - state.get("emitted", 0))
            state["emitted"] = state.get("emitted", 0) + count
            if count:
                state["rate"] = float(_uniform(item["rate_min"], item["rate_max"], 1)[0])
                events.append((self.index, tm[3, :3].copy(), np.zeros(3, np.float32), count))
        elif kind == "burst_emitter":
            state["time"] = state.get("time", 0.0) + dt
            bursts = item["bursts"]
            done = state.get("index", 0)
            if done < min(10, len(bursts)) and bursts[done]["time"] < state["time"]:
                events.append((self.index, tm[3, :3].copy(), np.zeros(3, np.float32), bursts[done]["count"]))
                state["index"] = done + 1
        elif kind == "local_space":
            # particles move along with the system: the change of its transform since the last frame
            previous = state.get("tm")
            state["tm"] = tm.copy()
            if previous is None:
                return
            delta = np.linalg.inv(previous) @ tm
            position = self.channel(item["position"])
            if position is not None:
                position[rows, :3] = position[rows, :3] @ delta[:3, :3] + delta[3, :3]
            for name in ("tangent", "binormal", "velocity"):
                target = self.channel(item[name])
                if target is not None:
                    target[rows, :3] = target[rows, :3] @ delta[:3, :3]
        elif kind == "copy_variable_to_float":
            target = self.channel(item["destination"])
            if target is not None and 0 <= item["variable"] < len(variables):
                target[rows, 0] = variables[item["variable"]][min(item["component"], 2)]
        elif kind == "scale_float":
            target, scale = self.channel(item["destination"]), self.channel(item["scale_by"])
            if target is not None and scale is not None:
                target[rows, 0] *= scale[rows, 0]

    # -- billboards: the vertex data the writers write per particle
    def billboard_vertices(self, visualizer, system_t, camera):
        n = self.n
        if not n:
            return None
        rows = slice(0, n)
        vertex = {}
        for channel in visualizer["vertex"]:
            name = channel["component"] + (str(channel["set"]) if channel["component"] == "texcoord" else "")
            vertex[name] = np.zeros((n, 4), dtype=np.float32)
        alpha = np.ones(n, dtype=np.float32)
        right, up, forward, eye = camera
        for index, item in enumerate(visualizer["writers"]):
            kind = item["type"]
            if kind not in WRITERS:
                continue
            if kind == "copy_vector3":
                source, target = self.channel(item["source"]), vertex.get(item["destination"])
                if source is not None and target is not None:
                    target[:, :3] = source[rows, :3]
            elif kind == "write_float":
                target = vertex.get(item["destination"])
                if target is not None:
                    target[:, min(item["component"], 3)] = item["value"]
            elif kind == "size":
                source, target = self.channel(item["source"]), vertex.get(item["destination"])
                age, life = self.channel(item["age"]), self.channel(item["life"])
                if source is None or target is None:
                    continue
                curve = self.curve(("writer", id(visualizer), index), item["scale"])
                if item["over_system_lifetime"] or age is None or life is None:
                    value = _evaluate(curve, system_t) * source[rows, 0]
                else:
                    value = _evaluate(curve, _ratio(age[rows, 0], life[rows, 0])) * source[rows, 0]
                if item["component"] < 2:
                    target[:, item["component"]] = value
                else:
                    target[:, 0] = target[:, 1] = value
            elif kind in ("color", "hdr_color"):
                target = vertex.get(item["destination"])
                age, life = self.channel(item["age"]), self.channel(item["life"])
                if target is None or age is None or life is None:
                    continue
                x = _ratio(age[rows, 0], life[rows, 0])
                opacity = np.sqrt(np.maximum(_evaluate(self.curve(("op", id(visualizer), index), item["opacity"]), x), 0))
                rgb = _evaluate(self.curve(("rgb", id(visualizer), index), item["gradient"]), x)
                luminance = self.channel(item["luminance_source"]) if item["luminance"] else None
                if luminance is not None:
                    rgb = rgb * luminance[rows, 0][:, None]
                    if kind == "color":
                        rgb = np.clip(rgb, 0, 255)
                # the engine's vertex layouts: color = ARGB bytes (a u32, read as bytes B, G, R, A), hdr_color =
                # floats (opacity, r, g, b) with rgb / 255
                if kind == "color":
                    argb = np.stack([opacity * 255, rgb[:, 0], rgb[:, 1], rgb[:, 2]], 1)
                    packed = np.trunc(argb).astype(np.int64) & 0xff
                    target[:, :] = packed[:, [3, 2, 1, 0]] / 255.0
                    alpha[:] = packed[:, 0] / 255.0
                else:
                    target[:, 0] = opacity
                    target[:, 1:4] = rgb / 255.0
                    alpha[:] = opacity
            elif kind == "copy_float":
                source, target = self.channel(item["source"]), vertex.get(item["destination"])
                if source is not None and target is not None:
                    target[:, min(item["component"], 3)] = source[rows, 0]
            elif kind == "local_age":
                target = vertex.get(item["destination"])
                age, life = self.channel(item["age"]), self.channel(item["life"])
                if target is not None and age is not None and life is not None:
                    target[:, min(item["component"], 3)] = _ratio(age[rows, 0], life[rows, 0])
            elif kind == "rotate_by_velocity":
                velocity, target = self.channel(item["velocity"]), vertex.get(item["destination"])
                if velocity is not None and target is not None:
                    target[:, 0] = np.arctan2(-(velocity[rows, :3] @ right), velocity[rows, :3] @ up)
            elif kind == "rotation_align_to_velocity":
                velocity, target = self.channel(item["velocity"]), vertex.get(item["tangent"])
                if velocity is not None and target is not None:
                    length = np.linalg.norm(velocity[rows, :3], axis=1)
                    target[:, :3] = np.where(length[:, None] >= 1e-4, velocity[rows, :3] / np.maximum(length, 1e-4)[:, None], 0)
            elif kind == "stretch_by_velocity":
                velocity, target = self.channel(item["velocity"]), vertex.get(item["destination"])
                if velocity is None or target is None:
                    continue
                span = item["velocity_max"] - item["velocity_min"]
                slope = (item["stretch_max"] - item["stretch_min"]) / span if span else 0.0
                stretch = (np.linalg.norm(velocity[rows, :3], axis=1) - item["velocity_min"]) * slope + item["stretch_min"]
                target[:, 1] *= np.clip(stretch, item["stretch_min"], item["stretch_max"])
            elif kind == "distance_fade":
                position = self.channel(item["position"])
                hdr = not item["color"]
                target = vertex.get(item["hdr_color"]) if hdr else vertex.get(item["color"])
                if position is None or target is None:
                    continue
                depth = (position[rows, :3] - eye) @ forward
                near, near_fade, far, far_fade = item["near_range"], item["near_fade"], item["far_range"], item["far_fade"]
                fade = np.ones(n, dtype=np.float32)
                fade = np.where(depth < near + near_fade, (depth - near) / near_fade if near_fade else 1.0, fade)
                fade = np.where(depth > far - far_fade, (far - depth) / far_fade if far_fade else 1.0, fade)
                fade = np.where((depth < near) | (depth > far), 0.0, fade)
                slot = 0 if hdr else 3
                target[:, slot] *= fade
                if not hdr:
                    target[:, slot] = np.trunc(target[:, slot] * 255) / 255
                alpha[:] = target[:, slot]
        if vertex.get("position") is None:
            return None
        return vertex, alpha


def billboard_quads(vertex, alpha, camera):
    """Soft-dot quads for a billboard whose material the viewport can't draw: corners, uv, rgba, triangles."""
    right, up, forward, eye = camera
    position = vertex.get("position")
    size = vertex.get("texcoord7")
    if position is None or size is None:
        return None
    visible = alpha > 0
    if not visible.any():
        return None
    color = vertex.get("color")
    if color is not None:
        rgba = color[visible][:, [2, 1, 0, 3]]
    elif vertex.get("hdr_color") is not None:
        rgba = vertex["hdr_color"][visible][:, [1, 2, 3, 0]]
    else:
        rgba = np.ones((int(visible.sum()), 4), dtype=np.float32)
    position, size = position[visible, :3], size[visible, :2]
    count = len(position)
    angle = vertex["texcoord1"][visible, 0] if "texcoord1" in vertex else np.zeros(count, dtype=np.float32)
    tangent = vertex.get("tangent")
    binormal = vertex.get("binormal")
    if tangent is not None and binormal is not None:
        y, x = tangent[visible, :3], binormal[visible, :3]
    elif tangent is not None:
        y = tangent[visible, :3]
        x = np.cross(position - eye, y)
        x /= np.maximum(np.linalg.norm(x, axis=1), 1e-6)[:, None]
    else:
        x = np.repeat(right[None], count, 0)
        y = np.repeat(up[None], count, 0)
    c, s = np.cos(angle)[:, None], np.sin(angle)[:, None]
    x_axis, y_axis = x * c + y * s, y * c - x * s
    half_x, half_y = x_axis * size[:, :1] * 0.5, y_axis * size[:, 1:2] * 0.5
    corners = np.stack([position - half_x - half_y, position + half_x - half_y,
                        position + half_x + half_y, position - half_x + half_y], 1).reshape(-1, 3)
    uv = np.tile(np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=np.float32), (count, 1))
    rgba = rgba * rgba  # the billboard shader squares the vertex colour
    rgba = np.repeat(rgba, 4, 0)
    base = np.arange(count, dtype=np.int32)[:, None] * 4
    triangles = np.concatenate([base + [0, 1, 2], base + [0, 2, 3]], 0).astype(np.int32)
    return corners, uv, rgba, triangles


# the engine's quad corners (POSITION1) for every billboard
CORNERS = np.array([[-1, -1, 0, 0], [1, -1, 0, 0], [1, 1, 0, 0], [-1, 1, 0, 0]], dtype=np.float32)
SEMANTICS = {"position": "POSITION0", "color": "COLOR0", "tangent": "TANGENT0", "binormal": "BINORMAL0"}


def billboard_attributes(vertex, alpha, attributes, sort_eye=None):
    """Per-corner vertex attributes for a game shader: each particle's channels on its four corners."""
    keep = np.nonzero(alpha > 0)[0]
    if not len(keep):
        return None
    if sort_eye is not None:
        depth = np.linalg.norm(vertex["position"][keep, :3] - sort_eye, axis=1)
        keep = keep[np.argsort(-depth, kind="stable")]
    count = len(keep)
    by_semantic = {}
    for name, data in vertex.items():
        semantic = SEMANTICS.get(name) or ("TEXCOORD" + name[8:] if name.startswith("texcoord") else None)
        if semantic:
            by_semantic[semantic] = data
    out = {}
    for name, semantic in attributes:
        if semantic == "POSITION1":
            out[name] = np.tile(CORNERS, (count, 1))
        elif name == "dt_instance":
            out[name] = np.repeat(keep.astype(np.float32), 4)
        elif name == "dt_vertex":
            out[name] = np.tile(np.arange(4, dtype=np.float32), count)
        else:
            data = by_semantic.get(semantic)
            out[name] = np.repeat(data[keep], 4, 0) if data is not None else np.zeros((count * 4, 4), dtype=np.float32)
    base = np.arange(count, dtype=np.int32)[:, None] * 4
    triangles = np.concatenate([base + [0, 1, 2], base + [0, 2, 3]], 0).astype(np.int32)
    return out, triangles


class EffectPreview:
    def __init__(self, obj):
        self.name = obj.name
        self.checked = time.monotonic()
        self.start(obj)

    def start(self, obj):
        self.effect = particle_effect.description(obj.dt_particles)
        self.signature = json.dumps(self.effect, sort_keys=True)
        self.systems = [System(data, index) for index, data in enumerate(self.effect["systems"])]
        self.variables = [list(v["value"]) for v in self.effect["variables"]]
        self.life_time = self.effect["life_time"]
        self.age = 0.0
        self.emitting = True
        self.pending = 0.0
        self.finished_for = 0.0
        self.load_materials(obj)

    def update(self, obj, dt):
        orientation = _engine_matrix(obj.matrix_world)
        self.pending = min(self.pending + dt, 0.25)
        while self.pending >= STEP:
            self.pending -= STEP
            self.tick(STEP, orientation)
            if not self.emitting and not any(system.n for system in self.systems):
                # done: play it again after a moment, like the editor's preview
                self.finished_for += STEP
                if self.finished_for > 0.5:
                    self.start(obj)
                    return

    def tick(self, dt, orientation):
        self.age += dt
        if self.life_time < self.age:
            self.emitting = False
        system_t = min(max(self.age / self.life_time, 0.0), 1.0) if self.life_time else 1.0
        events = []
        for system in self.systems:
            system.simulate(dt, system_t, system.offset @ orientation, self.variables, events)
        for target, position, velocity, count in events:
            if self.emitting and 0 <= target < len(self.systems):
                system = self.systems[target]
                system.emit(count, system.offset @ orientation, position, velocity, self.variables)

    def billboards(self, camera):
        """(visualizer, its material shader or None, vertex data, alpha) for every billboard with particles."""
        system_t = min(max(self.age / self.life_time, 0.0), 1.0) if self.life_time else 1.0
        out = []
        for system in self.systems:
            for visualizer in system.billboards:
                data = system.billboard_vertices(visualizer, system_t, camera)
                if data is not None:
                    out.append((visualizer, self.materials.get(id(visualizer)), data[0], data[1]))
        return out

    def load_materials(self, obj):
        """The game shader of every billboard's material (the panel shows why one can't be used)."""
        from . import prefs
        self.materials = {}
        self.problems = []
        settings = prefs()
        extract = bpy.path.abspath(settings.game_extract_folder.strip())
        game = bpy.path.abspath(settings.game_folder.strip())
        edits = {}
        for item in obj.dt_particles.materials:
            try:
                values = particle_effect._parse_values(item.values)
                textures = particle_effect._parse_textures(item.textures, lambda path: path)
            except ValueError:
                continue
            edits[item.material_hash] = {
                "variables": {shader_preview.name_id(k): (v if isinstance(v, list) else [v]) for k, v in values.items()},
                "textures": {shader_preview.name_id(k): v for k, v in textures.items()}}
        for system in self.systems:
            for visualizer in system.billboards:
                material = visualizer["material"].lstrip("#")
                if not extract or not game:
                    self.problems.append("set 'Game files extract' and 'Darktide folder' in Setup to see the materials")
                    continue
                key = (material, json.dumps(visualizer["vertex"], sort_keys=True), visualizer.get("mode", 0),
                       json.dumps(edits.get(material), sort_keys=True))
                cached = _material_cache.get(key)
                if cached is None:
                    try:
                        cached = shader_preview.material_shader(int(material, 16), visualizer, extract, game)
                        cached.edits = edits.get(material)
                    except Exception as exc:  # any shader the translation can't handle falls back to dots
                        cached = str(exc)[:160] or type(exc).__name__
                    _material_cache[key] = cached
                if isinstance(cached, str):
                    self.problems.append("material " + material + ": " + cached)
                else:
                    self.materials[id(visualizer)] = cached


def not_previewed(settings):
    """Component and visualizer types of the effect the preview does not run or draw."""
    missing = set()
    for system in settings.systems:
        missing.update(c.type for c in system.initializers if c.type not in INITIALIZERS)
        missing.update(c.type for c in system.simulators if c.type not in SIMULATORS)
        for visualizer in system.visualizers:
            if visualizer.type != "billboard":
                missing.add(visualizer.type + " visualizer")
            else:
                missing.update(w.type for w in visualizer.writers if w.type not in WRITERS and w.type != "write_int")
    return sorted(missing)


# ---------------------------------------------------------------------------------------------------------
# Playback: a timer steps every previewing effect, a draw handler draws them in every 3D view.
_previews = {}
_handle = None
_last = None
_shader = None
_dot = None
_started = time.monotonic()
_material_cache = {}
_uniforms = shader_preview.Uniforms()


def _make_shader():
    info = gpu.types.GPUShaderCreateInfo()
    info.push_constant("MAT4", "view_projection")
    info.sampler(0, "FLOAT_2D", "sprite")
    info.vertex_in(0, "VEC3", "pos")
    info.vertex_in(1, "VEC2", "uv")
    info.vertex_in(2, "VEC4", "color")
    interface = gpu.types.GPUStageInterfaceInfo("darktide_fx_interface")
    interface.smooth("VEC2", "uv_out")
    interface.smooth("VEC4", "color_out")
    info.vertex_out(interface)
    info.fragment_out(0, "VEC4", "frag")
    info.vertex_source("void main() { uv_out = uv; color_out = color; gl_Position = view_projection * vec4(pos, 1.0); }")
    info.fragment_source("void main() { frag = color_out * texture(sprite, uv_out).r; }")
    return gpu.shader.create_from_info(info)


def _make_dot():
    size = 32
    coords = (np.arange(size, dtype=np.float32) + 0.5) / size * 2 - 1
    distance = np.sqrt(coords[None, :] ** 2 + coords[:, None] ** 2)
    falloff = np.clip(1 - distance, 0, 1) ** 1.5
    pixels = np.repeat(falloff.reshape(-1, 1), 4, 1).astype(np.float32)
    return gpu.types.GPUTexture((size, size), format="RGBA16F",
                                data=gpu.types.Buffer("FLOAT", size * size * 4, pixels.ravel().tolist()))


def _draw():
    global _shader, _dot
    from . import prefs
    if not _previews:
        return
    region = bpy.context.region_data
    if region is None:
        return
    if _shader is None:
        _shader, _dot = _make_shader(), _make_dot()
    view = region.view_matrix.inverted()
    camera = (np.array(view.col[0][:3], dtype=np.float32), np.array(view.col[1][:3], dtype=np.float32),
              -np.array(view.col[2][:3], dtype=np.float32), np.array(view.col[3][:3], dtype=np.float32))
    area_region = bpy.context.region
    shader_preview.set_region_size(area_region.width, area_region.height)
    engine = shader_preview.engine_values(region, bpy.context.space_data, time.monotonic() - _started, STEP)
    extract = bpy.path.abspath(prefs().game_extract_folder.strip())
    gpu.state.depth_test_set("LESS_EQUAL")
    gpu.state.depth_mask_set(False)
    for preview in _previews.values():
        for visualizer, material, vertex, alpha in preview.billboards(camera):
            if material is not None:
                shader_preview.prepare(material, extract, material.edits)
                if material.shader is not None and _draw_game(material, visualizer, vertex, alpha, engine, camera):
                    continue
            quads = billboard_quads(vertex, alpha, camera)
            if quads is None:
                continue
            corners, uv, rgba, triangles = quads
            gpu.state.blend_set("ALPHA")
            _shader.bind()
            _shader.uniform_float("view_projection", region.perspective_matrix)
            _shader.uniform_sampler("sprite", _dot)
            batch = batch_for_shader(_shader, "TRIS", {"pos": corners, "uv": uv, "color": rgba}, indices=triangles)
            batch.draw(_shader)
    gpu.state.depth_mask_set(True)
    gpu.state.depth_test_set("NONE")
    gpu.state.blend_set("NONE")


def _bind(setter, name, value):
    try:
        setter(name, value)
    except ValueError:  # not used by the shader after compiling
        pass


def _draw_game(material, visualizer, vertex, alpha, engine, camera):
    """Draw one billboard with its material's game shader (premultiplied alpha, like every game particle)."""
    program = material.program
    built = billboard_attributes(vertex, alpha, program.attributes, camera[3] if visualizer.get("sort") else None)
    if built is None:
        return True
    attributes, triangles = built
    shader = material.shader
    gpu.state.blend_set("ALPHA_PREMULT")
    shader.bind()
    width, height = shader_preview.region_size()
    # the GLSL compiler drops inputs the shader doesn't use; those can't be bound
    _bind(shader.uniform_float, "dt_viewport", (0.0, 0.0, width, height))
    for name, buffer in program.cbuffers.items():
        registers = shader_preview.cbuffer_data(buffer, engine, material.variables)
        _bind(shader.uniform_block, buffer["glsl"], _uniforms.get((id(material), name), registers))
    for name, texture in material.gpu_textures.items():
        _bind(shader.uniform_sampler, name, texture)
    used = {name for name, _ in shader.attrs_info_get()}
    batch = batch_for_shader(shader, "TRIS", {k: v for k, v in attributes.items() if k in used}, indices=triangles)
    batch.draw(shader)
    return True


def _tick():
    global _last
    now = time.monotonic()
    dt = 0.0 if _last is None else now - _last
    _last = now
    for name in list(_previews):
        obj = bpy.data.objects.get(name)
        if obj is None or not obj.dt_particles.enabled or not obj.dt_particles.systems:
            del _previews[name]
            continue
        preview = _previews[name]
        try:
            # edits restart the preview; reading the description walks every curve, so not every frame
            if now - preview.checked > 0.5:
                preview.checked = now
                if json.dumps(particle_effect.description(obj.dt_particles), sort_keys=True) != preview.signature:
                    preview.start(obj)
            preview.update(obj, dt)
        except Exception as exc:  # a broken effect stops its own preview, not the timer
            print("Darktide particle preview stopped for " + name + ": " + str(exc))
            del _previews[name]
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()
    if not _previews:
        _stop()
        return None
    return STEP


def _stop():
    global _handle, _last
    if _handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_handle, "WINDOW")
        _handle = None
    _last = None
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)


def is_previewing(obj):
    return obj.name in _previews


def preview_problems(obj):
    """Why billboards of the playing preview are drawn as dots instead of with their material."""
    preview = _previews.get(obj.name)
    if preview is None:
        return []
    return sorted(set(preview.problems) | {"shader: " + m.error for m in preview.materials.values() if m.error})


def toggle(obj):
    global _handle
    if obj.name in _previews:
        del _previews[obj.name]
        if not _previews:
            _stop()
        return False
    _previews[obj.name] = EffectPreview(obj)
    if _handle is None:
        _handle = bpy.types.SpaceView3D.draw_handler_add(_draw, (), "WINDOW", "POST_VIEW")
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick)
    return True


def restart(obj):
    if obj.name in _previews:
        _previews[obj.name].start(obj)


@bpy.app.handlers.persistent
def _on_load(_):
    _previews.clear()
    _stop()


class DARKTIDE_OT_fx_preview(bpy.types.Operator):
    bl_idname = "darktide.fx_preview"
    bl_label = "Preview"
    bl_description = "Play the effect in the viewport like the game runs and draws it (needs the game files extract for the materials)"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.dt_particles.enabled

    def execute(self, context):
        try:
            toggle(context.object)
        except (KeyError, TypeError, ValueError) as exc:
            self.report({"ERROR"}, "The effect can't be previewed: " + str(exc)[:160])
            return {"CANCELLED"}
        return {"FINISHED"}


class DARKTIDE_OT_fx_preview_restart(bpy.types.Operator):
    bl_idname = "darktide.fx_preview_restart"
    bl_label = "Restart"
    bl_description = "Play the effect from the start"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.dt_particles.enabled

    def execute(self, context):
        restart(context.object)
        return {"FINISHED"}


classes = (DARKTIDE_OT_fx_preview, DARKTIDE_OT_fx_preview_restart)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.app.handlers.load_pre.append(_on_load)


def unregister():
    _previews.clear()
    _stop()
    if _on_load in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.remove(_on_load)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
