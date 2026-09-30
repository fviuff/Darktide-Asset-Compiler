"""Blender authoring and compilation workflow for Darktide assets."""

import os
import shutil
import sys
import json
import subprocess
import uuid
import copy
import re
import math
import struct

import bpy
import bmesh
from mathutils import Matrix, Vector
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty
from .reference_skeleton import ReferenceSkeletonError, load_reference, _murmur64
from . import fit_skeleton


SCHEMA_KEY = "darktide_asset"
MATERIAL_SCHEMA_KEY = "darktide_material"
ACTOR_ITEMS = [
    ("static", "Fixed collider", "Stays fixed and provides collision geometry"),
    ("dynamic", "Simulated body", "PhysX simulates this rigid body when loaded into a physics scene"),
    ("keyframed", "Moved externally", "Kinematic body; a runtime controller must supply motion"),
]

bl_info = {
    "name": "Darktide Assets",
    "author": "fviuff",
    "version": (0, 7, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > Darktide",
    "category": "Import-Export",
}


def _enum(items, default):
    return EnumProperty(items=[(v, v.title(), "") for v in items], default=default)


class DarktideAnimationState(bpy.types.PropertyGroup):
    state_name: StringProperty(name="State name", default="State")
    clip_index: IntProperty(name="Clip index", default=0, min=0)
    playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat this clip"),
        ("once", "Play once", "Play this clip without looping"),
    ], default="loop")


class DarktideAnimationVariable(bpy.types.PropertyGroup):
    variable_name: StringProperty(name="Variable name", default="Variable")
    initial_value: FloatProperty(name="Initial value", default=0.0)
    minimum: FloatProperty(name="Minimum", default=0.0)
    maximum: FloatProperty(name="Maximum", default=1.0)


class DarktideAnimationTransition(bpy.types.PropertyGroup):
    from_state: IntProperty(name="From state index", default=0, min=0)
    to_state: IntProperty(name="To state index", default=0, min=0)
    event_name: StringProperty(name="Event name")
    blend_seconds: FloatProperty(name="Blend seconds", default=0.0, min=0.0)
    condition: EnumProperty(name="Condition", items=[
        ("always", "Always on event", "Take this transition when the named event arrives"),
        ("range", "Variable range", "Choose this transition when a float variable falls inside its range"),
    ], default="always")
    variable_index: IntProperty(name="Variable index", default=0, min=0)
    lower: FloatProperty(name="Lower bound", default=0.0)
    upper: FloatProperty(name="Upper bound", default=1.0)
    lower_exclusive: BoolProperty(name="Exclude lower bound", default=False)
    upper_exclusive: BoolProperty(name="Exclude upper bound", default=False)


SKELETON_PRESETS = {
    "human": "content/characters/player/human/third_person/base",
    "ogryn": "content/characters/player/ogryn/third_person/base",
    "traitor_guard": "content/characters/enemy/chaos_traitor_guard/third_person/base",
    "flamer": "content/characters/enemy/chaos_traitor_guard/third_person/flamer_base",
}


def apply_skeleton_preset(identity, folder, key):
    """Fill the reference UNIT/BONES/resource fields from a limn-extracted folder (<16hex>.<ext> files)."""
    resource = SKELETON_PRESETS[key]
    folder = bpy.path.abspath(folder.strip()) if folder else ""
    if not folder or not os.path.isdir(folder):
        raise ValueError("Set 'Game extract folder' to a directory of extracted <hash>.unit/.bones files")
    stem = "%016x" % _murmur64(resource)
    unit = os.path.join(folder, stem + ".unit")
    bones = os.path.join(folder, stem + ".bones")
    missing = [path for path in (unit, bones) if not os.path.isfile(path)]
    if missing:
        raise ValueError("Preset " + resource + " needs " + ", ".join(missing) + " (not found in the extract folder)")
    identity.reference_unit = unit
    identity.reference_bones = bones
    identity.skeleton_resource = resource


def _skeleton_preset_changed(self, context):
    self.skeleton_preset_status = ""
    if self.skeleton_preset == "custom":
        return
    try:
        apply_skeleton_preset(self, context.scene.dt_asset.game_extract_folder, self.skeleton_preset)
        fit_skeleton.refresh_reference_names(self)
    except ValueError as exc:
        self.skeleton_preset_status = str(exc)


class DarktideSceneSettings(bpy.types.PropertyGroup):
    game_extract_folder: StringProperty(name="Game extract folder", subtype="DIR_PATH",
                                        description="Directory of limn-extracted <16hex>.<ext> files, used by skeleton presets")
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (retail-gear style subset skeleton)",
        default=False,
    )
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=2.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    compiler: StringProperty(name="Compiler", subtype="FILE_PATH")
    output_path: StringProperty(name="Output path", subtype="DIR_PATH")
    asset_filename: StringProperty(name="Asset filename", default="asset")
    asset_path: StringProperty(name="Resource path", description="Stable extensionless identity, e.g. content/mods/my_mod/trolley; blank uses the GLB filename")
    scale: FloatProperty(name="Scale", default=1.0, min=0.000001)
    asset_collection: PointerProperty(name="Asset collection", type=bpy.types.Collection)
    visible_meshes_collide: BoolProperty(
        name="Visible meshes collide",
        description="Use visible mesh geometry as render-and-collision shapes in this UNIT; does not create a ground plane or drive animation",
        default=False,
    )
    output_kind: EnumProperty(name="Output kind", default="all", items=[
        ("all", "All", "Compile the model and animations"),
        ("model", "Model", "Compile model resources"),
        ("animations", "Animations", "Compile animation resources"),
    ])
    select_clip: BoolProperty(name="Select clip", default=False)
    clip_index: IntProperty(name="Zero-based clip index", default=0, min=0)
    embed_simple_animation: BoolProperty(
        name="Simple animation",
        description="Without a state machine, embed one clip in the UNIT so scripts can play it with Unit.play_simple_animation",
        default=True,
    )
    simple_animation_clip: IntProperty(name="Simple animation clip", default=0, min=0)
    auto_loop_single_clip: BoolProperty(
        name="Auto-loop single-clip assets",
        description="Create a looping state machine when the exported GLB contains exactly one animation clip",
        default=True,
    )
    create_looping_state_machine: BoolProperty(
        name="Manual single-clip state machine",
        description="Create a one-state controller for the selected clip; automatic state retirement is not provided",
        default=False,
    )
    loop_clip_index: IntProperty(name="State clip index", default=0, min=0)
    state_machine_playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat the selected clip"),
        ("once", "Play once", "Play the selected clip without looping"),
    ], default="loop")
    create_state_graph: BoolProperty(
        name="Create event-driven state graph",
        description="Create a multi-state controller whose transitions are triggered by named runtime events",
        default=False,
    )
    state_graph_states: CollectionProperty(type=DarktideAnimationState)
    state_graph_variables: CollectionProperty(type=DarktideAnimationVariable)
    state_graph_transitions: CollectionProperty(type=DarktideAnimationTransition)
    animation_translation_tolerance: FloatProperty(name="Translation tolerance", default=1e-4, min=1e-12, precision=8)
    animation_scale_tolerance: FloatProperty(name="Scale tolerance", default=1e-4, min=1e-12, precision=8)
    animation_rotation_tolerance_radians: FloatProperty(name="Rotation tolerance (radians)", default=1e-3, min=1e-12, precision=8)
    animation_fit_advanced: BoolProperty(name="Show animation fit controls", default=False)
    compile_timeout: IntProperty(name="Compile timeout (seconds)", description="0 waits without a time limit", default=600, min=0)
    inspection_summary: StringProperty(name="Last source inspection", default="")


