"""Carry an animation from any armature onto the asset's Darktide armature.

The source rig is laid onto the Darktide skeleton the same way Fit to Darktide Skeleton lays it (FitPlan: one global
turn and scale, then a turn per mapped bone). A source bone's motion in the world (its rotation away from its rest
pose) is turned by that same fit rotation and applied to the Darktide bone it maps to, so the fitted mesh moves the
way the source mesh moved. The hips also move: their path is scaled and turned like the fit. Every other bone keeps
its Darktide length. After a fit the source rig's own extra bones (hair, skirts, tails) are carried along too.

Works for the rig the model was fitted from (its animation, e.g. an MMD dance loaded with mmd tools) and for any
other rig whose bones Auto-map can read (an animation-only armature from Mixamo, another model...).
"""
import json

import bpy
from bpy_extras import anim_utils
from mathutils import Matrix, Quaternion

from . import fit_skeleton


def darktide_armature(collection):
    """The asset's Darktide armature: the fitted one or the reference skeleton."""
    rigs = [o for o in collection.all_objects if o.type == "ARMATURE" and "darktide_reference_unit" in o]
    if not rigs:
        raise ValueError("The asset has no Darktide armature yet: fit the model, or import the reference skeleton")
    return rigs[0]


def _rows(identity, source, target):
    """{source bone: Darktide bone} for the fit maths, and {Darktide bone: source bone} for every bone to drive."""
    if target.get("darktide_fit_source") == source.name:
        rows = {r.source_bone: r.target_bone for r in identity.bone_map if r.target_bone}
        carried = json.loads(target.get("darktide_fit_bones", "{}"))
        return rows, {new: old for new, old in carried.items() if new in target.data.bones and old in source.data.bones}
    ordered, listing = fit_skeleton._armature_bone_list(source)
    names = [b.name for b in target.data.bones]
    assigned = fit_skeleton.auto_map_bones(listing, names, [tuple(b.head_local) for b in ordered])
    rows = {ordered[i].name: t for i, t in assigned.items()}
    return rows, {t: s for s, t in rows.items() if t in target.data.bones}


def _rotation(matrix):
    return matrix.to_3x3().normalized()