class DarktideCollectionSettings(bpy.types.PropertyGroup):
    asset_path: StringProperty(name="Resource path", description="Stable extensionless identity for this asset")
    reference_unit: StringProperty(name="Reference UNIT", subtype="FILE_PATH")
    reference_bones: StringProperty(name="Reference BONES", subtype="FILE_PATH")
    skeleton_resource: StringProperty(name="Skeleton resource", description="Extensionless native skeleton resource identity")
    skeleton_preset: EnumProperty(name="Preset", default="custom", update=_skeleton_preset_changed, items=[
        ("custom", "Custom", "Enter the reference paths manually"),
        ("human", "Human", SKELETON_PRESETS["human"]),
        ("ogryn", "Ogryn player", SKELETON_PRESETS["ogryn"]),
        ("traitor_guard", "Traitor guard", SKELETON_PRESETS["traitor_guard"]),
        ("flamer", "Flamer", SKELETON_PRESETS["flamer"]),
    ])
    skeleton_preset_status: StringProperty(name="Preset status", default="")
    bone_map: CollectionProperty(type=fit_skeleton.DarktideBoneMapRow)
    bone_map_index: IntProperty(default=0)
    reference_bone_names: CollectionProperty(type=fit_skeleton.DarktideReferenceBoneName)
    fit_report: StringProperty(name="Last fit report", default="")
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (retail-gear style subset skeleton)",
        default=False,
    )
    owned_by_addon: BoolProperty(name="Darktide asset collection", default=False, options={"HIDDEN"})
    settings_version: IntProperty(default=0, options={"HIDDEN"})
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=2.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    visible_meshes_collide: BoolProperty(
        name="Visible meshes collide",
        description="Use this asset's visible mesh geometry as a collider",
        default=False,
    )
    asset_filename: StringProperty(name="Asset filename", default="asset")
    scale: FloatProperty(name="Scale", default=1.0, min=0.000001)
    output_kind: EnumProperty(name="Output kind", default="all", items=[
        ("all", "All", "Compile the model and animations"),
        ("model", "Model", "Compile model resources"),
        ("animations", "Animations", "Compile animation resources"),
    ])
    select_clip: BoolProperty(name="Select clip", default=False)
    clip_index: IntProperty(name="Zero-based clip index", default=0, min=0)
    embed_simple_animation: BoolProperty(
        name="Simple animation",
        description="Without a state machine, embed one clip in the UNIT so scripts can play it with Unit.play_simple_animation",
        default=True,
    )
    simple_animation_clip: IntProperty(name="Simple animation clip", default=0, min=0)
    auto_loop_single_clip: BoolProperty(
        name="Auto-loop single-clip assets",
        description="Create a looping state machine when the exported GLB contains exactly one animation clip",
        default=True,
    )
    create_looping_state_machine: BoolProperty(
        name="Manual single-clip state machine",
        description="Create a one-state controller for the selected clip; automatic state retirement is not provided",
        default=False,
    )
    loop_clip_index: IntProperty(name="State clip index", default=0, min=0)
    state_machine_playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat the selected clip"),
        ("once", "Play once", "Play the selected clip without looping"),
    ], default="loop")
    create_state_graph: BoolProperty(
        name="Create event-driven state graph",
        description="Create a multi-state controller whose transitions are triggered by named runtime events",
        default=False,
    )
    state_graph_states: CollectionProperty(type=DarktideAnimationState)
    state_graph_variables: CollectionProperty(type=DarktideAnimationVariable)
    state_graph_transitions: CollectionProperty(type=DarktideAnimationTransition)
    animation_translation_tolerance: FloatProperty(name="Translation tolerance", description="Maximum measured translation error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_scale_tolerance: FloatProperty(name="Scale tolerance", description="Maximum measured scale error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_rotation_tolerance_radians: FloatProperty(name="Rotation tolerance (radians)", description="Maximum measured angular error for fitted animation", default=1e-3, min=1e-12, precision=8)
    animation_fit_advanced: BoolProperty(name="Show animation fit controls", default=False)
    inspection_summary: StringProperty(name="Last source inspection", default="")


class DarktideMaterialSettings(bpy.types.PropertyGroup):
    mode: EnumProperty(name="Material intent", default="generated", items=[
        ("generated", "Generated from glTF PBR", "Build a Darktide material from this material's glTF PBR values"),
        ("external", "Use external material", "Bind an existing native Darktide material resource"),
        ("donor", "Experimental world material donor", "Preserve a v61 world surface blend material; UNIT renderer pairing awaits game validation"),
    ])
    resource: StringProperty(name="Material resource", description="Native material resource identity, e.g. content/mods/my_mod/materials/paint")
    donor_stream: StringProperty(name="Donor stream", description="Native v61 .streamdata or .stream material file", subtype="FILE_PATH")
    override_nm_r_blend: BoolProperty(name="Override nm_r_blend", default=False)
    nm_r_blend: FloatProperty(name="nm_r_blend", default=0.0)
    override_shared_blend: BoolProperty(name="Override shared_blend", default=False)
    shared_blend: FloatProperty(name="shared_blend", default=0.0)
    override_bc_blend: BoolProperty(name="Override bc_blend", default=False)
    bc_blend: FloatProperty(name="bc_blend", default=0.0)
    surface: EnumProperty(name="Surface response", default="default", items=[
        ("default", "Default", "Default material surface context"),
        ("metal_solid", "Solid metal", "Solid metal surface context"),
        ("metal_sheet", "Sheet metal", "Sheet metal surface context"),
        ("cloth", "Cloth", "Cloth surface context"),
        ("concrete", "Concrete", "Concrete surface context"),
        ("brick", "Brick", "Brick surface context"),
        ("bone", "Bone", "Bone surface context"),
        ("plastic", "Plastic", "Plastic surface context"),
    ])


class DarktideColliderSettings(bpy.types.PropertyGroup):
    stable_id: StringProperty(name="Stable ID")
    is_body: BoolProperty(name="Body", default=False)
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=1.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    body: PointerProperty(name="Body", type=bpy.types.Object)
    joint_kind: EnumProperty(name="Joint", default="none", items=[
        ("none", "None", ""), ("fixed", "Fixed", ""), ("hinge", "Hinge", ""), ("ragdoll", "Ragdoll", ""),
    ])
    body_a: PointerProperty(name="Body A", type=bpy.types.Object)
    body_b: PointerProperty(name="Body B", type=bpy.types.Object)
    hinge_limits: BoolProperty(name="Limit twist", default=False)
    twist_min: FloatProperty(name="Twist minimum", default=-3.14159265, subtype="ANGLE")
    twist_max: FloatProperty(name="Twist maximum", default=3.14159265, subtype="ANGLE")
    swing_y: FloatProperty(name="Swing Y", default=0.78539816, subtype="ANGLE", min=0.0)
    swing_z: FloatProperty(name="Swing Z", default=0.78539816, subtype="ANGLE", min=0.0)
    shape: EnumProperty(name="Collision geometry", default="geometry", items=[
        ("geometry", "Mesh surface", "Use this mesh's triangle surface for static/keyframed bodies"),
        ("convex", "Convex mesh", "Use a convex hull of this authored mesh for a simulated body"),
    ])
    role: _enum(("render", "collision", "both"), "render")
    use_asset_collision: BoolProperty(
        name="Use as asset collision",
        description="When the asset uses visible mesh collision, include this mesh as a collider",
        default=True,
    )


def _new_id():
    return str(uuid.uuid4())


def _reference_armature(context, collection, unit_path, bones_path, resource_name):
    nodes, scene_node_count = load_reference(unit_path, bones_path, resource_name)
    armature = bpy.data.armatures.new("Darktide Reference Skeleton")
    obj = bpy.data.objects.new("Darktide Reference Skeleton", armature)
    collection.objects.link(obj)
    previous_active = context.view_layer.objects.active
    previous_selected = list(context.selected_objects)
    previous_mode = previous_active.mode if previous_active else "OBJECT"
    try:
        if previous_mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        edit_bones = armature.edit_bones
        by_name = {}
        world_matrices = {}
        for node in nodes:
            values = node["world"]
            world = Matrix(tuple(tuple(values[row + column * 4] for column in range(4))
                                 for row in range(4)))
            location = Vector((values[12], values[13], values[14]))
            by_name[node["name"]] = edit_bones.new(node["name"])
            world_matrices[node["name"]] = (world, location)
        children = {node["name"]: [] for node in nodes}
        for node in nodes:
            if node["parent"]:
                children[node["parent"]].append(node["name"])
        for node in nodes:
            name = node["name"]
            bone = by_name[name]
            world, head = world_matrices[name]
            child_positions = [world_matrices[child][1] for child in children[name]]
            length = min(((point - head).length for point in child_positions if (point - head).length > 1e-5),
                         default=0.03)
            basis = world.to_3x3().normalized()
            bone.head = head
            bone.tail = head + (basis @ Vector((0.0, 0.0, length)))
            bone.align_roll(basis @ Vector((0.0, -1.0, 0.0)))
            if node["parent"]:
                bone.parent = by_name[node["parent"]]
                bone.use_connect = False
        bpy.ops.object.mode_set(mode="OBJECT")
    except Exception:
        if obj.mode == "EDIT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.armatures.remove(armature)
        raise
    finally:
        bpy.ops.object.select_all(action="DESELECT")
        for selected in previous_selected:
            if selected.name in context.view_layer.objects:
                selected.select_set(True)
        context.view_layer.objects.active = previous_active
        if previous_active and previous_mode != "OBJECT":
            bpy.ops.object.mode_set(mode=previous_mode)
    obj["darktide_reference_unit"] = bpy.path.abspath(unit_path)
    obj["darktide_reference_bones"] = bpy.path.abspath(bones_path)
    obj["darktide_scene_node_count"] = scene_node_count
    return obj


_COLLECTION_OPTIONS = ("actor", "mass", "material", "visible_meshes_collide",
                       "asset_filename", "scale", "output_kind", "select_clip", "clip_index",
                       "embed_simple_animation", "simple_animation_clip", "auto_loop_single_clip",
                       "create_looping_state_machine", "loop_clip_index", "state_machine_playback",
                       "create_state_graph",
                       "animation_translation_tolerance", "animation_scale_tolerance",
                       "animation_rotation_tolerance_radians", "animation_fit_advanced",
                       "only_weighted_bones")


def _copy_state_graph(source, target):
    rows = (
        ("state_graph_states", ("state_name", "clip_index", "playback")),
        ("state_graph_variables", ("variable_name", "initial_value", "minimum", "maximum")),
        ("state_graph_transitions", ("from_state", "to_state", "event_name", "blend_seconds",
                                     "condition", "variable_index", "lower", "upper",
                                     "lower_exclusive", "upper_exclusive")),
    )
    for collection_name, fields in rows:
        source_rows = getattr(source, collection_name)
        target_rows = getattr(target, collection_name)
        if target_rows:
            continue
        for source_row in source_rows:
            target_row = target_rows.add()
            for field in fields:
                setattr(target_row, field, getattr(source_row, field))


def _asset_options(scene):
    """Keep authoring choices with the collection when a .blend holds many assets."""
    scene_settings = scene.dt_asset
    collection = scene_settings.asset_collection
    if collection is None:
        return scene_settings
    options = collection.dt_asset_identity
    if options.settings_version == 0:
        for name in _COLLECTION_OPTIONS:
            setattr(options, name, getattr(scene_settings, name))
        _copy_state_graph(scene_settings, options)
        options.settings_version = 2
    elif options.settings_version < 2:
        options.auto_loop_single_clip = scene_settings.auto_loop_single_clip
        options.settings_version = 2
    return options


def _glb_animations(path):
    """Return animation names from the JSON chunk of the exported GLB."""
    with open(path, "rb") as glb:
        header = glb.read(12)
        if len(header) != 12:
            raise ValueError("Exported GLB has an incomplete header")
        magic, version, total_length = struct.unpack("<4sII", header)
        if magic != b"glTF" or version != 2 or total_length < 20:
            raise ValueError("Exported file is not a valid GLB 2.0")
        while glb.tell() + 8 <= total_length:
            chunk_length, chunk_type = struct.unpack("<II", glb.read(8))
            chunk = glb.read(chunk_length)
            if len(chunk) != chunk_length:
                raise ValueError("Exported GLB has an incomplete chunk")
            if chunk_type == 0x4E4F534A:
                document = json.loads(chunk.decode("utf-8"))
                return [animation.get("name", "") for animation in document.get("animations", [])]
        raise ValueError("Exported GLB does not contain a JSON chunk")


def _validate_state_graph(options):
    states = options.state_graph_states
    if not states:
        raise ValueError("Add at least one state to the event-driven state graph")
    names = [state.state_name.strip() for state in states]
    if any(not name for name in names):
        raise ValueError("Every state needs a name")
    if len(set(names)) != len(names):
        raise ValueError("State names must be unique")
    clip_count = re.search(r"Clips \((\d+)\)", options.inspection_summary)
    if clip_count:
        count = int(clip_count.group(1))
        for index, state in enumerate(states):
            if state.clip_index >= count:
                raise ValueError("State " + str(index) + " clip index must be below " + str(count))
    variables = options.state_graph_variables
    variable_names = [variable.variable_name.strip() for variable in variables]
    if any(not name for name in variable_names) or len(set(variable_names)) != len(variable_names):
        raise ValueError("State graph variable names must be unique and nonempty")
    for index, variable in enumerate(variables):
        if (not all(math.isfinite(value) for value in
                    (variable.initial_value, variable.minimum, variable.maximum)) or
                variable.minimum > variable.maximum or
                not variable.minimum <= variable.initial_value <= variable.maximum):
            raise ValueError("Variable " + str(index) + " needs finite ordered bounds containing its initial value")
    event_bindings = {}
    for index, transition in enumerate(options.state_graph_transitions):
        if transition.from_state >= len(states) or transition.to_state >= len(states):
            raise ValueError("Transition " + str(index) + " refers to a missing state")
        if not transition.event_name.strip():
            raise ValueError("Every transition needs an event name")
        if not math.isfinite(transition.blend_seconds) or transition.blend_seconds < 0:
            raise ValueError("Transition blend seconds must be a finite nonnegative value")
        variable_index = transition.variable_index if transition.condition == "range" else None
        key = (transition.from_state, transition.event_name.strip())
        if key in event_bindings and (event_bindings[key] != variable_index or variable_index is None):
            raise ValueError("State/event " + str(key) + " must use one variable selector or one direct transition")
        event_bindings[key] = variable_index
        if variable_index is not None:
            if variable_index >= len(variables):
                raise ValueError("Transition " + str(index) + " refers to a missing variable")
            if (not math.isfinite(transition.lower) or not math.isfinite(transition.upper) or
                    transition.lower > transition.upper or
                    (transition.lower == transition.upper and
                     (transition.lower_exclusive or transition.upper_exclusive))):
                raise ValueError("Transition " + str(index) + " needs a nonempty finite range")


def _collider_role(obj, include_visible=False):
    role = obj.dt_collider.role
    if role == "render" and include_visible and obj.dt_collider.use_asset_collision:
        return "both"
    return role


def _collider_objects(objects, include_visible=False):
    return [o for o in objects if o.type == "MESH" and
            _collider_role(o, include_visible) != "render"]


def _authored_objects(objects, include_visible=False):
    return [o for o in objects if o.dt_collider.is_body or o.dt_collider.joint_kind != "none" or
            (o.type == "MESH" and _collider_role(o, include_visible) != "render")]


def _resolve_ids(objects, include_visible=False):
    """Ensure persistent IDs exist and make duplicates unique at export time."""
    seen = set()
    for obj in _authored_objects(objects, include_visible):
        stable_id = obj.dt_collider.stable_id.strip()
        if not stable_id or stable_id in seen:
            stable_id = _new_id()
        obj.dt_collider.stable_id = stable_id
        seen.add(stable_id)


def _scene_extras(scene):
    settings = _asset_options(scene)
    return {
        "version": 1,
        "body": {
            "id": "main",
            "actor": settings.actor,
            "mass": settings.mass,
            "material": settings.material,
        },
    }


def _nearest_body(obj):
    current = obj
    seen = set()
    while current and current not in seen:
        seen.add(current)
        if current.dt_collider.is_body:
            return current
        current = current.parent
    return None


def _v2_objects(objects):
    return any(o.dt_collider.is_body or o.dt_collider.joint_kind != "none" for o in objects)


def _validate_v2(objects, include_visible=False):
    objects = set(objects)
    bodies = [o for o in objects if o.dt_collider.is_body]
    for body in bodies:
        if body.dt_collider.actor == "dynamic" and body.dt_collider.mass <= 0:
            raise ValueError("Dynamic body mass must be positive: " + body.name)
        if not any(o.type == "MESH" and _collider_role(o, include_visible) != "render" and
                   (o.dt_collider.body or _nearest_body(o)) == body for o in objects):
            raise ValueError("Every version 2 body requires a collider")
    for obj in objects:
        settings = obj.dt_collider
        if obj.type == "MESH" and _collider_role(obj, include_visible) != "render":
            target = settings.body or _nearest_body(obj)
            if target not in objects or not target.dt_collider.is_body:
                raise ValueError("Collider must target a body in the active scene")
        if settings.joint_kind != "none":
            if settings.body_a not in objects or settings.body_b not in objects or not settings.body_a.dt_collider.is_body or not settings.body_b.dt_collider.is_body:
                raise ValueError("Joint endpoints must be bodies in the active scene")
            if settings.body_a == settings.body_b:
                raise ValueError("Joint endpoints must be distinct")
            if settings.body_a.dt_collider.actor != "dynamic" and settings.body_b.dt_collider.actor != "dynamic":
                raise ValueError("A joint needs at least one dynamic body")
            if settings.hinge_limits and settings.twist_min > settings.twist_max:
                raise ValueError("Twist minimum must not exceed maximum")


def _v2_object_extras(obj, include_visible=False):
    settings = obj.dt_collider
    result = {"version": 2, "id": settings.stable_id}
    if settings.is_body:
        result["body"] = {"actor": settings.actor, "mass": settings.mass, "material": settings.material}
    if obj.type == "MESH" and _collider_role(obj, include_visible) != "render":
        target = settings.body or _nearest_body(obj)
        result["collider"] = {"body": target.dt_collider.stable_id,
                              "shape": settings.shape, "role": _collider_role(obj, include_visible)}
    if settings.joint_kind != "none":
        joint = {"body_a": settings.body_a.dt_collider.stable_id, "body_b": settings.body_b.dt_collider.stable_id, "kind": settings.joint_kind}
        if settings.joint_kind == "hinge" and settings.hinge_limits:
            joint.update({"twist_min": settings.twist_min, "twist_max": settings.twist_max})
        if settings.joint_kind == "ragdoll":
            joint.update({"swing_y": settings.swing_y, "swing_z": settings.swing_z})
            if settings.hinge_limits:
                joint.update({"twist_min": settings.twist_min, "twist_max": settings.twist_max})
        result["joint"] = joint
    return result


def _object_extras(obj, include_visible=False):
    settings = obj.dt_collider
    role = "both" if include_visible and settings.role == "render" else settings.role
    return {
        "version": 1,
        "id": settings.stable_id,
        "collider": {"body": "main", "shape": settings.shape, "role": role},
    }


def _set_if_supported(kwargs, operator, name, value):
    if name in operator.get_rna_type().properties:
        kwargs[name] = value


def _set_required(kwargs, operator, name, value):
    if name not in operator.get_rna_type().properties:
        raise RuntimeError("Installed glTF exporter lacks required option: " + name)
    kwargs[name] = value


def _copy_idproperty(value):
    if hasattr(value, "to_dict"):
        return copy.deepcopy(value.to_dict())
    return copy.deepcopy(value)


def _asset_stem(value):
    stem = (value or "").strip() or "asset"
    reserved = {"CON", "PRN", "AUX", "NUL"} | {
        prefix + str(index) for prefix in ("COM", "LPT") for index in range(1, 10)}
    if (os.path.isabs(stem) or stem in {".", ".."} or
            any(char in '<>:"/\\|?*' or ord(char) < 32 for char in stem) or
            stem.endswith(".") or stem.split(".", 1)[0].upper() in reserved):
        raise ValueError("Asset filename must be a simple filename stem")
    return stem


def _suggest_stem(name):
    return re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_") or "asset"


def _compiler_path(settings):
    if not settings.compiler.strip():
        raise ValueError("Set a compiler path before compiling")
    compiler = bpy.path.abspath(settings.compiler)
    if not os.path.isfile(compiler):
        raise ValueError("Compiler executable not found: " + compiler)
    return compiler


def _wine_tools(compiler):
    """Return Wine and winepath executables for a Linux-hosted Windows compiler."""
    if not sys.platform.startswith("linux") or not os.path.basename(compiler).lower().endswith(".exe"):
        return None
    wine = shutil.which("wine") or shutil.which("wine64")
    if not wine:
        raise ValueError("This .exe compiler needs Wine on Linux; install wine or wine64 and retry")
    winepath = shutil.which("winepath") or shutil.which("winepath-stable")
    if not winepath:
        raise ValueError("This .exe compiler needs winepath on Linux; install winepath or winepath-stable and retry")
    return wine, winepath


def _windows_path(path, wine_tools):
    if wine_tools is None:
        return os.fspath(path)
    result = subprocess.run([wine_tools[1], "-w", os.fspath(path)],
                            capture_output=True, text=True, timeout=120, check=False)
    converted = (result.stdout or "").strip()
    if result.returncode or not converted:
        detail = (result.stderr or result.stdout or "winepath returned no path").strip()
        raise ValueError("Could not convert path for Wine: " + os.fspath(path) + "; " + detail[:140])
    return converted


def _run_compiler(compiler, arguments, wine_tools, **kwargs):
    command = ([wine_tools[0], compiler] if wine_tools else [compiler]) + arguments
    return subprocess.run(command, **kwargs)


def _inspection_summary(output):
    """Keep the compiler's clip indices visible beside the Blender clip control."""
    lines = output.splitlines()
    summary = []
    in_clips = False
    for line in lines:
        if line.startswith(("Active meshes:", "Skins (", "Clips (", "Authored physics:")):
            summary.append(line)
            in_clips = line.startswith("Clips (")
        elif in_clips and line.startswith("  ["):
            summary.append(line.strip())
        elif in_clips:
            in_clips = False
    return "\n".join(summary)


def _validate_material_resource(value, material_name):
    name = value
    if not name or name.startswith(("/", "\\")) or "\\" in name or ":" in name:
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    if any(ord(char) < 32 for char in name):
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    if any(part == ".." for part in name.split("/")):
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    return name


def _collection_materials(objects):
    materials = set()
    for obj in objects:
        if obj.type == "MESH" and obj.data and obj.dt_collider.role != "collision":
            materials.update(material for material in obj.data.materials if material)
    return sorted(materials, key=lambda material: (material.name, material.as_pointer()))


def _has_constant_emission_output(material):
    if not material.use_nodes or material.node_tree is None:
        return False
    outputs = [node for node in material.node_tree.nodes
               if node.type == "OUTPUT_MATERIAL" and node.is_active_output]
    if len(outputs) != 1:
        return False
    surface = outputs[0].inputs.get("Surface")
    if surface is None or len(surface.links) != 1:
        return False
    emission = surface.links[0].from_node
    if emission.type != "EMISSION" or surface.links[0].from_socket.name != "Emission":
        return False
    for input_name in ("Color", "Strength"):
        socket = emission.inputs.get(input_name)
        if socket is None or socket.is_linked:
            return False
    return True


def _apply_modifiers_for_export(objects):
    """Keep Blender's evaluated geometry, except where it would erase morphs."""
    morphs = [obj for obj in objects if obj.type == "MESH" and obj.data and
              obj.data.shape_keys and len(obj.data.shape_keys.key_blocks) > 1]
    if not morphs:
        return True
    modified = [obj.name for obj in objects if obj.type == "MESH" and
                any(mod.show_viewport and mod.type != "ARMATURE" for mod in obj.modifiers)]
    if modified:
        raise ValueError("Blender's glTF exporter cannot preserve both shape keys and evaluated modifiers in this asset: "
                         + ", ".join(modified) + ". Apply those modifiers to a separate mesh or export the meshes separately.")
    return False


def _collision_review(scene, objects):
    options = _asset_options(scene)
    version2 = _v2_objects(objects)
    messages = []
    for obj in objects:
        if obj.type != "MESH" or _collider_role(obj, options.visible_meshes_collide) == "render":
            continue
        body = (obj.dt_collider.body or _nearest_body(obj)) if version2 else None
        actor = body.dt_collider.actor if body else options.actor
        if actor == "dynamic" and obj.dt_collider.shape == "geometry":
            messages.append(obj.name + ": simulated bodies use a convex hull of this mesh; "
                            "use separate editable collision meshes for concave shapes")
    return messages


def _weighted_bone_sets(objects):
    """Map each armature to the names of bones with nonzero vertex weights plus all ancestors."""
    result = {}
    for obj in objects:
        if obj.type != "MESH":
            continue
        rigs = {mod.object for mod in obj.modifiers if mod.type == "ARMATURE" and mod.object is not None}
        if not rigs:
            continue
        group_names = {group.index: group.name for group in obj.vertex_groups}
        used = {group_names[g.group] for vertex in obj.data.vertices for g in vertex.groups
                if g.weight > 0.0 and g.group in group_names}
        for rig in rigs:
            keep = result.setdefault(rig, set())
            for name in used:
                bone = rig.data.bones.get(name)
                while bone is not None:
                    keep.add(bone.name)
                    bone = bone.parent
    return result


# glb is just the handoff to the compiler, nobody is supposed to open it
def export_asset(context, path, donor_path_converter=None):
    """Export the active asset collection to GLB with temporary schema extras.

    ``context`` must provide a Blender context whose ``scene`` is the asset
    scene. ``path`` is the complete output .glb path. Scene/object extras and
    all other authoring properties are restored even if export fails.
    """
    scene = context.scene
    collection = scene.dt_asset.asset_collection
    if collection is None:
        raise ValueError("Create an asset collection from the selection first")
    objects = list(collection.all_objects)
    if not objects:
        raise ValueError("The asset collection is empty")
    apply_modifiers = _apply_modifiers_for_export(objects)
    view_objects = context.view_layer.objects
    missing = [obj.name for obj in objects
               if view_objects.get(obj.name) != obj or not obj.visible_get(view_layer=context.view_layer)]
    if missing:
        raise ValueError(
            "Asset collection members are excluded from the active view layer: "
            + ", ".join(missing)
            + ". Include the asset collection in the active view layer before exporting."
        )
    version2 = _v2_objects(objects)
    include_visible = _asset_options(scene).visible_meshes_collide
    _resolve_ids(objects, include_visible)
    authored = set(_authored_objects(objects, include_visible))
    materials = _collection_materials(objects)
    for material in materials:
        if material.dt_material.mode == "external":
            _validate_material_resource(material.dt_material.resource, material.name)
        elif material.dt_material.mode == "donor":
            donor_path = os.path.abspath(bpy.path.abspath(material.dt_material.donor_stream)) if material.dt_material.donor_stream else ""
            if not donor_path or not os.path.isfile(donor_path):
                raise ValueError("Material " + material.name + " needs an existing native donor stream")
            if os.path.splitext(donor_path)[1].lower() not in (".streamdata", ".stream"):
                raise ValueError("Material " + material.name + " donor must be a .streamdata or .stream file")
            for enabled, value, key in (
                (material.dt_material.override_nm_r_blend, material.dt_material.nm_r_blend, "nm_r_blend"),
                (material.dt_material.override_shared_blend, material.dt_material.shared_blend, "shared_blend"),
                (material.dt_material.override_bc_blend, material.dt_material.bc_blend, "bc_blend"),
            ):
                if enabled and not math.isfinite(value):
                    raise ValueError("Material " + material.name + " override " + key + " must be finite")
    if version2:
        _validate_v2(objects, include_visible)
    had_scene_extra = SCHEMA_KEY in scene
    original_scene_extra = _copy_idproperty(scene[SCHEMA_KEY]) if had_scene_extra else None
    changed_objects = []
    changed_materials = []
    changed_bones = []
    original_selection = [obj for obj in view_objects if obj.select_get()]
    original_active = context.view_layer.objects.active
    try:
        for obj in original_selection:
            obj.select_set(False)
        for obj in objects:
            obj.select_set(True)
        unselected = [obj.name for obj in objects if not obj.select_get()]
        if unselected:
            raise ValueError(
                "Asset collection members cannot be selected for glTF export: "
                + ", ".join(unselected)
                + ". Make them selectable in the active view layer."
            )
        if objects:
            context.view_layer.objects.active = objects[0]
        scene.pop(SCHEMA_KEY, None)
        if not version2 and _collider_objects(objects, include_visible):
            scene[SCHEMA_KEY] = _scene_extras(scene)
        colliders = set(_collider_objects(objects, include_visible))
        for obj in objects:
            had_original = SCHEMA_KEY in obj
            original = _copy_idproperty(obj[SCHEMA_KEY]) if had_original else None
            changed_objects.append((obj, original, had_original))
            obj.pop(SCHEMA_KEY, None)
            if version2 and obj in authored:
                obj[SCHEMA_KEY] = _v2_object_extras(obj, include_visible)
            elif obj in colliders:
                obj[SCHEMA_KEY] = _object_extras(obj, include_visible)
        for material in materials:
            had_original = MATERIAL_SCHEMA_KEY in material
            original = _copy_idproperty(material[MATERIAL_SCHEMA_KEY]) if had_original else None
            changed_materials.append((material, original, had_original))
            material.pop(MATERIAL_SCHEMA_KEY, None)
            if material.dt_material.mode == "external":
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "external",
                    "resource": _validate_material_resource(material.dt_material.resource, material.name),
                }
            elif material.dt_material.mode == "donor":
                settings = material.dt_material
                overrides = {}
                for enabled, value, key in (
                    (settings.override_nm_r_blend, settings.nm_r_blend, "nm_r_blend"),
                    (settings.override_shared_blend, settings.shared_blend, "shared_blend"),
                    (settings.override_bc_blend, settings.bc_blend, "bc_blend"),
                ):
                    if enabled:
                        overrides[key] = value
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "donor",
                    "family": "world_surface_blend_v1",
                    "stream": (donor_path_converter(os.path.abspath(bpy.path.abspath(settings.donor_stream)))
                               if donor_path_converter else
                               os.path.abspath(bpy.path.abspath(settings.donor_stream))),
                    "variable_overrides": overrides,
                }
            elif _has_constant_emission_output(material):
                intent = {"version": 1, "mode": "emissive"}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                material[MATERIAL_SCHEMA_KEY] = intent
            else:
                intent = {"version": 1, "mode": "generated"}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                material[MATERIAL_SCHEMA_KEY] = intent
        kwargs = {"filepath": os.fspath(path), "export_format": "GLB"}
        operator = bpy.ops.export_scene.gltf
        if _asset_options(scene).only_weighted_bones:
            # Temporarily mark unweighted bones non-deform; the exporter then drops them.
            for rig, keep in _weighted_bone_sets(objects).items():
                for bone in rig.data.bones:
                    if bone.use_deform and bone.name not in keep:
                        changed_bones.append(bone)
                        bone.use_deform = False
            _set_required(kwargs, operator, "export_def_bones", True)
        _set_required(kwargs, operator, "use_active_scene", True)
        _set_required(kwargs, operator, "use_selection", True)
        _set_required(kwargs, operator, "export_extras", True)
        _set_required(kwargs, operator, "export_apply", apply_modifiers)
        _set_if_supported(kwargs, operator, "export_tangents", True)
        _set_if_supported(kwargs, operator, "export_all_influences", True)
        _set_if_supported(kwargs, operator, "will_save_settings", False)
        result = operator(**kwargs)
        if result != {"FINISHED"}:
            raise RuntimeError("glTF export did not finish: " + repr(result))
        return result
    finally:
        if not had_scene_extra:
            scene.pop(SCHEMA_KEY, None)
        else:
            scene[SCHEMA_KEY] = original_scene_extra
        for obj, original, had_original in changed_objects:
            if not had_original:
                obj.pop(SCHEMA_KEY, None)
            else:
                obj[SCHEMA_KEY] = original
        for bone in changed_bones:
            bone.use_deform = True
        for material, original, had_original in changed_materials:
            if not had_original:
                material.pop(MATERIAL_SCHEMA_KEY, None)
            else:
                material[MATERIAL_SCHEMA_KEY] = original
        for obj in view_objects:
            if obj.select_get():
                obj.select_set(False)
        for obj in original_selection:
            if obj.name in view_objects:
                obj.select_set(True)
        context.view_layer.objects.active = original_active


class DARKTIDE_OT_inspect(bpy.types.Operator):
    bl_idname = "darktide.inspect_asset"
    bl_label = "Inspect Asset"
    bl_description = "Export the collection and list exactly which meshes, skins, and numbered clips the compiler sees"

    def execute(self, context):
        settings = context.scene.dt_asset
        options = _asset_options(context.scene)
        try:
            compiler = _compiler_path(settings)
            wine_tools = _wine_tools(compiler)
            stem = _asset_stem(options.asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
            glb_path = os.path.join(output_dir, stem + ".glb")
            export_asset(context, glb_path,
                         donor_path_converter=(lambda path: _windows_path(path, wine_tools)) if wine_tools else None)
            collision_notes = _collision_review(context.scene, list(settings.asset_collection.all_objects))
            command_glb = _windows_path(glb_path, wine_tools)
            result = _run_compiler(compiler, [command_glb, "--inspect"], wine_tools,
                                    capture_output=True, text=True,
                                    timeout=settings.compile_timeout or None, check=False)
            inspection_path = os.path.join(output_dir, stem + ".inspect.txt")
            with open(inspection_path, "w", encoding="utf-8") as report:
                report.write(result.stdout or "")
                report.write(result.stderr or "")
                if collision_notes:
                    report.write("\nCollision review:\n" + "\n".join(collision_notes) + "\n")
            if result.returncode:
                options.inspection_summary = ""
                self.report({"ERROR"}, "Inspection failed; see " + inspection_path)
                return {"CANCELLED"}
            options.inspection_summary = _inspection_summary(result.stdout)
            if collision_notes:
                options.inspection_summary += "\nCollision review: " + collision_notes[0]
            self.report({"INFO"}, "Source inspected; see " + inspection_path)
            return {"FINISHED"}
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            options.inspection_summary = ""
            self.report({"ERROR"}, "Inspection failed: " + str(exc)[:180])
            return {"CANCELLED"}


class DARKTIDE_OT_build(bpy.types.Operator):
    bl_idname = "darktide.build_asset"
    bl_label = "Compile Asset"

    def execute(self, context):
        settings = context.scene.dt_asset
        collection = settings.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection from the selection first")
            return {"CANCELLED"}
        asset_path = collection.dt_asset_identity.asset_path.strip()
        options = _asset_options(context.scene)
        if options.create_looping_state_machine and options.create_state_graph:
            self.report({"ERROR"}, "Choose either the single-clip state machine or event-driven state graph")
            return {"CANCELLED"}
        if (options.create_looping_state_machine or options.create_state_graph) and options.output_kind != "all":
            self.report({"ERROR"}, "Create state machine requires Output kind All")
            return {"CANCELLED"}
        if options.create_state_graph:
            if options.select_clip:
                self.report({"ERROR"}, "Disable Select clip when using an event-driven state graph")
                return {"CANCELLED"}
            try:
                _validate_state_graph(options)
            except ValueError as exc:
                self.report({"ERROR"}, str(exc))
                return {"CANCELLED"}
        if bool(collection.dt_asset_identity.reference_bones.strip()) != bool(
                collection.dt_asset_identity.skeleton_resource.strip()):
            self.report({"ERROR"}, "Reference BONES and skeleton resource must both be set")
            return {"CANCELLED"}
        reference_bones = collection.dt_asset_identity.reference_bones.strip()
        if reference_bones and not os.path.isfile(bpy.path.abspath(reference_bones)):
            self.report({"ERROR"}, "Reference BONES file not found: " + bpy.path.abspath(reference_bones))
            return {"CANCELLED"}
        try:
            compiler = _compiler_path(settings)
            wine_tools = _wine_tools(compiler)
            stem = _asset_stem(options.asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
        except (OSError, ValueError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        glb_path = os.path.join(output_dir, stem + ".glb")
        try:
            export_asset(context, glb_path,
                         donor_path_converter=(lambda path: _windows_path(path, wine_tools)) if wine_tools else None)
        except Exception as exc:
            self.report({"ERROR"}, "GLB export failed: " + str(exc))
            return {"CANCELLED"}
        try:
            glb_animations = _glb_animations(glb_path)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            self.report({"ERROR"}, "Could not read exported GLB animations: " + str(exc)[:150])
            return {"CANCELLED"}
        collision_notes = _collision_review(context.scene, list(collection.all_objects))
        compiled_dir = os.path.join(output_dir, stem)
        log_path = os.path.join(compiled_dir, "build.log")
        try:
            os.makedirs(compiled_dir, exist_ok=True)
            command = [_windows_path(glb_path, wine_tools), "-o",
                       _windows_path(compiled_dir, wine_tools),
                       "--scale", str(options.scale), "--output-kind", options.output_kind,
                       "--animation-translation-tolerance", str(options.animation_translation_tolerance),
                       "--animation-scale-tolerance", str(options.animation_scale_tolerance),
                       "--animation-rotation-tolerance-radians", str(options.animation_rotation_tolerance_radians)]
            if options.select_clip and options.output_kind != "model":
                command.extend(["--clip", str(options.clip_index)])
            use_single_clip_machine = options.create_looping_state_machine or (
                options.auto_loop_single_clip and not options.create_state_graph
                and options.output_kind == "all" and len(glb_animations) == 1
            )
            if use_single_clip_machine:
                clip_index = options.loop_clip_index if options.create_looping_state_machine else 0
                playback = options.state_machine_playback if options.create_looping_state_machine else "loop"
                command.extend(["--once-clip" if playback == "once" else "--loop-clip",
                                str(clip_index)])
            uses_state_machine = use_single_clip_machine or options.create_state_graph
            if not options.embed_simple_animation:
                command.append("--no-simple-animation")
            elif not uses_state_machine and options.output_kind == "all" and options.simple_animation_clip:
                command.extend(["--simple-clip", str(options.simple_animation_clip)])
            if options.create_state_graph:
                for state in options.state_graph_states:
                    command.extend(["--sm-state", state.state_name.strip(), str(state.clip_index), state.playback])
                for variable in options.state_graph_variables:
                    command.extend(["--sm-variable", variable.variable_name.strip(),
                                    str(variable.initial_value), str(variable.minimum), str(variable.maximum)])
                for transition in options.state_graph_transitions:
                    if transition.condition == "range":
                        command.extend(["--sm-range-transition", str(transition.from_state),
                                        str(transition.to_state), transition.event_name.strip(),
                                        str(transition.blend_seconds), str(transition.variable_index),
                                        str(transition.lower), str(transition.upper),
                                        "exclusive" if transition.lower_exclusive else "inclusive",
                                        "exclusive" if transition.upper_exclusive else "inclusive"])
                    else:
                        command.extend(["--sm-transition", str(transition.from_state), str(transition.to_state),
                                        transition.event_name.strip(), str(transition.blend_seconds)])
            if asset_path:
                command.extend(["--asset-path", asset_path])
            if reference_bones:
                command.extend(["--reference-bones",
                                _windows_path(bpy.path.abspath(reference_bones), wine_tools),
                                "--skeleton-resource",
                                collection.dt_asset_identity.skeleton_resource.strip()])
            result = _run_compiler(compiler, command, wine_tools,
                                   capture_output=True, text=True,
                                    timeout=settings.compile_timeout or None, check=False)
            with open(log_path, "w", encoding="utf-8") as log:
                if collision_notes:
                    log.write("Collision review:\n" + "\n".join(collision_notes) + "\n\n")
                log.write(result.stdout or "")
                log.write(result.stderr or "")
            if result.returncode:
                lines = (result.stderr + "\n" + result.stdout).splitlines()
                first_error = next((line.strip()[2:].strip() for line in lines
                                    if line.lstrip().startswith("- ")), None)
                if not first_error:
                    first_error = next((line.strip() for line in lines if line.strip()),
                                       "Compiler exited with code " + str(result.returncode))
                self.report({"ERROR"}, first_error[:180] + "; see " + log_path)
                return {"CANCELLED"}
            manifest_path = os.path.join(compiled_dir, "compile_manifest.json")
            if os.path.isfile(manifest_path):
                with open(manifest_path, "r", encoding="utf-8") as manifest_file:
                    gaps = json.load(manifest_file).get("compiler_gaps", [])
                if gaps:
                    self.report({"WARNING"}, str(gaps[0])[:180] + "; see " + log_path)
            if collision_notes:
                self.report({"WARNING"}, collision_notes[0][:180] + "; see " + log_path)
            else:
                self.report({"INFO"}, "Asset built; see " + log_path)
            return {"FINISHED"}
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            with open(log_path, "w", encoding="utf-8") as log:
                if isinstance(exc, subprocess.TimeoutExpired):
                    for captured in (exc.stdout, exc.stderr):
                        if captured:
                            log.write(captured.decode("utf-8", errors="replace")
                                      if isinstance(captured, bytes) else captured)
                log.write("\n" + str(exc) + "\n")
            self.report({"ERROR"}, "Compiler failed: " + str(exc)[:150] + "; see " + log_path)
            return {"CANCELLED"}


class DARKTIDE_OT_import_reference_skeleton(bpy.types.Operator):
    bl_idname = "darktide.import_reference_skeleton"
    bl_label = "Import Rest-Pose Reference Skeleton"
    bl_description = "Create an armature from the selected native UNIT scene graph and BONES table"

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        identity = collection.dt_asset_identity
        unit_path = bpy.path.abspath(identity.reference_unit.strip())
        bones_path = bpy.path.abspath(identity.reference_bones.strip())
        if not unit_path or not bones_path or not identity.skeleton_resource.strip():
            self.report({"ERROR"}, "Choose a UNIT, matching BONES, and skeleton resource identity")
            return {"CANCELLED"}
        if not os.path.isfile(unit_path) or not os.path.isfile(bones_path):
            self.report({"ERROR"}, "Reference UNIT or BONES file was not found")
            return {"CANCELLED"}
        try:
            obj = _reference_armature(context, collection, unit_path, bones_path,
                                      identity.skeleton_resource.strip())
        except (OSError, ReferenceSkeletonError, RuntimeError, ValueError) as exc:
            self.report({"ERROR"}, "Reference skeleton import failed: " + str(exc)[:180])
            return {"CANCELLED"}
        self.report({"INFO"}, "Imported " + str(len(obj.data.bones)) + " BONES joints into " + collection.name)
        return {"FINISHED"}


def _selection_closure(context):
    selected = set(context.selected_objects)
    if not selected:
        raise ValueError("Select at least one object to define the asset")
    objects = set()
    pending = list(selected)
    while pending:
        obj = pending.pop()
        if obj in objects:
            continue
        objects.add(obj)
        pending.extend(obj.children)

    # Parent and physics links are dependencies, but traversing their children
    # would pull unrelated siblings into an asset selected by one child mesh.
    pending = list(objects)
    while pending:
        obj = pending.pop()
        settings = obj.dt_collider
        dependencies = [obj.parent, settings.body, settings.body_a, settings.body_b]
        for target in dependencies:
            if target is not None and target not in objects:
                objects.add(target)
                pending.append(target)
    return objects


def _replace_members(context, collection, objects):
    for child in tuple(collection.children):
        collection.children.unlink(child)
    for obj in tuple(collection.objects):
        collection.objects.unlink(obj)
    for obj in context.scene.objects:
        if obj in objects:
            collection.objects.link(obj)


def _convex_hull_mesh(mesh, name):
    """Make the simulated collision volume visible as editable Blender faces."""
    bm = bmesh.new()
    try:
        vertices = [bm.verts.new(vertex.co) for vertex in mesh.vertices]
        if len(vertices) < 4:
            raise ValueError("dynamic collision needs a volume with at least four vertices")
        result = bmesh.ops.convex_hull(bm, input=vertices, use_existing_faces=False)
        faces = [part for part in result["geom"] if isinstance(part, bmesh.types.BMFace)]
        if len(faces) < 4:
            raise ValueError("dynamic collision needs a non-planar convex volume")
        points = []
        indices = {}
        polygons = []
        for face in faces:
            polygon = []
            for vertex in face.verts:
                if vertex not in indices:
                    indices[vertex] = len(points)
                    points.append(tuple(vertex.co))
                polygon.append(indices[vertex])
            polygons.append(polygon)
        hull = bpy.data.meshes.new(name)
        hull.from_pydata(points, [], polygons)
        hull.update()
        return hull
    finally:
        bm.free()


class DARKTIDE_OT_create_asset_collection(bpy.types.Operator):
    bl_idname = "darktide.create_asset_collection"
    bl_label = "New Asset from Selection"
    bl_description = "Create a new asset collection from selected objects and their assembly"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            objects = _selection_closure(context) if context.selected_objects else set()
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        collection = _new_asset_collection(context, objects)
        self.report({"INFO"}, "Created " + collection.name + " with " + str(len(objects)) + " objects")
        return {"FINISHED"}


def _new_asset_collection(context, objects):
    settings = context.scene.dt_asset
    first_asset = settings.asset_collection is None
    name = context.active_object.name if context.active_object else "Darktide Asset"
    collection = bpy.data.collections.new(name + " Asset")
    context.scene.collection.children.link(collection)
    options = collection.dt_asset_identity
    options.owned_by_addon = True
    options.asset_path = settings.asset_path if first_asset else ""
    for field in _COLLECTION_OPTIONS:
        setattr(options, field, getattr(settings, field))
    if first_asset:
        _copy_state_graph(settings, options)
    else:
        options.asset_filename = _suggest_stem(name)
        options.visible_meshes_collide = False
        options.actor = "static"
        options.output_kind = "all"
        options.select_clip = False
        options.create_looping_state_machine = False
        options.create_state_graph = False
    options.settings_version = 2
    _replace_members(context, collection, objects)
    settings.asset_collection = collection
    return collection


class DARKTIDE_OT_update_asset_collection(bpy.types.Operator):
    bl_idname = "darktide.update_asset_collection"
    bl_label = "Replace Asset Members from Selection"
    bl_description = "Replace only this addon's current asset collection membership"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None or not collection.dt_asset_identity.owned_by_addon:
            self.report({"ERROR"}, "Select an asset collection created by this addon")
            return {"CANCELLED"}
        try:
            objects = _selection_closure(context)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _replace_members(context, collection, objects)
        self.report({"INFO"}, "Updated " + collection.name + " with " + str(len(objects)) + " objects")
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_state(bpy.types.Operator):
    bl_idname = "darktide.add_animation_state"
    bl_label = "Add State"

    def execute(self, context):
        states = _asset_options(context.scene).state_graph_states
        state = states.add()
        state.state_name = "State " + str(len(states) - 1)
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_state(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_state"
    bl_label = "Remove State"
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        states = options.state_graph_states
        if 0 <= self.index < len(states):
            transitions = options.state_graph_transitions
            for transition_index in reversed(range(len(transitions))):
                transition = transitions[transition_index]
                if transition.from_state == self.index or transition.to_state == self.index:
                    transitions.remove(transition_index)
                    continue
                if transition.from_state > self.index:
                    transition.from_state -= 1
                if transition.to_state > self.index:
                    transition.to_state -= 1
            states.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_variable(bpy.types.Operator):
    bl_idname = "darktide.add_animation_variable"
    bl_label = "Add Variable"

    def execute(self, context):
        variables = _asset_options(context.scene).state_graph_variables
        variable = variables.add()
        variable.variable_name = "Variable " + str(len(variables) - 1)
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_variable(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_variable"
    bl_label = "Remove Variable"
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        variables = options.state_graph_variables
        if 0 <= self.index < len(variables):
            transitions = options.state_graph_transitions
            for transition_index in reversed(range(len(transitions))):
                transition = transitions[transition_index]
                if transition.condition != "range":
                    continue
                if transition.variable_index == self.index:
                    transitions.remove(transition_index)
                elif transition.variable_index > self.index:
                    transition.variable_index -= 1
            variables.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_transition(bpy.types.Operator):
    bl_idname = "darktide.add_animation_transition"
    bl_label = "Add Transition"

    def execute(self, context):
        transitions = _asset_options(context.scene).state_graph_transitions
        transition = transitions.add()
        transition.from_state = 0
        transition.to_state = 1 if len(_asset_options(context.scene).state_graph_states) > 1 else 0
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_transition(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_transition"
    bl_label = "Remove Transition"
    index: IntProperty()

    def execute(self, context):
        transitions = _asset_options(context.scene).state_graph_transitions
        if 0 <= self.index < len(transitions):
            transitions.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_make_collision_proxy(bpy.types.Operator):
    bl_idname = "darktide.make_collision_proxy"
    bl_label = "Create Editable Collision Mesh"
    bl_description = "Create collision-only meshes from evaluated Blender geometry; simulated bodies get a visible convex hull"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Choose an asset collection first")
            return {"CANCELLED"}
        sources = [obj for obj in context.selected_objects
                   if obj.type == "MESH" and collection.all_objects.get(obj.name) == obj and
                   obj.dt_collider.role != "collision"]
        if not sources:
            self.report({"ERROR"}, "Select visible mesh objects in the active asset")
            return {"CANCELLED"}
        depsgraph = context.evaluated_depsgraph_get()
        prepared = []
        pending_mesh = None
        try:
            for source in sources:
                parent_body = source if source.dt_collider.is_body else (source.dt_collider.body or _nearest_body(source))
                dynamic = (parent_body.dt_collider.actor if parent_body else _asset_options(context.scene).actor) == "dynamic"
                evaluated = source.evaluated_get(depsgraph)
                mesh = bpy.data.meshes.new_from_object(
                    evaluated, preserve_all_data_layers=False, depsgraph=depsgraph)
                pending_mesh = mesh
                if mesh is None or not mesh.polygons:
                    raise ValueError(source.name + " has no evaluated mesh faces for collision")
                if dynamic:
                    hull = _convex_hull_mesh(mesh, source.name + "_collision")
                    bpy.data.meshes.remove(mesh)
                    mesh = hull
                    pending_mesh = mesh
                mesh.materials.clear()
                prepared.append((source, mesh, parent_body, dynamic))
                pending_mesh = None
        except (RuntimeError, ValueError) as exc:
            if pending_mesh is not None and pending_mesh.users == 0:
                bpy.data.meshes.remove(pending_mesh)
            for _, mesh, _, _ in prepared:
                bpy.data.meshes.remove(mesh)
            self.report({"ERROR"}, "Collision mesh creation failed: " + str(exc)[:160])
            return {"CANCELLED"}
        created = []
        for source, mesh, parent_body, dynamic in prepared:
            proxy = source.copy()
            proxy.data = mesh
            proxy.name = source.name + "_collision"
            proxy.animation_data_clear()
            for modifier in tuple(proxy.modifiers):
                proxy.modifiers.remove(modifier)
            for constraint in tuple(proxy.constraints):
                proxy.constraints.remove(constraint)
            collection.objects.link(proxy)
            world = source.evaluated_get(depsgraph).matrix_world.copy()
            proxy.parent = source if source.dt_collider.is_body else source.parent
            proxy.matrix_world = world
            proxy.dt_collider.stable_id = ""
            proxy.dt_collider.is_body = False
            proxy.dt_collider.joint_kind = "none"
            proxy.dt_collider.role = "collision"
            proxy.dt_collider.body = parent_body
            proxy.dt_collider.shape = "convex" if dynamic else "geometry"
            proxy.display_type = "WIRE"
            proxy.show_in_front = True
            proxy.hide_render = True
            source.dt_collider.role = "render"
            source.dt_collider.use_asset_collision = False
            created.append(proxy)
        for obj in context.selected_objects:
            obj.select_set(False)
        for proxy in created:
            proxy.select_set(True)
        context.view_layer.objects.active = created[0]
        self.report({"INFO"}, "Created " + str(len(created)) + " editable collision meshes")
        return {"FINISHED"}


class DARKTIDE_OT_export_intermediate(bpy.types.Operator):
    bl_idname = "darktide.export_intermediate"
    bl_label = "Export Intermediate GLB"

    def execute(self, context):
        settings = context.scene.dt_asset
        if not settings.asset_collection:
            self.report({"ERROR"}, "Create an asset collection from the selection first")
            return {"CANCELLED"}
        try:
            stem = _asset_stem(_asset_options(context.scene).asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
            export_asset(context, os.path.join(output_dir, stem + ".glb"))
        except Exception as exc:
            self.report({"ERROR"}, "GLB export failed: " + str(exc))
            return {"CANCELLED"}
        self.report({"INFO"}, "Intermediate GLB exported")
        return {"FINISHED"}


class DARKTIDE_PT_asset_panel(bpy.types.Panel):
    bl_label = "Darktide Asset"
    bl_idname = "DARKTIDE_PT_asset_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Darktide"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        settings = scene.dt_asset
        collection = settings.asset_collection
        options = _asset_options(scene)
        asset_objects = list(collection.all_objects) if collection else []
        version2 = _v2_objects(asset_objects) if asset_objects else False
        if not version2:
            body = layout.box()
            body.label(text="Asset collision")
            body.prop(options, "visible_meshes_collide", text="Use visible meshes as collision")
            if options.visible_meshes_collide or any(o.dt_collider.role != "render" for o in asset_objects):
                body.prop(options, "actor")
                if options.actor == "dynamic":
                    body.prop(options, "mass")
                body.prop(options, "material")
            body.label(text="Static uses the authored mesh surface")
        output = layout.box()
        output.label(text="Build")
        output.operator(DARKTIDE_OT_create_asset_collection.bl_idname)
        if collection and collection.dt_asset_identity.owned_by_addon:
            output.operator(DARKTIDE_OT_update_asset_collection.bl_idname)
        output.prop(settings, "asset_collection")
        if collection:
            output.prop(collection.dt_asset_identity, "asset_path")
        else:
            output.label(text="Create an asset collection before export or compile")
        output.prop(settings, "compiler")
        output.prop(settings, "output_path")
        output.prop(options, "asset_filename")
        output.prop(options, "output_kind")
        if options.output_kind != "model":
            output.prop(options, "select_clip")
            if options.select_clip:
                output.prop(options, "clip_index")
        simple_row = output.row(align=True)
        simple_row.enabled = options.output_kind == "all"
        simple_row.prop(options, "embed_simple_animation")
        if options.embed_simple_animation:
            simple_row.prop(options, "simple_animation_clip", text="Clip")
        loop_row = output.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "auto_loop_single_clip")
        loop_row = output.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "create_looping_state_machine")
        graph_row = output.row()
        graph_row.enabled = options.output_kind == "all"
        graph_row.prop(options, "create_state_graph")
        if options.output_kind != "all":
            output.label(text="Animation controllers require Output kind All", icon="ERROR")
        elif options.create_looping_state_machine and options.create_state_graph:
            output.label(text="Choose one animation controller", icon="ERROR")
        elif options.create_looping_state_machine:
            row = output.row(align=True)
            row.prop(options, "loop_clip_index", text="Clip")
            row.prop(options, "state_machine_playback", text="")
        elif options.auto_loop_single_clip and not options.create_state_graph:
            output.label(text="A looping controller is added only when the exported GLB has one clip")
        elif options.create_state_graph:
            output.label(text="State 0 is the initial state")
            states = options.state_graph_states
            for index, state in enumerate(states):
                state_box = output.box()
                row = state_box.row(align=True)
                row.label(text="State " + str(index))
                row.prop(state, "state_name", text="")
                remove = row.operator(DARKTIDE_OT_remove_animation_state.bl_idname, text="", icon="X")
                remove.index = index
                row = state_box.row(align=True)
                row.prop(state, "clip_index", text="Clip")
                row.prop(state, "playback", text="")
            output.operator(DARKTIDE_OT_add_animation_state.bl_idname, icon="ADD")
            output.label(text="Float variables", icon="DRIVER")
            for index, variable in enumerate(options.state_graph_variables):
                variable_box = output.box()
                row = variable_box.row(align=True)
                row.label(text="Variable " + str(index))
                row.prop(variable, "variable_name", text="")
                remove = row.operator(DARKTIDE_OT_remove_animation_variable.bl_idname,
                                      text="", icon="X")
                remove.index = index
                variable_box.prop(variable, "initial_value")
                row = variable_box.row(align=True)
                row.prop(variable, "minimum")
                row.prop(variable, "maximum")
            output.operator(DARKTIDE_OT_add_animation_variable.bl_idname, icon="ADD")
            output.label(text="Transitions", icon="DRIVER_TRANSFORM")
            for index, transition in enumerate(options.state_graph_transitions):
                transition_box = output.box()
                row = transition_box.row(align=True)
                from_name = (states[transition.from_state].state_name if transition.from_state < len(states)
                             else "Missing state")
                to_name = (states[transition.to_state].state_name if transition.to_state < len(states)
                           else "Missing state")
                row.label(text="From " + str(transition.from_state) + ": " + from_name)
                row.prop(transition, "from_state", text="Index")
                row = transition_box.row(align=True)
                row.label(text="To " + str(transition.to_state) + ": " + to_name)
                row.prop(transition, "to_state", text="Index")
                transition_box.prop(transition, "event_name")
                transition_box.prop(transition, "condition")
                if transition.condition == "range":
                    row = transition_box.row(align=True)
                    row.prop(transition, "variable_index")
                    variable_index = transition.variable_index
                    variable_name = (options.state_graph_variables[variable_index].variable_name
                                     if variable_index < len(options.state_graph_variables) else "Missing variable")
                    row.label(text=variable_name)
                    row = transition_box.row(align=True)
                    row.prop(transition, "lower")
                    row.prop(transition, "upper")
                    row = transition_box.row(align=True)
                    row.prop(transition, "lower_exclusive")
                    row.prop(transition, "upper_exclusive")
                row = transition_box.row(align=True)
                row.prop(transition, "blend_seconds")
                remove = row.operator(DARKTIDE_OT_remove_animation_transition.bl_idname,
                                      text="", icon="X")
                remove.index = index
            output.operator(DARKTIDE_OT_add_animation_transition.bl_idname, icon="ADD")
            output.label(text="The game or mod must send each named event.")
            output.label(text="Runtime event dispatch is unverified.", icon="INFO")
        if not collection:
            output.prop(settings, "asset_path", text="Initial resource path")
            output.operator(DARKTIDE_OT_create_asset_collection.bl_idname, text="New Empty Asset")
        if collection:
            reference = layout.box()
            reference.label(text="Reference Skeleton")
            identity = collection.dt_asset_identity
            reference.prop(settings, "game_extract_folder")
            reference.prop(identity, "skeleton_preset")
            if identity.skeleton_preset_status:
                reference.label(text=identity.skeleton_preset_status[:90], icon="ERROR")
            reference.prop(identity, "reference_unit")
            reference.prop(identity, "reference_bones")
            reference.prop(identity, "skeleton_resource")
            reference.operator(DARKTIDE_OT_import_reference_skeleton.bl_idname)
            reference.prop(options, "only_weighted_bones")
            reference.label(text="Imported armature is added to this asset collection")
            reference.label(text="Rest-pose authoring reference; runtime deformation is not validated")
            fit_skeleton.draw_fit_panel(layout, context, collection)
        output.prop(options, "scale")
        if options.output_kind != "model":
            output.prop(options, "animation_fit_advanced", text="Animation fit settings")
            if options.animation_fit_advanced:
                fit = output.box()
                fit.prop(options, "animation_translation_tolerance")
                fit.prop(options, "animation_scale_tolerance")
                fit.prop(options, "animation_rotation_tolerance_radians")
        output.prop(settings, "compile_timeout")
        output.operator(DARKTIDE_OT_inspect.bl_idname)
        if options.inspection_summary:
            inspection = output.box()
            inspection.label(text="Compiler source view (last inspection)")
            for line in options.inspection_summary.splitlines():
                inspection.label(text=line)
        output.operator(DARKTIDE_OT_export_intermediate.bl_idname)
        output.operator(DARKTIDE_OT_build.bl_idname)
        collision_notes = _collision_review(scene, asset_objects) if asset_objects else []
        if collision_notes:
            review = layout.box()
            review.label(text="Collision review")
            for message in collision_notes[:3]:
                review.label(text=message)
        if context.object and context.object.active_material:
            material = context.object.active_material
            material_box = layout.box()
            material_box.label(text="Active material: " + material.name)
            material_box.prop(material.dt_material, "mode")
            if material.dt_material.mode == "external":
                material_box.prop(material.dt_material, "resource")
            elif material.dt_material.mode == "donor":
                settings = material.dt_material
                material_box.label(text="Renderer pairing is not game-verified")
                material_box.prop(settings, "donor_stream")
                for toggle, value in (("override_nm_r_blend", "nm_r_blend"),
                                      ("override_shared_blend", "shared_blend"),
                                      ("override_bc_blend", "bc_blend")):
                    row = material_box.row(align=True)
                    row.prop(settings, toggle, text="")
                    row.prop(settings, value)
            else:
                material_box.prop(material.dt_material, "surface")
        if context.object:
            collider = layout.box()
            collider.label(text="Active object: " + context.object.name)
            collider.prop(context.object.dt_collider, "is_body")
            if context.object.dt_collider.is_body:
                collider.prop(context.object.dt_collider, "actor")
                if context.object.dt_collider.actor == "dynamic":
                    collider.prop(context.object.dt_collider, "mass")
                collider.prop(context.object.dt_collider, "material")
            if context.object.type == "MESH":
                collider.prop(context.object.dt_collider, "role")
                if options.visible_meshes_collide and context.object.dt_collider.role == "render":
                    collider.prop(context.object.dt_collider, "use_asset_collision")
                if context.object.dt_collider.role != "render":
                    collider.prop(context.object.dt_collider, "shape")
                    if version2:
                        collider.prop(context.object.dt_collider, "body")
                if collection and collection.all_objects.get(context.object.name) == context.object:
                    collider.operator(DARKTIDE_OT_make_collision_proxy.bl_idname)
        if context.object:
            joint = context.object.dt_collider
            if version2 or context.object.type == "EMPTY":
                box = layout.box(); box.label(text="Joint")
                box.prop(joint, "joint_kind")
                if joint.joint_kind != "none":
                    box.prop(joint, "body_a"); box.prop(joint, "body_b")
                    if joint.joint_kind in {"hinge", "ragdoll"}:
                        box.prop(joint, "hinge_limits")
                        if joint.hinge_limits:
                            box.prop(joint, "twist_min"); box.prop(joint, "twist_max")
                    if joint.joint_kind == "ragdoll":
                        box.prop(joint, "swing_y"); box.prop(joint, "swing_z")


CLASSES = (*fit_skeleton.CLASSES, DarktideAnimationState, DarktideAnimationVariable, DarktideAnimationTransition,
           DarktideSceneSettings, DarktideCollectionSettings, DarktideMaterialSettings, DarktideColliderSettings,
           DARKTIDE_OT_create_asset_collection, DARKTIDE_OT_update_asset_collection,
           DARKTIDE_OT_add_animation_state, DARKTIDE_OT_remove_animation_state,
           DARKTIDE_OT_add_animation_variable, DARKTIDE_OT_remove_animation_variable,
           DARKTIDE_OT_add_animation_transition, DARKTIDE_OT_remove_animation_transition,
           DARKTIDE_OT_make_collision_proxy, DARKTIDE_OT_export_intermediate,
           DARKTIDE_OT_inspect, DARKTIDE_OT_build, DARKTIDE_OT_import_reference_skeleton,
           *fit_skeleton.UI_CLASSES, DARKTIDE_PT_asset_panel)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.dt_asset = PointerProperty(type=DarktideSceneSettings)
    bpy.types.Object.dt_collider = PointerProperty(type=DarktideColliderSettings)
    bpy.types.Collection.dt_asset_identity = PointerProperty(type=DarktideCollectionSettings)
    bpy.types.Material.dt_material = PointerProperty(type=DarktideMaterialSettings)


def unregister():
    del bpy.types.Object.dt_collider
    del bpy.types.Collection.dt_asset_identity
    del bpy.types.Material.dt_material
    del bpy.types.Scene.dt_asset
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