def retarget(context, collection, source, frame_start=None, frame_end=None):
    """Bake source's current animation onto the Darktide armature as a new action. Returns (action, report)."""
    identity = collection.dt_asset_identity
    target = darktide_armature(collection)
    if source == target or source.type != "ARMATURE":
        raise ValueError("Pick the armature that has the animation (not the Darktide one)")
    action = source.animation_data.action if source.animation_data else None
    if action is None:
        raise ValueError(source.name + " has no animation (no action assigned)")
    rows, drive = _rows(identity, source, target)
    plan = fit_skeleton.fit_plan(identity, source, rows)
    start, end = action.frame_range if frame_start is None else (frame_start, frame_end)
    frames = list(range(int(round(start)), int(round(end)) + 1))

    scene = context.scene
    # a fitted-from rig is hidden and out of the asset collection: it has to be evaluated for its pose
    linked = source.name not in context.view_layer.objects
    if linked:
        scene.collection.objects.link(source)
    hidden = source.hide_viewport
    source.hide_viewport = False
    previous_frame = scene.frame_current

    tbones = target.data.bones
    order = sorted(tbones, key=lambda b: len(b.parent_recursive))
    rest = {b.name: b.matrix_local.copy() for b in tbones}
    relative = {b.name: (rest[b.parent.name].inverted() @ rest[b.name]) if b.parent else rest[b.name] for b in tbones}
    source_rest = {b.name: _rotation(source.matrix_world @ b.matrix_local) for b in source.data.bones}
    turn = {t: plan.rotation(s) for t, s in drive.items()}
    hips_source = plan.src_of["j_hips"]
    moves = "j_hips" in drive and drive["j_hips"] == hips_source
    keys = {name: [] for name in drive}
    locations = []
    try:
        for frame in frames:
            scene.frame_set(frame)
            world = source.matrix_world
            posed = {}
            for bone in order:
                name = bone.name
                parent = posed[bone.parent.name] if bone.parent else Matrix.Identity(4)
                inherited = parent @ relative[name]
                if name not in drive:
                    posed[name] = inherited
                    continue
                src = drive[name]
                delta = _rotation(world @ source.pose.bones[src].matrix) @ source_rest[src].transposed()
                c = turn[name]
                rotation = c @ delta @ c.transposed() @ _rotation(rest[name])
                location = inherited.translation
                if name == "j_hips" and moves:
                    location = plan.place((world @ source.pose.bones[src].matrix).translation)
                posed[name] = Matrix.LocRotScale(location, rotation, None)
            for name in drive:
                bone = tbones[name]
                parent = posed[bone.parent.name] if bone.parent else Matrix.Identity(4)
                basis = relative[name].inverted() @ parent.inverted() @ posed[name]
                quat = basis.to_quaternion()
                if keys[name] and quat.dot(Quaternion(keys[name][-1])) < 0.0:
                    quat.negate()
                keys[name].append(tuple(quat))
                if name == "j_hips" and moves:
                    locations.append(tuple(basis.translation))
    finally:
        scene.frame_set(previous_frame)
        source.hide_viewport = hidden
        if linked:
            scene.collection.objects.unlink(source)

    baked = bpy.data.actions.new(action.name + " (Darktide)")
    slot = baked.slots.new(id_type="OBJECT", name=target.name)
    bag = anim_utils.action_ensure_channelbag_for_slot(baked, slot)

    def curve(path, index, values, group):
        fc = bag.fcurves.new(path, index=index, group_name=group)
        fc.keyframe_points.add(len(frames))
        fc.keyframe_points.foreach_set("co", [v for pair in zip(frames, values) for v in pair])
        fc.keyframe_points.foreach_set("interpolation", [1] * len(frames))  # LINEAR, every frame is a key
        fc.update()

    for name, quats in keys.items():
        target.pose.bones[name].rotation_mode = "QUATERNION"
        for i in range(4):
            curve('pose.bones["%s"].rotation_quaternion' % name, i, [q[i] for q in quats], name)
    if locations:
        for i in range(3):
            curve('pose.bones["j_hips"].location', i, [p[i] for p in locations], "j_hips")
    if target.animation_data is None:
        target.animation_data_create()
    target.animation_data.action = baked
    target.animation_data.action_slot = slot
    report = "Carried %d frames of %s onto %d Darktide bones" % (len(frames), action.name, len(keys))
    return baked, report


class DARKTIDE_OT_retarget_animation(bpy.types.Operator):
    bl_idname = "darktide.retarget_animation"
    bl_label = "Carry Over Animation"
    bl_description = ("Bake the picked armature's current animation onto the Darktide armature as a new action "
                      "(every frame keyed). Empty pick = the rig the model was fitted from")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection first")
            return {"CANCELLED"}
        identity = collection.dt_asset_identity
        source = identity.animation_source
        try:
            if source is None:
                name = darktide_armature(collection).get("darktide_fit_source")
                source = bpy.data.objects.get(name) if name else None
                if source is None:
                    raise ValueError("Pick the armature with the animation")
            if context.object and context.object.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
            _action, text = retarget(context, collection, source)
        except (ValueError, RuntimeError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        self.report({"INFO"}, text)
        return {"FINISHED"}


def draw(layout, collection):
    identity = collection.dt_asset_identity
    box = layout.box()
    box.label(text="Carry Over Animation")
    box.prop(identity, "animation_source", text="From")
    box.operator(DARKTIDE_OT_retarget_animation.bl_idname, icon="ARMATURE_DATA")


CLASSES = (DARKTIDE_OT_retarget_animation,)
