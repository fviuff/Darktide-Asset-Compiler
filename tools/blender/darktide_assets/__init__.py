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
import fnmatch

import bpy
import bmesh
from mathutils import Matrix, Vector
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, PointerProperty, StringProperty
from .reference_skeleton import ReferenceSkeletonError, load_reference, _murmur64
from . import fit_skeleton
from . import retarget
from .skeleton_presets import SKELETON_PRESET_LIST, SKELETON_PRESETS
from . import game_unit
from . import game_state_machine
from . import state_graph
from . import breed
from . import particle_effect
from . import particle_preview
from . import state_machine_preview
from . import flow_editor


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


_BREED_ITEMS = []


def _breed_items():
    if not _BREED_ITEMS:
        _BREED_ITEMS.extend((name, name, entry["base_unit"]) for name, entry in sorted(breed.breeds().items()))
    return _BREED_ITEMS


_ZONE_ITEMS = []


def _zone_items():
    if not _ZONE_ITEMS:
        _ZONE_ITEMS.append(("auto", "From the game breed", "The zone the game breed gives this actor's name"))
        _ZONE_ITEMS.extend((zone, zone.replace("_", " ").capitalize(), "") for zone in breed.zone_names())
    return _ZONE_ITEMS


# Dropdowns that show states, clips and variables by name while the graph keeps their numbers. Blender needs the
# item lists kept alive while a dropdown is open.
_PICK_ITEMS = {}


def _graph_options(context):
    scene = context.scene if context else bpy.context.scene
    return _asset_options(scene) if scene else None


def _pick_items(kind, context):
    options = _graph_options(context)
    names = []
    if options is not None:
        if kind == "state":
            names = [state.state_name for state in options.state_graph_states]
        elif kind == "variable":
            names = [variable.variable_name for variable in options.state_graph_variables]
        else:
            clips = state_graph.clip_names(options)
            names = [clips.get(number, "clip " + str(number)) for number in range(max(clips) + 1)] if clips else []
    items = [(str(number), "%d  %s" % (number, name), "", number) for number, name in enumerate(names)]
    if not items:
        items = [("0", "0  (Inspect Asset to see clip names)" if kind == "clip" else "0", "", 0)]
    _PICK_ITEMS[kind] = items
    return items


def _picker(kind, field, name, description=""):
    return EnumProperty(name=name, description=description, items=lambda self, context: _pick_items(kind, context),
                        get=lambda self: getattr(self, field), set=lambda self, value: setattr(self, field, value))


class DarktideAnimationState(bpy.types.PropertyGroup):
    state_name: StringProperty(name="State name", default="State")
    clip_index: IntProperty(name="Clip index", default=0, min=0)
    clip_pick: _picker("clip", "clip_index", "Clip", "The action this state plays")
    playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat this clip"),
        ("once", "Play once", "Play this clip without looping"),
    ], default="loop")
    blend: BoolProperty(name="Blend", default=False,
                        description="Blend several clips by a float variable (e.g. stand, walk and run by speed)")
    blend_variable: IntProperty(name="Variable", default=0, min=0,
                                description="Index of the float variable that picks the mix")
    blend_clips: StringProperty(name="Clips", default="0:0, 1:1",
                                description="clip:value pairs, the clip by its name or number. A clip plays fully "
                                            "when the variable equals its value and fades into the clips next to it, "
                                            "e.g. walk:1, run:2. With 2D on, clip:x:y, e.g. idle:0:0, right:1:0, "
                                            "left:-1:0, forward:0:1")
    blend_2d: BoolProperty(name="2D", default=False,
                           description="Blend by two variables at once, e.g. strafing: x sideways, y forward")
    blend_variable2: IntProperty(name="Variable Y", default=1, min=0,
                                 description="Index of the float variable for the second direction")
    blend_variable_pick: _picker("variable", "blend_variable", "Variable", "The float variable that picks the mix")
    blend_variable2_pick: _picker("variable", "blend_variable2", "Variable Y",
                                  "The float variable for the second direction")
    random: BoolProperty(name="Random", default=False,
                         description="Play one of several clips picked at random (idle variations and the like); "
                                     "Clips lists them as clip:weight, e.g. 0:3, 1:1 plays clip 0 three times as often")
    random_pick: EnumProperty(name="Pick", items=[
        ("every_loop", "Every loop", "Pick a clip again each time one finishes"),
        ("no_repeat", "Every loop, no repeat", "Pick again each time, never the same clip twice in a row"),
        ("on_entry", "On entry", "Pick once when the state starts and keep it"),
    ], default="every_loop")
    speed: FloatProperty(name="Speed", default=1.0, description="Playback speed, 1 = as animated")
    speed_from_variable: BoolProperty(name="Speed from variable", default=False,
                                      description="Play at the speed a float variable holds, which your mod sets")
    speed_variable: IntProperty(name="Speed variable", default=0, min=0,
                                description="Index of the float variable that sets the playback speed")
    speed_variable_pick: _picker("variable", "speed_variable", "Speed variable",
                                 "The float variable that sets the playback speed")
    layer: IntProperty(name="Layer", default=0, min=0,
                       description="0 is the base layer. Higher layers play on top of it, each starting in its first "
                                   "state; transitions stay inside a layer and an event reaches every layer")
    empty: BoolProperty(name="Empty", default=False,
                        description="Play nothing on this layer, so the layers below show through")
    additive: BoolProperty(name="Additive", default=False,
                           description="Add this clip on top of the layers below instead of replacing them")
    events_at: StringProperty(name="Events at", default="",
                              description="Send these state machine events when the clip reaches a time in seconds, "
                                          "as seconds:event, e.g. 0.4:hit, 1.2:fire. Transitions on other states "
                                          "and layers react to them like to Unit.animation_event")
    exit_event: StringProperty(name="Exit event", default="",
                               description="Sent once just before the clip ends, e.g. done, with a transition on "
                                           "done back to idle for a clip that plays once")
    exit_blend: FloatProperty(name="Seconds left", default=0.2, min=0.0,
                              description="How long before the end the exit event goes out (use the transition's "
                                          "blend time so the next clip is fully in when this one ends)")
    bones: StringProperty(name="Bones", default="",
                          description="Only these bones play this state, each with everything below it, e.g. "
                                      "j_spine1 for the upper body. bone:weight plays it partly, e.g. j_neck:0.5, later "
                                      "entries win. Empty = the whole body")


class DarktideAnimationVariable(bpy.types.PropertyGroup):
    variable_name: StringProperty(name="Variable name", default="Variable")
    initial_value: FloatProperty(name="Initial value", default=0.0)
    minimum: FloatProperty(name="Minimum", default=0.0)
    maximum: FloatProperty(name="Maximum", default=1.0)
    preview_value: FloatProperty(name="Preview value", default=0.0,
                                 description="The variable's value while Play State Machine runs")


class DarktideAnimationTransition(bpy.types.PropertyGroup):
    from_state: IntProperty(name="From state index", default=0, min=0)
    from_any: BoolProperty(name="From any state", default=False,
                           description="Take this transition from every state of the target's layer (a transition "
                                       "of a state's own for the same event wins)")
    to_state: IntProperty(name="To state index", default=0, min=0)
    from_pick: _picker("state", "from_state", "From")
    to_pick: _picker("state", "to_state", "To")
    event_name: StringProperty(name="Event name")
    blend_seconds: FloatProperty(name="Blend seconds", default=0.0, min=0.0)
    condition: EnumProperty(name="Condition", items=[
        ("always", "Always on event", "Take this transition when the named event arrives"),
        ("range", "Variable range", "Choose this transition when a float variable falls inside its range"),
    ], default="always")
    variable_index: IntProperty(name="Variable index", default=0, min=0)
    variable_pick: _picker("variable", "variable_index", "Variable")
    lower: FloatProperty(name="Lower bound", default=0.0)
    upper: FloatProperty(name="Upper bound", default=1.0)
    lower_exclusive: BoolProperty(name="Exclude lower bound", default=False)
    upper_exclusive: BoolProperty(name="Exclude upper bound", default=False)




def apply_skeleton_preset(identity, folder, key):
    """Fill the reference UNIT/BONES/resource fields from a limn-extracted folder (<16hex>.<ext> files)."""
    resource = SKELETON_PRESETS[key]
    folder = bpy.path.abspath(folder.strip()) if folder else ""
    if not folder or not os.path.isdir(folder):
        raise ValueError("Set 'Game files extract' in the Setup panel to your limn extract")
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
        apply_skeleton_preset(self, prefs().game_extract_folder, self.skeleton_preset)
        fit_skeleton.refresh_reference_names(self)
    except ValueError as exc:
        self.skeleton_preset_status = str(exc)


def _default_game_folder():
    for root in (r"C:\Program Files (x86)\Steam\steamapps\common\Warhammer 40,000 DARKTIDE",
                 os.path.expanduser("~/.local/share/Steam/steamapps/common/Warhammer 40,000 DARKTIDE")):
        if os.path.isdir(os.path.join(root, "bundle")):
            return root
    return ""


class DarktidePreferences(bpy.types.AddonPreferences):
    """Paths that belong to this computer, not to a .blend: set once for every project."""
    bl_idname = __package__

    compiler: StringProperty(name="Compiler", subtype="FILE_PATH", description="DarktideGLBCompiler.exe")
    game_folder: StringProperty(name="Darktide folder", subtype="DIR_PATH", default=_default_game_folder(),
                                description="The game's install folder (the one with binaries and bundle in it)")
    game_extract_folder: StringProperty(name="Game files extract", subtype="DIR_PATH",
                                        description="Folder of game files extracted with limn: skeletons, game effects, "
                                                    "the game unit panel and game textures read from it")

    def draw(self, context):
        draw_setup(self.layout, context)


def prefs():
    return bpy.context.preferences.addons[__package__].preferences


class DarktideSceneSettings(bpy.types.PropertyGroup):
    game_unit: StringProperty(name="Game unit",
                              description="Resource path of a game unit whose nodes you need (attach, effect and animated "
                                          "nodes): a piece of gear, a weapon part, a prop, a character")
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (a subset skeleton, like the game's own gear)",
        default=False,
    )
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=2.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
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
    in_place: BoolProperty(
        name="In place",
        description="Remove the forward travel walk/run clips carry in their root bone so they loop on the spot. "
                    "build.log lists each clip's speed for moving the unit from your mod",
        default=False,
    )
    flow_tree: PointerProperty(
        name="Flow", type=bpy.types.NodeTree, poll=lambda self, tree: tree.bl_idname == flow_editor.TREE,
        description="The unit's flow (Darktide Flow node tree in the node editor): what happens when it spawns or "
                    "when a script calls Unit.flow_event(unit, name)",
    )
    weapon_materials: BoolProperty(
        name="Weapon or gear materials",
        description="Build the generated materials on the game's weapon shaders, so level decals like snow and dirt "
                    "don't land on them (the game's weapons and gear work that way)",
        default=False,
    )
    ragdoll_event: StringProperty(
        name="Ragdoll event",
        description="Animation event that turns the rig into a ragdoll (Unit.animation_event(unit, name)). "
                    "Dynamic bodies on the rig's bones stay switched off until then",
    )
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
    state_graph_game_events: StringProperty(name="Game events", default="")
    state_graph_game_events_source: StringProperty(name="Game events from", default="")
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
        ("custom", "Custom", "Enter the reference paths manually")] +
        [(key, label, resource) for key, label, resource in SKELETON_PRESET_LIST])
    skeleton_preset_status: StringProperty(name="Preset status", default="")
    bone_map: CollectionProperty(type=fit_skeleton.DarktideBoneMapRow)
    bone_map_index: IntProperty(default=0)
    reference_bone_names: CollectionProperty(type=fit_skeleton.DarktideReferenceBoneName)
    fit_report: StringProperty(name="Last fit report", default="")
    animation_source: PointerProperty(
        name="Animation from", type=bpy.types.Object, poll=lambda _self, obj: obj.type == "ARMATURE",
        description="Armature whose current animation Carry Over Animation bakes onto the Darktide armature "
                    "(empty = the rig the model was fitted from)",
    )
    fit_keep_torso: BoolProperty(
        name="Keep torso shape",
        description="The spine bones only move to their Darktide joints, without being stretched or tilted "
                    "(stops chests creasing or sagging on rigs whose spine differs a lot)",
        default=False,
    )
    fit_keep_proportions: BoolProperty(
        name="Keep proportions",
        description="Scale the model as a whole to the best average of all its bone lengths and scale each limb "
                    "evenly instead of stretching it along the bone (keeps the model's own build)",
        default=False,
    )
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (a subset skeleton, like the game's own gear)",
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
    in_place: BoolProperty(
        name="In place",
        description="Remove the forward travel walk/run clips carry in their root bone so they loop on the spot. "
                    "build.log lists each clip's speed for moving the unit from your mod",
        default=False,
    )
    root_motion: BoolProperty(
        name="Root motion",
        description="Move the forward travel of walk/run clips from the hips onto root_point, where the game reads "
                    "it as the character's movement (its own characters and enemies work that way). "
                    "build.log lists each clip's speed",
        default=False,
    )
    flow_tree: PointerProperty(
        name="Flow", type=bpy.types.NodeTree, poll=lambda self, tree: tree.bl_idname == flow_editor.TREE,
        description="The unit's flow (Darktide Flow node tree in the node editor): what happens when it spawns or "
                    "when a script calls Unit.flow_event(unit, name)",
    )
    weapon_materials: BoolProperty(
        name="Weapon or gear materials",
        description="Build the generated materials on the game's weapon shaders, so level decals like snow and dirt "
                    "don't land on them (the game's weapons and gear work that way)",
        default=False,
    )
    ragdoll_event: StringProperty(
        name="Ragdoll event",
        description="Animation event that turns the rig into a ragdoll (Unit.animation_event(unit, name)). "
                    "Dynamic bodies on the rig's bones stay switched off until then",
    )
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
    state_graph_game_events: StringProperty(
        name="Game events", default="",
        description="Every event a game character's scripts send (Import Game Events); the state machine lists them "
                    "all, since the game stops on an event it doesn't know. Events no transition takes do nothing")
    state_graph_game_events_source: StringProperty(name="Game events from", default="")
    animation_translation_tolerance: FloatProperty(name="Translation tolerance", description="Maximum measured translation error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_scale_tolerance: FloatProperty(name="Scale tolerance", description="Maximum measured scale error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_rotation_tolerance_radians: FloatProperty(name="Rotation tolerance (radians)", description="Maximum measured angular error for fitted animation", default=1e-3, min=1e-12, precision=8)
    animation_fit_advanced: BoolProperty(name="Show animation fit controls", default=False)
    inspection_summary: StringProperty(name="Last source inspection", default="")
    use_mover: BoolProperty(
        name="Mover",
        description="Give the unit a mover: the upright capsule that walks a character through the level and keeps it "
                    "out of walls (the game's characters and enemies have one named mover)",
        default=False,
    )
    mover_height: FloatProperty(name="Height", default=1.8, min=0.01, subtype="DISTANCE")
    mover_radius: FloatProperty(name="Radius", default=0.4, min=0.01, subtype="DISTANCE")
    mover_slope: FloatProperty(name="Max slope", default=0.6981317, min=0.0, max=1.5707963, subtype="ANGLE",
                               description="Steepest ground it walks up")
    mover_filter: StringProperty(name="Collides with", default="filter_minion_mover",
                                 description="The game's collision filter for it: filter_minion_mover for enemies, "
                                             "filter_player_mover for player characters")
    unit_package: BoolProperty(
        name="Package named like the unit",
        description="Load the asset as a package with the asset's own resource path, like the game's units. An enemy "
                    "body needs it: the game loads a breed's base unit as a package of that name when a mission loads "
                    "(Make a breed always does it)",
        default=False,
    )
    make_breed: BoolProperty(
        name="Make a breed (EXPERIMENTAL)",
        description="Write a Lua file next to the compiled asset that adds an enemy of its own to the game: it behaves "
                    "like the game breed you pick, with this asset as its body",
        default=False,
    )
    breed_name: StringProperty(name="Name", default="my_enemy",
                               description="The new breed's name, lowercase (what your mod spawns it by)")
    breed_base: EnumProperty(name="Behaves like", items=lambda self, context: _breed_items(),
                             description="The game breed whose scripts drive it: behaviour, attacks, sounds, "
                                         "staggers")
    breed_body: EnumProperty(
        name="Body",
        items=(
            ("own", "Own body", "Your model is the whole body: the game breed's weapons are hung on it, its other body "
                                "pieces are left off, and so are its wounds, gibbing and dissolving (they work on those "
                                "pieces)"),
            ("game", "Game body pieces too", "The game breed's body pieces, wounds and gibbing on top of your model, "
                                             "like the game's own enemy"),
        ),
        default="own",
    )
    breed_health: StringProperty(name="Health", default="",
                                 description="One number (health on the easiest difficulty, the others scaled like "
                                             "the game breed's) or five, one per difficulty. Empty keeps the game "
                                             "breed's")
    breed_walk_speed: FloatProperty(name="Walk speed", default=0.0, min=0.0,
                                    description="Metres per second, 0 keeps the game breed's")
    breed_run_speed: FloatProperty(name="Run speed", default=0.0, min=0.0,
                                   description="Metres per second, 0 keeps the game breed's")


_GAME_SHADERS = None


def _game_shaders():
    """The game's own shaders offered in the Game shader list (game_shaders.json)."""
    global _GAME_SHADERS
    if _GAME_SHADERS is None:
        try:
            with open(os.path.join(os.path.dirname(__file__), "game_shaders.json"), encoding="utf-8") as handle:
                _GAME_SHADERS = {preset["id"]: preset for preset in json.load(handle)["presets"]}
        except (OSError, ValueError, KeyError):
            _GAME_SHADERS = {}
    return _GAME_SHADERS


def _game_shader_items(self, context):
    return [(key, preset["label"], preset["shader"]) for key, preset in _game_shaders().items()] or [("", "None", "")]


def _parse_assignments(text, what):
    """'name=1,0.5,0; other=2' -> {name: [floats]} (what='numbers') or {name: word} (what='words')."""
    result = {}
    for part in text.replace("\n", ";").split(";"):
        if not part.strip():
            continue
        if "=" not in part:
            raise ValueError("Expected name=value in '" + part.strip() + "'")
        name, value = (item.strip() for item in part.split("=", 1))
        if what == "numbers":
            numbers = [float(item) for item in value.replace(" ", ",").split(",") if item.strip()]
            if not 1 <= len(numbers) <= 4 or not all(math.isfinite(number) for number in numbers):
                raise ValueError("'" + name + "' needs 1 to 4 finite numbers")
            result[name] = numbers[0] if len(numbers) == 1 else numbers
        else:
            if value not in ("base_color", "normal", "orm", "emissive"):
                raise ValueError("Texture channel '" + name + "' must map to base_color, normal, orm or emissive")
            result[name] = value
    return result


_TEXTURE_SOURCES = (
    ("game", "Game's texture", "Keep the texture the game's material has in this slot"),
    ("base_color", "Base Color", "The image plugged into Base Color on this material"),
    ("normal", "Normal", "The image behind the Normal Map node on this material"),
    ("orm", "ORM", "The occlusion / roughness / metallic image of this material"),
    ("emissive", "Emission", "The image plugged into Emission on this material"),
)


class DarktideShaderValue(bpy.types.PropertyGroup):
    """One setting of a game shader, like a row of the Stingray property editor."""
    floats: IntProperty(default=1, min=1, max=4)
    is_color: BoolProperty(default=False)
    changed: BoolProperty(name="Change", default=False,
                          description="Use your own value for this setting (unticked keeps the game's)")
    value: FloatVectorProperty(name="Value", size=4)
    color: FloatVectorProperty(name="Color", size=4, subtype="COLOR", min=0.0, soft_max=1.0)


class DarktideShaderTexture(bpy.types.PropertyGroup):
    source: EnumProperty(name="Image", items=_TEXTURE_SOURCES, default="game",
                         description="Which image goes into this texture slot")


def _shader_rows(settings):
    """Fill the settings and texture rows from the picked game shader; typed values from older files carry over."""
    preset = _game_shaders().get(settings.game_shader)
    if not preset:
        return
    kept = {row.name: (row.changed, list(row.value), list(row.color)) for row in settings.shader_value_rows}
    try:
        typed = _parse_assignments(settings.shader_values, "numbers")
    except ValueError:
        typed = {}
    settings.shader_value_rows.clear()
    for variable in preset["variables"]:
        row = settings.shader_value_rows.add()
        row.name, row.floats = variable["name"], variable["floats"]
        row.is_color = "color" in variable["name"] and variable["floats"] >= 3
        default = (list(variable["default"]) + [0.0] * 4)[:4]
        row.value = default
        row.color = default[:3] + [default[3] if variable["floats"] == 4 else 1.0]
        if variable["name"] in typed:
            numbers = typed.pop(variable["name"])
            numbers = numbers if isinstance(numbers, list) else [numbers]
            numbers = (numbers + default[len(numbers):])[:4]
            row.changed, row.value, row.color = True, numbers, numbers
        elif variable["name"] in kept:
            row.changed, row.value, row.color = kept[variable["name"]]
    kept_textures = {row.name: row.source for row in settings.shader_texture_rows}
    try:
        typed_textures = _parse_assignments(settings.shader_textures, "words")
    except ValueError:
        typed_textures = {}
    settings.shader_texture_rows.clear()
    for slot in preset["textures"]:
        row = settings.shader_texture_rows.add()
        row.name = slot
        row.source = typed_textures.pop(slot, None) or kept_textures.get(slot, "game")
    # typed names this shader's list doesn't have stay typed (Other settings)
    settings.shader_values = "; ".join("%s=%s" % (name, ",".join("%g" % v for v in (value if isinstance(value, list) else [value])))
                                       for name, value in typed.items())
    settings.shader_textures = "; ".join("%s=%s" % item for item in typed_textures.items())


def _shader_assignments(settings):
    """(variables, textures) for the compiler from the rows, plus typed values of an older file whose rows were never
    built."""
    variables = _parse_assignments(settings.shader_values, "numbers")
    textures = _parse_assignments(settings.shader_textures, "words")
    for row in settings.shader_value_rows:
        if row.changed:
            numbers = list(row.color if row.is_color else row.value)[:row.floats]
            variables[row.name] = numbers[0] if row.floats == 1 else numbers
    for row in settings.shader_texture_rows:
        if row.source != "game":
            textures[row.name] = row.source
    return variables, textures


class DarktideMaterialSettings(bpy.types.PropertyGroup):
    mode: EnumProperty(name="Material intent", default="generated", items=[
        ("generated", "Generated from glTF PBR", "Build a Darktide material from this material's glTF PBR values"),
        ("external", "Use external material", "Bind an existing native Darktide material resource"),
        ("donor", "World blend material", "Reuse one of the game's world surface blend materials from its .stream file, with your blend values"),
        ("game_shader", "Game shader", "Use one of the game's own shaders (glass, water, hologram, glow, fur...) with your values"),
    ])
    game_shader: EnumProperty(name="Shader", items=_game_shader_items,
                              update=lambda self, context: _shader_rows(self),
                              description="Game shader to copy; its settings and texture slots are listed below")
    shader_value_rows: CollectionProperty(type=DarktideShaderValue)
    shader_texture_rows: CollectionProperty(type=DarktideShaderTexture)
    shader_values: StringProperty(name="Values", description="Variables to change, e.g. color=1,0.2,0.1; opacity=0.8")
    shader_textures: StringProperty(name="Textures",
                                    description="Texture channels to fill from this material's glTF images, "
                                                "e.g. bca=base_color; nm=normal (others: orm, emissive)")
    resource: StringProperty(name="Material resource", description="Native material resource identity, e.g. content/mods/my_mod/materials/paint")
    donor_stream: StringProperty(name="Donor stream", description="Native v61 .streamdata or .stream material file", subtype="FILE_PATH")
    override_nm_r_blend: BoolProperty(name="Override nm_r_blend", default=False)
    nm_r_blend: FloatProperty(name="nm_r_blend", default=0.0)
    override_shared_blend: BoolProperty(name="Override shared_blend", default=False)
    shared_blend: FloatProperty(name="shared_blend", default=0.0)
    override_bc_blend: BoolProperty(name="Override bc_blend", default=False)
    bc_blend: FloatProperty(name="bc_blend", default=0.0)
    double_sided: BoolProperty(
        name="Double-sided", default=False,
        description="Show the back faces too (leaves, cloth, cards, flat signs). They are added as extra triangles, "
                    "so only tick it where it's needed",
    )
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


class DarktideDangleSettings(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="Dangle", default=False,
                          description="Let this bone swing freely under gravity in game (pendulum). "
                                      "The unit gets a state machine; mods start it with Unit.enable_animation_state_machine")
    length: FloatProperty(name="Length", default=0.0, min=0.0, subtype="DISTANCE",
                          description="Pendulum length; 0 uses the distance to the child bone, or this bone's length")
    mass: FloatProperty(name="Mass", default=1.0, min=0.001)
    gravity: FloatProperty(name="Gravity", default=9.82)
    damping: FloatProperty(name="Damping", default=3.0, min=0.0, description="Higher settles faster")
    stiffness: FloatProperty(name="Stiffness", default=0.0, min=0.0,
                             description="Pull back toward the bone's rest direction; 0 hangs freely")
    max_angle: FloatProperty(name="Max angle", default=0.0, min=0.0, max=180.0,
                             description="Swing limit in degrees from the rest direction; 0 is unlimited")
    mode: EnumProperty(name="Mode", default="swing", items=[
        ("swing", "Swing", "Pendulum: the bone swings around its head (straps, cables, tassels)"),
        ("jiggle", "Jiggle", "Spring: the bone's position bounces behind its animated position (pouches, bellies)"),
    ])
    jiggle_stiffness: FloatProperty(name="Stiffness", default=1000.0, min=0.0, description="Spring strength")
    jiggle_damping: FloatProperty(name="Damping", default=400.0, min=0.0, description="Higher settles faster")
    max_stretch: FloatProperty(name="Max stretch", default=0.1, min=0.001, subtype="DISTANCE",
                               description="How far the bone may lag behind its animated position")


class DarktideAimSettings(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="Aim", default=False,
                          description="Turn bones so this bone points at a target your mod moves "
                                      "(Unit.animation_set_constraint_target), like a turret or a head following you")
    target: StringProperty(name="Target", default="aim_target",
                           description="Target name; your mod finds it with Unit.animation_find_constraint_target")
    turn: StringProperty(name="Turn", default="",
                         description="Bones that turn toward the target, as bone:weight, e.g. j_spine2:0.2, j_neck:1 "
                                     "for an aim on j_head. Empty turns this bone's parent fully")


class DarktideColliderSettings(bpy.types.PropertyGroup):
    stable_id: StringProperty(name="Stable ID")
    is_body: BoolProperty(name="Body", default=False)
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=1.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    body: PointerProperty(name="Body", type=bpy.types.Object)
    joint_kind: EnumProperty(name="Joint", default="none", items=[
        ("none", "None", ""), ("fixed", "Fixed", ""), ("hinge", "Hinge", "Turns around the empty's x axis"),
        ("ragdoll", "Ragdoll", "Swings and twists within limits, like a body joint"),
        ("slider", "Slider", "Slides along the empty's x axis, like a drawer"),
        ("ball", "Ball", "Turns freely in every direction"),
    ])
    body_a: PointerProperty(name="Body A", type=bpy.types.Object)
    body_b: PointerProperty(name="Body B", type=bpy.types.Object)
    hinge_limits: BoolProperty(name="Limit twist", default=False)
    twist_min: FloatProperty(name="Twist minimum", default=-3.14159265, subtype="ANGLE")
    twist_max: FloatProperty(name="Twist maximum", default=3.14159265, subtype="ANGLE")
    swing_y: FloatProperty(name="Swing Y", default=0.78539816, subtype="ANGLE", min=0.0)
    swing_z: FloatProperty(name="Swing Z", default=0.78539816, subtype="ANGLE", min=0.0)
    slide_limits: BoolProperty(name="Limit travel", default=False)
    slide_min: FloatProperty(name="Travel minimum", default=-0.5, subtype="DISTANCE")
    slide_max: FloatProperty(name="Travel maximum", default=0.5, subtype="DISTANCE")
    spring_stiffness: FloatProperty(name="Spring", default=0.0, min=0.0,
                                    description="Pulls the joint back to how it sits in blender (0 = no spring), "
                                                "e.g. a door that swings shut by itself")
    spring_damping: FloatProperty(name="Spring damping", default=0.0, min=0.0,
                                  description="Slows the spring down so it settles instead of bouncing")
    break_force: FloatProperty(name="Break force", default=0.0, min=0.0,
                               description="The joint snaps above this force in newtons (0 = never)")
    break_torque: FloatProperty(name="Break torque", default=0.0, min=0.0,
                                description="The joint snaps above this twisting force (0 = never)")
    shape: EnumProperty(name="Collision geometry", default="geometry", items=[
        ("geometry", "Mesh surface", "Use this mesh's triangle surface for static/keyframed bodies"),
        ("convex", "Convex mesh", "Use a convex hull of this authored mesh for a simulated body"),
        ("capsule", "Capsule", "A capsule fitted to this mesh along its longest axis (bodies of their own)"),
        ("sphere", "Sphere", "A sphere fitted to this mesh (bodies of their own)"),
        ("box", "Box", "A box fitted to this mesh (bodies of their own)"),
    ])
    role: _enum(("render", "collision", "both"), "render")
    unit_actor: EnumProperty(name="Own actor", default="none", items=[
        ("none", "None", "A normal mesh"),
        ("hit_zone", "Hit zone (EXPERIMENTAL)", "An enemy hit zone: shots and swings land on it, it follows the bone it's parented "
                                 "to while the character animates. Breeds list their hit zones by these names"),
        ("custom", "Other", "An actor of its own with the physics templates you type in"),
    ], description="Turn this mesh into a collision actor of its own, named after the object (not drawn)")
    actor_name: StringProperty(name="Actor name", description="Name scripts find it by (empty = the object's name)")
    actor_shape: EnumProperty(name="Shape", default="capsule", items=[
        ("capsule", "Capsule", "Fitted along the mesh's longest axis"),
        ("sphere", "Sphere", ""),
        ("box", "Box", ""),
        ("convex", "Convex", "A convex hull of the mesh"),
        ("geometry", "Mesh surface", "The triangles themselves (not for simulated bodies)"),
    ])
    actor_template: StringProperty(name="Actor template", default="keyframed",
                                   description="physics_properties actor template: static, dynamic, keyframed, "
                                               "or #1234abcd for one only known by its hash")
    actor_shape_template: StringProperty(name="Shape template", default="default",
                                         description="physics_properties shape template (what it collides with), "
                                                     "or #1234abcd")
    actor_spawn: BoolProperty(name="Created at spawn", default=True,
                              description="Off leaves it to a script (Unit.create_actor)")
    hit_zone_group: EnumProperty(name="Hit zone group (EXPERIMENTAL)", items=lambda self, context: _zone_items(),
                                 description="Which part of the body a hit here counts as (head takes headshots) "
                                             "in a breed you make")
    use_asset_collision: BoolProperty(
        name="Use as asset collision",
        description="When the asset uses visible mesh collision, include this mesh as a collider",
        default=True,
    )
    visibility_group: StringProperty(
        name="Visibility group",
        description="Name of a group this object's meshes (and all meshes below it) belong to; "
                    "toggle in game with Unit.set_visibility(unit, name, visible)",
    )
    render_visible: BoolProperty(
        name="Visible", default=True,
        description="Draw this object's meshes (and all meshes below it). Off with Casts shadow on makes a "
                    "shadow-only stand-in, like the game's simplified shadow meshes",
    )
    render_shadow: BoolProperty(
        name="Casts shadow", default=True,
        description="This object's meshes (and all meshes below it) cast shadows",
    )
    lod_group: StringProperty(
        name="LOD group",
        description="Makes this object (its meshes and all meshes below it) one detail level of a LOD object "
                    "with this name. The game's units use \"lod\"; weapons and gear need it named that way",
    )
    lod_level: IntProperty(
        name="Level", min=0, default=0,
        description="0 = full detail, 1 = the next simpler version, and so on",
    )
    lod_down_to: FloatProperty(
        name="Visible down to", min=0.0, default=0.0, precision=3,
        description="Screen height share (1 = the object fills the screen height) below which the next level "
                    "takes over. 0 on the last level keeps it visible at any distance",
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
                       "create_state_graph", "ragdoll_event", "in_place", "weapon_materials", "flow_tree",
                       "animation_translation_tolerance", "animation_scale_tolerance",
                       "animation_rotation_tolerance_radians", "animation_fit_advanced",
                       "only_weighted_bones")


def _copy_state_graph(source, target):
    rows = (
        ("state_graph_states", ("state_name", "clip_index", "playback", "blend", "blend_variable", "blend_clips",
                                "speed", "speed_from_variable", "speed_variable", "random", "random_pick",
                                "layer", "empty", "additive", "bones", "blend_2d", "blend_variable2",
                                "events_at", "exit_event", "exit_blend")),
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
    layers = sorted({state.layer for state in states})
    if layers != list(range(len(layers))):
        raise ValueError("Layers have to count up from 0 without gaps (layer " + str(len(layers)) + " is missing)")
    for transition in options.state_graph_transitions:
        if (transition.from_state < len(states) and transition.to_state < len(states) and
                states[transition.from_state].layer != states[transition.to_state].layer):
            raise ValueError("Transition from state " + str(transition.from_state) + " to state " +
                             str(transition.to_state) + " crosses layers; transitions stay inside a layer")
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
    for transition in options.state_graph_transitions:
        if "*" in transition.event_name and not any(fnmatch.fnmatchcase(event, transition.event_name.strip())
                                                    for event in state_graph.known_events(options)):
            raise ValueError("No event matches " + transition.event_name.strip())
    for index, transition in enumerate(state_graph.expand(options)):
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
    if obj.dt_collider.unit_actor != "none":
        return "render"     # an actor's mesh is its shape, the compiler takes it out
    role = obj.dt_collider.role
    if role == "render" and include_visible and obj.dt_collider.use_asset_collision:
        return "both"
    return role


def _collider_objects(objects, include_visible=False):
    return [o for o in objects if o.type == "MESH" and
            _collider_role(o, include_visible) != "render"]


def _authored_objects(objects, include_visible=False):
    return [o for o in objects if o.dt_collider.is_body or o.dt_collider.joint_kind != "none" or
            o.dt_collider.visibility_group.strip() or o.dt_collider.lod_group.strip() or
            not o.dt_collider.render_visible or not o.dt_collider.render_shadow or
            (o.type == "EMPTY" and o.dt_particles.enabled) or (o.type == "LIGHT" and o.data.use_shadow) or
            (o.type == "MESH" and _collider_role(o, include_visible) != "render") or
            (o.type == "MESH" and o.dt_collider.unit_actor != "none")]


def _actor_extras(obj):
    settings = obj.dt_collider
    hit_zone = settings.unit_actor == "hit_zone"
    result = {
        "name": settings.actor_name.strip() or obj.name,
        "template": game_unit.HIT_ZONE_TEMPLATE if hit_zone else settings.actor_template.strip(),
        "shape_template": game_unit.HIT_ZONE_SHAPE_TEMPLATE if hit_zone else settings.actor_shape_template.strip(),
        "shape": settings.actor_shape,
        "spawn": settings.actor_spawn,
    }
    if not result["template"] or not result["shape_template"]:
        raise ValueError(obj.name + ": an actor needs an actor template and a shape template")
    return result


def _mover_extras(options):
    return {"height": options.mover_height, "radius": options.mover_radius, "slope_limit": options.mover_slope,
            "collision_filter": options.mover_filter.strip() or "filter_minion_mover"}


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
            if settings.joint_kind == "slider" and settings.slide_limits and settings.slide_min > settings.slide_max:
                raise ValueError("Travel minimum must not exceed maximum")


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
        if settings.joint_kind == "slider" and settings.slide_limits:
            joint.update({"slide_min": settings.slide_min, "slide_max": settings.slide_max})
        if settings.joint_kind != "fixed" and (settings.spring_stiffness > 0.0 or settings.spring_damping > 0.0):
            joint.update({"spring_stiffness": settings.spring_stiffness, "spring_damping": settings.spring_damping})
        if settings.break_force > 0.0:
            joint["break_force"] = settings.break_force
        if settings.break_torque > 0.0:
            joint["break_torque"] = settings.break_torque
        result["joint"] = joint
    if settings.visibility_group.strip():
        result["visibility_group"] = settings.visibility_group.strip()
    if settings.lod_group.strip():
        result["lod"] = _lod_extras(settings)
    if not settings.render_visible or not settings.render_shadow:
        result["render"] = _render_extras(settings)
    if obj.type == "LIGHT" and obj.data.use_shadow:
        result["light"] = {"shadow": True}
    return result


def _render_extras(settings):
    return {"visible": settings.render_visible, "shadow": settings.render_shadow}


def _lod_extras(settings):
    return {"group": settings.lod_group.strip(), "level": settings.lod_level, "down_to": settings.lod_down_to}


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


def _compiler_path():
    if not prefs().compiler.strip():
        raise ValueError("Set the compiler in the Setup panel first")
    compiler = bpy.path.abspath(prefs().compiler)
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
    # the compiler reads the game's files (Oodle, shaders, stub textures) from the Darktide folder of the setup
    game = prefs().game_folder.strip()
    if game:
        kwargs["env"] = dict(kwargs.get("env") or os.environ,
                             DARKTIDE_GAME_ROOT=_windows_path(os.path.abspath(bpy.path.abspath(game)), wine_tools))
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


_BONE_PATH = re.compile(r'pose\.bones\["((?:[^"\\]|\\.)*)"\]')


def _hide_foreign_actions(objects):
    """The glTF exporter adds every bone action in the file to a lone armature, also actions made for another rig
    (an mmd tools model's dance, the rig an animation was carried over from). Those would come out as motionless
    clips, so actions that move none of the exported bones go on the exporter's own skip list for this export.
    Returns an undo function, or None when nothing needed hiding."""
    bones = {bone.name for obj in objects if obj.type == "ARMATURE" for bone in obj.data.bones}
    users = {}
    for obj in bpy.data.objects:
        data = obj.animation_data
        if data is None:
            continue
        used = [data.action] + [strip.action for track in data.nla_tracks for strip in track.strips]
        for action in used:
            if action is not None:
                users.setdefault(action, set()).add(obj)
    exported = set(objects)
    foreign = []
    for action in bpy.data.actions:
        if action in users and not users[action] & exported:
            foreign.append(action)          # someone else's animation, e.g. the rig it was carried over from
            continue
        moved = set()
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for curve in bag.fcurves:
                        match = _BONE_PATH.match(curve.data_path)
                        if match:
                            moved.add(match.group(1))
        if moved and not moved & bones:
            foreign.append(action)
    if not foreign or not bpy.data.scenes:
        return None
    scene = bpy.data.scenes[0]                    # the exporter reads the list from the first scene
    registered = not hasattr(bpy.types.Scene, "gltf_action_filter")
    if registered:
        import io_scene_gltf2
        bpy.types.Scene.gltf_action_filter = bpy.props.CollectionProperty(type=io_scene_gltf2.GLTF2_filter_action)
    added, changed = [], []
    for action in foreign:
        item = next((i for i in scene.gltf_action_filter if i.action == action), None)
        if item is None:
            item = scene.gltf_action_filter.add()
            item.action = action
            added.append(action)
        elif item.keep:
            changed.append(item)
        item.keep = False

    def undo():
        for item in changed:
            item.keep = True
        for index in reversed(range(len(scene.gltf_action_filter))):
            if scene.gltf_action_filter[index].action in added:
                scene.gltf_action_filter.remove(index)
        if registered:
            del bpy.types.Scene.gltf_action_filter
    return undo


def _mmd_stand_in(material):
    """mmd tools materials draw through its own node group, which the glTF exporter can't read. For the export the
    material gets a Principled BSDF output with the same base texture and colour (cut-out textures alpha clipped,
    as the game draws them). Returns an undo function, or None when it isn't an mmd tools material."""
    import numpy as np
    tree = material.node_tree if material.use_nodes else None
    output = next((node for node in tree.nodes if node.type == "OUTPUT_MATERIAL" and node.is_active_output),
                  None) if tree else None
    surface = output.inputs.get("Surface") if output else None
    shader = surface.links[0].from_node if surface and surface.is_linked else None
    if shader is None or shader.type != "GROUP" or not all(
            shader.inputs.get(name) for name in ("Base Tex", "Diffuse Color", "Alpha")):
        return None
    added = [tree.nodes.new("ShaderNodeBsdfPrincipled"), tree.nodes.new("ShaderNodeOutputMaterial")]
    bsdf, stand_in = added
    tree.links.new(bsdf.outputs["BSDF"], stand_in.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.8
    texture = shader.inputs["Base Tex"].links[0].from_node if shader.inputs["Base Tex"].is_linked else None
    if texture is not None and texture.type == "TEX_IMAGE" and texture.image is not None:
        tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
        image = texture.image
        pixels = np.empty(len(image.pixels), np.float32)
        image.pixels.foreach_get(pixels)
        if image.channels == 4 and pixels.size and (pixels[3::4] < 0.5).any():
            clip = tree.nodes.new("ShaderNodeMath")
            clip.operation = "ROUND"
            added.append(clip)
            tree.links.new(texture.outputs["Alpha"], clip.inputs[0])
            tree.links.new(clip.outputs[0], bsdf.inputs["Alpha"])
    else:
        r, g, b, _ = shader.inputs["Diffuse Color"].default_value
        bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
        if shader.inputs["Alpha"].default_value < 0.999:
            bsdf.inputs["Alpha"].default_value = shader.inputs["Alpha"].default_value
    stand_in.is_active_output = True

    def undo():
        for node in added:
            tree.nodes.remove(node)
        output.is_active_output = True
    return undo


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
    """Map each armature to the names of bones with nonzero vertex weights or objects hung on them (rigid parts,
    hit zones, ragdoll bodies), plus all ancestors."""
    result = {}
    for obj in objects:
        if obj.parent is not None and obj.parent.type == "ARMATURE" and obj.parent_type == "BONE":
            keep = result.setdefault(obj.parent, set())
            bone = obj.parent.data.bones.get(obj.parent_bone)
            while bone is not None:
                keep.add(bone.name)
                bone = bone.parent
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
        elif material.dt_material.mode == "game_shader":
            preset = _game_shaders().get(material.dt_material.game_shader)
            if not preset:
                raise ValueError("Material " + material.name + " needs a game shader")
            stream = os.path.join(bpy.path.abspath(prefs().game_folder), "bundle", *preset["stream"].split("/"))
            if not os.path.isfile(stream):
                raise ValueError("Game shader stream not found (set the Darktide folder): " + stream)
            _shader_assignments(material.dt_material)
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
    backface_culling = []
    stand_ins = []
    hidden_actions = None
    changed_bones = []
    changed_bone_extras = []
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
            elif obj in authored:  # visibility group only
                obj[SCHEMA_KEY] = {"version": 1, "id": obj.dt_collider.stable_id}
            group = obj.dt_collider.visibility_group.strip()
            if group and not version2:
                obj[SCHEMA_KEY]["visibility_group"] = group
            if obj.dt_collider.lod_group.strip() and not version2:
                obj[SCHEMA_KEY]["lod"] = _lod_extras(obj.dt_collider)
            if (not obj.dt_collider.render_visible or not obj.dt_collider.render_shadow) and not version2:
                obj[SCHEMA_KEY]["render"] = _render_extras(obj.dt_collider)
            if obj.type == "LIGHT" and obj.data.use_shadow and not version2:
                obj[SCHEMA_KEY]["light"] = {"shadow": True}
            if obj.type == "MESH" and obj.dt_collider.unit_actor != "none":
                obj[SCHEMA_KEY]["actor"] = _actor_extras(obj)
            if obj.type == "EMPTY" and obj.dt_particles.enabled:
                extract = os.path.abspath(bpy.path.abspath(prefs().game_extract_folder.strip()))
                if not prefs().game_extract_folder.strip() or not os.path.isdir(extract):
                    raise ValueError("Particle effects need 'Game files extract' in the Setup panel set to your limn extract")
                obj[SCHEMA_KEY]["particles"] = particle_effect.node_extras(
                    obj, extract, donor_path_converter or (lambda value: value))
        data = _collection_data(scene.dt_asset.asset_collection)
        flow_tree = _asset_options(scene).flow_tree
        use_mover = _asset_options(scene).use_mover
        if (flow_tree is not None or data or use_mover) and objects:
            holder = objects[0]
            if SCHEMA_KEY not in holder:
                if not holder.dt_collider.stable_id.strip():
                    holder.dt_collider.stable_id = _new_id()
                holder[SCHEMA_KEY] = {"version": 1, "id": holder.dt_collider.stable_id}
            if flow_tree is not None:
                holder[SCHEMA_KEY]["flow"] = json.dumps(flow_editor.description(flow_tree))
            if data:
                holder[SCHEMA_KEY]["data"] = json.dumps(data)
            if use_mover:
                holder[SCHEMA_KEY]["mover"] = _mover_extras(_asset_options(scene))
        for obj in objects:
            if obj.type != "ARMATURE":
                continue
            for bone in obj.data.bones:
                if not bone.dt_dangle.enabled and not bone.dt_aim.enabled:
                    continue
                had_original = SCHEMA_KEY in bone
                original = _copy_idproperty(bone[SCHEMA_KEY]) if had_original else None
                changed_bone_extras.append((bone, original, had_original))
                extras = {"version": 1, "id": "dangle:" + obj.name + ":" + bone.name}
                dangle = bone.dt_dangle
                if dangle.enabled and dangle.mode == "jiggle":
                    extras["dangle"] = {"mode": "jiggle", "mass": dangle.mass, "gravity": dangle.gravity,
                                        "stiffness": dangle.jiggle_stiffness, "damping": dangle.jiggle_damping,
                                        "max_stretch": dangle.max_stretch}
                elif dangle.enabled:
                    extras["dangle"] = {"mass": dangle.mass, "gravity": dangle.gravity, "damping": dangle.damping,
                                        "stiffness": dangle.stiffness, "max_angle": dangle.max_angle,
                                        "length": dangle.length if dangle.length > 0.0 else bone.length}
                if bone.dt_aim.enabled:
                    extras["aim"] = {"target": bone.dt_aim.target.strip(), "turn": _aim_turn(bone)}
                bone[SCHEMA_KEY] = extras
        for material in materials:
            had_original = MATERIAL_SCHEMA_KEY in material
            original = _copy_idproperty(material[MATERIAL_SCHEMA_KEY]) if had_original else None
            changed_materials.append((material, original, had_original))
            material.pop(MATERIAL_SCHEMA_KEY, None)
            # the glTF exporter writes doubleSided from Backface Culling, which Blender leaves off by default
            backface_culling.append((material, material.use_backface_culling))
            material.use_backface_culling = not material.dt_material.double_sided
            if material.dt_material.mode == "external":
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "external",
                    "resource": _validate_material_resource(material.dt_material.resource, material.name),
                }
            elif material.dt_material.mode == "game_shader":
                preset = _game_shaders()[material.dt_material.game_shader]
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "game_shader",
                    "stream": (donor_path_converter or (lambda value: value))(os.path.join(
                        os.path.abspath(bpy.path.abspath(prefs().game_folder)), "bundle", *preset["stream"].split("/"))),
                    "variables": _shader_assignments(material.dt_material)[0],
                    "textures": _shader_assignments(material.dt_material)[1],
                    # the materials holding the compiled shaders; the compiler ships copies of them
                    "shader_streams": preset.get("shader_streams", {}),
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
                undo = _mmd_stand_in(material)
                if undo:
                    stand_ins.append(undo)
                intent = {"version": 1, "mode": "generated"}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                if _asset_options(scene).weapon_materials:
                    intent["weapon"] = True
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
        _set_if_supported(kwargs, operator, "export_lights", True)  # point/spot lamps become unit lights
        _set_if_supported(kwargs, operator, "export_tangents", True)
        _set_if_supported(kwargs, operator, "export_all_influences", True)
        _set_if_supported(kwargs, operator, "will_save_settings", False)
        hidden_actions = _hide_foreign_actions(objects)
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
        for bone, original, had_original in changed_bone_extras:
            if not had_original:
                bone.pop(SCHEMA_KEY, None)
            else:
                bone[SCHEMA_KEY] = original
        for material, culling in backface_culling:
            material.use_backface_culling = culling
        for undo in stand_ins:
            undo()
        if hidden_actions:
            hidden_actions()
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
            compiler = _compiler_path()
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
        breed_lua = None
        if options.make_breed:
            try:
                if not asset_path:
                    raise ValueError("A breed needs the asset's resource path (its body)")
                breed_lua = breed.lua(options, asset_path, [obj for obj in collection.all_objects
                                                            if obj.type == "MESH" and obj.dt_collider.unit_actor != "none"])
            except ValueError as exc:
                self.report({"ERROR"}, str(exc))
                return {"CANCELLED"}
        try:
            compiler = _compiler_path()
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
            if options.ragdoll_event.strip() and options.output_kind == "all":
                command.extend(["--ragdoll-event", options.ragdoll_event.strip()])
            if (options.unit_package or options.make_breed) and asset_path:
                command.extend(["--package-name", asset_path])
            if options.root_motion and options.output_kind != "model":
                command.append("--root-motion")
            elif options.in_place and options.output_kind != "model":
                command.append("--in-place")
            if options.create_state_graph:
                for state in options.state_graph_states:
                    clips = state_graph.clips_text(options, state.blend_clips)
                    if state.empty:
                        command.extend(["--sm-empty", state.state_name.strip(), str(state.layer)])
                    elif state.random:
                        command.extend(["--sm-random", state.state_name.strip(), state.random_pick, state.playback, clips])
                    elif state.blend:
                        variables = str(state.blend_variable) + ("," + str(state.blend_variable2) if state.blend_2d else "")
                        command.extend(["--sm-blend", state.state_name.strip(), variables, state.playback, clips])
                    else:
                        command.extend(["--sm-state", state.state_name.strip(), str(state.clip_index), state.playback])
                    if state.speed_from_variable:
                        command.extend(["--sm-speed", state.state_name.strip(), "var:" + str(state.speed_variable)])
                    elif state.speed != 1.0:
                        command.extend(["--sm-speed", state.state_name.strip(), repr(float(state.speed))])
                    if state.layer and not state.empty:
                        command.extend(["--sm-layer", state.state_name.strip(), str(state.layer)])
                    bones = ",".join(part.strip().replace(" ", "") for part in state.bones.split(",") if part.strip())
                    if bones:
                        command.extend(["--sm-mask", state.state_name.strip(), bones])
                    if state.additive and not state.empty:
                        command.extend(["--sm-additive", state.state_name.strip()])
                    timed = ",".join(part.strip().replace(" ", "") for part in state.events_at.split(",") if part.strip())
                    if timed and not state.empty:
                        command.extend(["--sm-events", state.state_name.strip(), timed])
                    if state.exit_event.strip() and not state.empty:
                        command.extend(["--sm-exit", state.state_name.strip(), state.exit_event.strip(),
                                        repr(float(state.exit_blend))])
                for variable in options.state_graph_variables:
                    command.extend(["--sm-variable", variable.variable_name.strip(),
                                    str(variable.initial_value), str(variable.minimum), str(variable.maximum)])
                game_events = state_graph.game_events(options)
                if game_events:
                    command.extend(["--sm-declare-events", ",".join(game_events)])
                for transition in state_graph.expand(options):
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
            if sum(len(part) + 3 for part in command) > 8000:
                # more than a Windows command line holds (big state machines): the arguments go in a file
                arguments_path = os.path.splitext(glb_path)[0] + ".arguments.txt"
                with open(arguments_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write("\n".join(command) + "\n")
                command = ["@" + _windows_path(arguments_path, wine_tools)]
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
            breed_note = ""
            if breed_lua is not None:
                breed_path = os.path.join(output_dir, options.breed_name.strip() + "_breed.lua")
                with open(breed_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(breed_lua)
                breed_note = "; breed in " + breed_path
            if collision_notes:
                self.report({"WARNING"}, collision_notes[0][:180] + "; see " + log_path + breed_note)
            else:
                self.report({"INFO"}, "Asset built; see " + log_path + breed_note)
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


class DARKTIDE_OT_import_game_unit(bpy.types.Operator):
    bl_idname = "darktide.import_game_unit"
    bl_label = "Import Game Unit Nodes"
    bl_description = ("Add the attach points, effect points and animated nodes of a game unit as named empties, "
                      "placed where the game has them")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.dt_asset
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = settings.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        collection = settings.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        try:
            count, unknown = game_unit.import_nodes(context, collection, folder, resource)
        except (OSError, ReferenceSkeletonError, ValueError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        note = (", " + str(unknown) + " only known by hash (#...), keep those names as they are") if unknown else ""
        self.report({"INFO"}, "Imported " + str(count) + " nodes into " + collection.name + note)
        return {"FINISHED"}


class DARKTIDE_OT_import_game_collision(bpy.types.Operator):
    bl_idname = "darktide.import_game_collision"
    bl_label = "Import Game Collision (EXPERIMENTAL)"
    bl_description = ("Copy a game unit's hit zones, ragdoll and mover: shape meshes on your rig's bones (by bone "
                      "name), ragdoll joints with the game's limits, and the mover into this asset's collision settings")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.dt_asset
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = settings.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        collection = settings.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        try:
            ragdoll = None
            unit_path = os.path.join(folder, "%016x.unit" % _murmur64(resource))
            if os.path.isfile(unit_path):
                # the ragdoll lives in the unit's PhysX collection, which the compiler reads
                compiler = _compiler_path()
                wine_tools = _wine_tools(compiler)
                result = _run_compiler(compiler, ["--physics-collection", _windows_path(unit_path, wine_tools)], wine_tools,
                                       capture_output=True, text=True, check=False)
                if result.returncode == 0:
                    ragdoll = json.loads(result.stdout)

                def decode(meshes):
                    # cooked convex / triangle shapes, through the compiler's PhysX
                    path = os.path.join(bpy.app.tempdir or os.path.dirname(unit_path), "darktide_cooked_shapes.bin")
                    with open(path, "wb") as handle:
                        for kind, cooked in meshes:
                            handle.write(struct.pack("<II", kind, len(cooked)) + cooked)
                    decoded = _run_compiler(compiler, ["--physics-meshes", _windows_path(path, wine_tools)], wine_tools,
                                            capture_output=True, text=True, check=False)
                    if decoded.returncode:
                        raise ValueError("The compiler could not read the game's collision meshes: " +
                                         (decoded.stderr or "").strip()[:150])
                    return json.loads(decoded.stdout)
            else:
                decode = None
            count, skipped, mover, bodies, nodes = game_unit.import_collision(context, collection, folder, resource,
                                                                       _asset_options(context.scene), ragdoll, decode)
        except (OSError, ReferenceSkeletonError, ValueError, json.JSONDecodeError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        text = "Imported " + str(count) + " hit zone and actor shapes"
        if bodies:
            text += ", a ragdoll of " + str(bodies) + " bodies (ragdoll event: " + _asset_options(context.scene).ragdoll_event + ")"
        if mover:
            text += " and the mover"
        if nodes:
            text += ", " + str(nodes) + " of its nodes your rig didn't have"
        if skipped:
            text += ", left out " + str(skipped) + " mesh-shaped ones"
        self.report({"INFO"}, text)
        return {"FINISHED"}


class DARKTIDE_OT_import_game_events(bpy.types.Operator):
    bl_idname = "darktide.import_game_events"
    bl_label = "Import Game Events (EXPERIMENTAL)"
    bl_description = ("List every event and variable of a game unit's state machine (type the unit, e.g. an enemy's "
                      "base unit, or the state machine itself), so your state machine takes everything the game's "
                      "scripts for that character send")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = context.scene.dt_asset.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        try:
            machine, events, variables = game_state_machine.load(folder, resource)
        except (OSError, ReferenceSkeletonError, ValueError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        options = _asset_options(context.scene)
        options.create_state_graph = True
        options.state_graph_game_events = ",".join(events)
        options.state_graph_game_events_source = machine
        have = {variable.variable_name.strip() for variable in options.state_graph_variables}
        added = 0
        for name, default, low, high in variables:
            if name in have:
                continue
            variable = options.state_graph_variables.add()
            variable.variable_name, variable.initial_value, variable.minimum, variable.maximum = name, default, low, high
            added += 1
        unnamed = sum(1 for event in events if event.startswith("#"))
        self.report({"INFO"}, str(len(events)) + " events (" + str(unnamed) + " only known by hash) and " +
                    str(added) + " new variables from " + machine)
        return {"FINISHED"}


class DARKTIDE_OT_clear_game_events(bpy.types.Operator):
    bl_idname = "darktide.clear_game_events"
    bl_label = "Clear Game Events"
    bl_description = "Stop listing the imported game events"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        options = _asset_options(context.scene)
        options.state_graph_game_events = ""
        options.state_graph_game_events_source = ""
        return {"FINISHED"}


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

    # Parent, armature and physics links are dependencies, but traversing their children
    # would pull unrelated siblings into an asset selected by one child mesh.
    pending = list(objects)
    while pending:
        obj = pending.pop()
        settings = obj.dt_collider
        dependencies = [obj.parent, settings.body, settings.body_a, settings.body_b]
        dependencies += [modifier.object for modifier in obj.modifiers if modifier.type == "ARMATURE"]
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


class DARKTIDE_OT_state_bones_from_selection(bpy.types.Operator):
    bl_idname = "darktide.state_bones_from_selection"
    bl_label = "From Selected Bones"
    bl_description = ("Fill Bones with the bones selected on the asset's rig (pose mode). A bone brings everything "
                      "below it, so only the topmost selected ones are listed; weights you typed stay")
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        collection = context.scene.dt_asset.asset_collection
        rigs = [obj for obj in (collection.all_objects if collection else []) if obj.type == "ARMATURE"]
        selected = [pose_bone.bone for rig in rigs for pose_bone in rig.pose.bones if pose_bone.select]
        if not selected:
            self.report({"ERROR"}, "Select the bones on the rig first (pose mode, click or box select)")
            return {"CANCELLED"}
        picked = set(selected)
        top = [bone for bone in selected if not any(parent in picked for parent in bone.parent_recursive)]
        state = options.state_graph_states[self.index]
        weights = {}
        for entry in state.bones.split(","):
            name, _, weight = entry.strip().partition(":")
            if name and weight:
                weights[name] = weight
        state.bones = ", ".join(bone.name + (":" + weights[bone.name] if bone.name in weights else "") for bone in top)
        return {"FINISHED"}


class DARKTIDE_OT_load_shader_settings(bpy.types.Operator):
    bl_idname = "darktide.load_shader_settings"
    bl_label = "Show Shader Settings"
    bl_description = "List this game shader's settings and texture slots to edit (values typed in older files carry over)"

    def execute(self, context):
        _shader_rows(context.object.active_material.dt_material)
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


def _aim_turn(bone):
    """The bones an aiming bone turns, as {name: weight}; empty means its parent at full weight."""
    turn = {}
    for part in bone.dt_aim.turn.split(","):
        name, _, weight = part.strip().partition(":")
        if not name.strip():
            continue
        if name.strip() not in bone.id_data.bones:
            raise ValueError("Aim on bone '" + bone.name + "' turns '" + name.strip() + "', which is not a bone of this armature")
        try:
            value = float(weight) if weight.strip() else 1.0
        except ValueError:
            raise ValueError("Aim on bone '" + bone.name + "': '" + part.strip() + "' is not bone or bone:weight") from None
        if not 0.0 <= value <= 1.0:
            raise ValueError("Aim on bone '" + bone.name + "': weights go from 0 to 1")
        if name.strip() == bone.name:
            raise ValueError("Aim on bone '" + bone.name + "' can't turn itself, it's the end that points")
        turn[name.strip()] = value
    if not turn:
        if bone.parent is None:
            raise ValueError("Aim on bone '" + bone.name + "' needs bones to turn (it has no parent)")
        turn[bone.parent.name] = 1.0
    if not bone.dt_aim.target.strip():
        raise ValueError("Aim on bone '" + bone.name + "' needs a target name")
    return turn


class DARKTIDE_PT_bone_aim(bpy.types.Panel):
    bl_label = "Darktide Aim"
    bl_idname = "DARKTIDE_PT_bone_aim"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "bone"

    @classmethod
    def poll(cls, context):
        return context.bone is not None

    def draw_header(self, context):
        self.layout.prop(context.bone.dt_aim, "enabled", text="")

    def draw(self, context):
        aim = context.bone.dt_aim
        column = self.layout.column()
        column.active = aim.enabled
        column.prop(aim, "target")
        column.prop(aim, "turn")
        column.label(text="Put this bone where the pointing ends (eye, muzzle)")


class DARKTIDE_PT_bone_dangle(bpy.types.Panel):
    bl_label = "Darktide Dangle"
    bl_idname = "DARKTIDE_PT_bone_dangle"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "bone"

    @classmethod
    def poll(cls, context):
        return context.bone is not None

    def draw_header(self, context):
        self.layout.prop(context.bone.dt_dangle, "enabled", text="")

    def draw(self, context):
        dangle = context.bone.dt_dangle
        column = self.layout.column()
        column.active = dangle.enabled
        column.prop(dangle, "mode", expand=True)
        names = (("mass", "gravity", "jiggle_stiffness", "jiggle_damping", "max_stretch") if dangle.mode == "jiggle"
                 else ("length", "mass", "gravity", "damping", "stiffness", "max_angle"))
        for name in names:
            column.prop(dangle, name)
        column.label(text="Skin the swinging mesh to this bone")


def draw_setup(layout, context):
    """The paths of this computer (add-on preferences) and the project's output folder."""
    settings = prefs()
    layout.prop(settings, "compiler")
    layout.prop(settings, "game_folder")
    layout.prop(settings, "game_extract_folder")
    if context.scene is not None:
        layout.prop(context.scene.dt_asset, "output_path")
        layout.prop(context.scene.dt_asset, "compile_timeout")


class _SidebarPanel:
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Darktide"


class DARKTIDE_PT_setup(_SidebarPanel, bpy.types.Panel):
    bl_label = "Setup"
    bl_idname = "DARKTIDE_PT_setup"
    bl_order = 0

    def draw(self, context):
        settings = prefs()
        layout = self.layout
        draw_setup(layout, context)
        if not settings.compiler.strip():
            layout.label(text="Set the compiler to DarktideGLBCompiler.exe", icon="INFO")
        if not settings.game_folder.strip():
            layout.label(text="Set the Darktide folder (textures and game shaders need it)", icon="INFO")


class DARKTIDE_PT_asset_panel(_SidebarPanel, bpy.types.Panel):
    bl_label = "Asset"
    bl_idname = "DARKTIDE_PT_asset_panel"
    bl_order = 1

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        settings = scene.dt_asset
        collection = settings.asset_collection
        options = _asset_options(scene)
        layout.operator(DARKTIDE_OT_create_asset_collection.bl_idname)
        if collection and collection.dt_asset_identity.owned_by_addon:
            layout.operator(DARKTIDE_OT_update_asset_collection.bl_idname)
        layout.prop(settings, "asset_collection")
        if collection:
            layout.prop(collection.dt_asset_identity, "asset_path")
        else:
            layout.prop(settings, "asset_path", text="Initial resource path")
            layout.operator(DARKTIDE_OT_create_asset_collection.bl_idname, text="New Empty Asset")
            layout.label(text="Create an asset collection before export or compile")
        layout.prop(options, "asset_filename")
        layout.prop(options, "scale")
        layout.prop(options, "weapon_materials")
        layout.prop(options, "output_kind")
        if options.output_kind != "model":
            row = layout.row(align=True)
            row.prop(options, "select_clip")
            if options.select_clip:
                row.prop(options, "clip_index")
        layout.operator(DARKTIDE_OT_inspect.bl_idname)
        if options.inspection_summary:
            inspection = layout.box()
            inspection.label(text="Compiler source view (last inspection)")
            for line in options.inspection_summary.splitlines():
                inspection.label(text=line)
        layout.operator(DARKTIDE_OT_export_intermediate.bl_idname)
        layout.operator(DARKTIDE_OT_build.bl_idname)


class DARKTIDE_PT_animation(_SidebarPanel, bpy.types.Panel):
    bl_label = "Animation"
    bl_idname = "DARKTIDE_PT_animation"
    bl_order = 2
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        options = _asset_options(context.scene)
        if options.output_kind == "model":
            layout.label(text="Output kind Model leaves the animations out")
            return
        simple_row = layout.row(align=True)
        simple_row.enabled = options.output_kind == "all"
        simple_row.prop(options, "embed_simple_animation")
        if options.embed_simple_animation:
            simple_row.prop(options, "simple_animation_clip", text="Clip")
        travel_row = layout.row(align=True)
        travel_row.prop(options, "in_place")
        travel_row.prop(options, "root_motion")
        loop_row = layout.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "auto_loop_single_clip")
        loop_row = layout.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "create_looping_state_machine")
        graph_row = layout.row()
        graph_row.enabled = options.output_kind == "all"
        graph_row.prop(options, "create_state_graph")
        if options.output_kind != "all":
            layout.label(text="State machines need Output kind All", icon="ERROR")
        elif options.create_looping_state_machine and options.create_state_graph:
            layout.label(text="Choose one animation controller", icon="ERROR")
        elif options.create_looping_state_machine:
            row = layout.row(align=True)
            row.prop(options, "loop_clip_index", text="Clip")
            row.prop(options, "state_machine_playback", text="")
        elif options.auto_loop_single_clip and not options.create_state_graph:
            layout.label(text="A looping controller is added only when the exported GLB has one clip")
        elif options.create_state_graph:
            _draw_state_graph(layout, options)
            state_machine_preview.draw(layout, options)
        layout.prop(options, "animation_fit_advanced", text="Animation fit settings")
        if options.animation_fit_advanced:
            fit = layout.box()
            fit.prop(options, "animation_translation_tolerance")
            fit.prop(options, "animation_scale_tolerance")
            fit.prop(options, "animation_rotation_tolerance_radians")


def _draw_state_graph(layout, options):
    layout.label(text="State 0 is the initial state (higher layers start in their first state)")
    states = options.state_graph_states
    for index, state in enumerate(states):
        state_box = layout.box()
        row = state_box.row(align=True)
        row.label(text="State " + str(index))
        row.prop(state, "state_name", text="")
        remove = row.operator(DARKTIDE_OT_remove_animation_state.bl_idname, text="", icon="X")
        remove.index = index
        row = state_box.row(align=True)
        row.prop(state, "layer")
        row.prop(state, "empty")
        if not state.empty:
            row.prop(state, "additive")
            row = state_box.row(align=True)
            row.prop(state, "bones")
            pick = row.operator(DARKTIDE_OT_state_bones_from_selection.bl_idname, text="", icon="BONE_DATA")
            pick.index = index
            row = state_box.row(align=True)
            row.prop(state, "blend")
            row.prop(state, "random")
        if state.empty:
            pass
        elif state.random:
            row = state_box.row(align=True)
            row.prop(state, "random_pick", text="")
            row.prop(state, "playback", text="")
            state_box.prop(state, "blend_clips")
        elif state.blend:
            row.prop(state, "blend_2d")
            row = state_box.row(align=True)
            row.prop(state, "blend_variable_pick", text="X" if state.blend_2d else "")
            if state.blend_2d:
                row.prop(state, "blend_variable2_pick", text="Y")
            row.prop(state, "playback", text="")
            state_box.prop(state, "blend_clips")
        else:
            row.prop(state, "clip_pick", text="")
            row.prop(state, "playback", text="")
        if not state.empty:
            row = state_box.row(align=True)
            row.prop(state, "speed_from_variable", text="Speed from variable")
            row.prop(state, "speed_variable_pick" if state.speed_from_variable else "speed", text="")
            state_box.prop(state, "events_at")
            row = state_box.row(align=True)
            row.prop(state, "exit_event")
            if state.exit_event.strip():
                row.prop(state, "exit_blend")
    layout.operator(DARKTIDE_OT_add_animation_state.bl_idname, icon="ADD")
    layout.label(text="Float variables", icon="DRIVER")
    for index, variable in enumerate(options.state_graph_variables):
        variable_box = layout.box()
        row = variable_box.row(align=True)
        row.label(text="Variable " + str(index))
        row.prop(variable, "variable_name", text="")
        remove = row.operator(DARKTIDE_OT_remove_animation_variable.bl_idname, text="", icon="X")
        remove.index = index
        variable_box.prop(variable, "initial_value")
        row = variable_box.row(align=True)
        row.prop(variable, "minimum")
        row.prop(variable, "maximum")
    layout.operator(DARKTIDE_OT_add_animation_variable.bl_idname, icon="ADD")
    layout.label(text="Transitions", icon="DRIVER_TRANSFORM")
    for index, transition in enumerate(options.state_graph_transitions):
        transition_box = layout.box()
        row = transition_box.row(align=True)
        if transition.from_any:
            row.label(text="From any state")
        else:
            row.prop(transition, "from_pick")
        row.prop(transition, "from_any", text="", icon="WORLD")
        transition_box.prop(transition, "to_pick")
        transition_box.prop(transition, "event_name")
        transition_box.prop(transition, "condition")
        if transition.condition == "range":
            transition_box.prop(transition, "variable_pick")
            row = transition_box.row(align=True)
            row.prop(transition, "lower")
            row.prop(transition, "upper")
            row = transition_box.row(align=True)
            row.prop(transition, "lower_exclusive")
            row.prop(transition, "upper_exclusive")
        row = transition_box.row(align=True)
        row.prop(transition, "blend_seconds")
        remove = row.operator(DARKTIDE_OT_remove_animation_transition.bl_idname, text="", icon="X")
        remove.index = index
    layout.operator(DARKTIDE_OT_add_animation_transition.bl_idname, icon="ADD")
    layout.label(text="Event names take * (stagger_* is every stagger event).")
    game = layout.box()
    game.label(text="Game events", icon="IMPORT")
    game.prop(bpy.context.scene.dt_asset, "game_unit", text="")
    game.operator(DARKTIDE_OT_import_game_events.bl_idname)
    events = state_graph.game_events(options)
    if events:
        row = game.row(align=True)
        row.label(text=str(len(events)) + " from " + options.state_graph_game_events_source.rsplit("/", 1)[-1])
        row.operator(DARKTIDE_OT_clear_game_events.bl_idname, text="", icon="X")


class DARKTIDE_PT_flow(_SidebarPanel, bpy.types.Panel):
    bl_label = "Flow"
    bl_idname = "DARKTIDE_PT_flow"
    bl_order = 3
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        options = _asset_options(context.scene)
        row = self.layout.row(align=True)
        row.prop(options, "flow_tree")
        row.operator(DARKTIDE_OT_new_flow.bl_idname, text="", icon="ADD")
        self.layout.label(text="Edit it in a node editor set to Darktide Flow")


class DARKTIDE_PT_skeleton(_SidebarPanel, bpy.types.Panel):
    bl_label = "Skeleton"
    bl_idname = "DARKTIDE_PT_skeleton"
    bl_order = 4
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        collection = context.scene.dt_asset.asset_collection
        options = _asset_options(context.scene)
        if not prefs().game_extract_folder.strip():
            layout.label(text="Set 'Game files extract' in Setup first", icon="INFO")
        if collection is None:
            layout.operator(fit_skeleton.DARKTIDE_OT_new_starter_asset.bl_idname, icon="MESH_CAPSULE")
            layout.label(text="Or make an asset first for the reference skeleton and fit tools")
            return
        identity = collection.dt_asset_identity
        layout.prop(identity, "skeleton_preset")
        if identity.skeleton_preset_status:
            layout.label(text=identity.skeleton_preset_status[:90], icon="ERROR")
        layout.prop(identity, "reference_unit")
        layout.prop(identity, "reference_bones")
        layout.prop(identity, "skeleton_resource")
        layout.operator(DARKTIDE_OT_import_reference_skeleton.bl_idname)
        layout.prop(options, "only_weighted_bones")
        layout.label(text="Imported armature is added to this asset collection")
        fit_skeleton.draw_fit_panel(layout, context, collection)
        retarget.draw(layout, collection)


class DARKTIDE_PT_game_unit(_SidebarPanel, bpy.types.Panel):
    bl_label = "Game Unit"
    bl_idname = "DARKTIDE_PT_game_unit"
    bl_order = 5
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return context.scene.dt_asset.asset_collection is not None

    def draw(self, context):
        layout = self.layout
        if not prefs().game_extract_folder.strip():
            layout.label(text="Set 'Game files extract' in Setup first", icon="INFO")
        layout.prop(context.scene.dt_asset, "game_unit", text="")
        layout.operator(DARKTIDE_OT_import_game_unit.bl_idname)
        layout.operator(DARKTIDE_OT_import_game_collision.bl_idname)
        options = _asset_options(context.scene)
        layout.prop(options, "unit_package")
        box = layout.box()
        box.prop(options, "make_breed")
        if options.make_breed:
            box.prop(options, "breed_name")
            box.prop(options, "breed_base")
            box.prop(options, "breed_body")
            box.prop(options, "breed_health")
            row = box.row(align=True)
            row.prop(options, "breed_walk_speed")
            row.prop(options, "breed_run_speed")


class DARKTIDE_PT_object(_SidebarPanel, bpy.types.Panel):
    bl_label = "Active Object"
    bl_idname = "DARKTIDE_PT_object"
    bl_order = 6

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def draw(self, context):
        layout = self.layout
        obj = context.object
        layout.label(text=obj.name, icon="OBJECT_DATA")
        layout.prop(obj.dt_collider, "visibility_group")
        row = layout.row(align=True)
        row.prop(obj.dt_collider, "render_visible")
        row.prop(obj.dt_collider, "render_shadow")
        layout.prop(obj.dt_collider, "lod_group")
        if obj.dt_collider.lod_group.strip():
            row = layout.row(align=True)
            row.prop(obj.dt_collider, "lod_level")
            row.prop(obj.dt_collider, "lod_down_to")
        if obj.type == "EMPTY":
            particle_effect.draw(layout, obj)


class DARKTIDE_PT_physics(_SidebarPanel, bpy.types.Panel):
    bl_label = "Collision and Physics"
    bl_idname = "DARKTIDE_PT_physics"
    bl_order = 7
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        collection = scene.dt_asset.asset_collection
        options = _asset_options(scene)
        asset_objects = list(collection.all_objects) if collection else []
        version2 = _v2_objects(asset_objects) if asset_objects else False
        if not version2:
            body = layout.box()
            body.label(text="Whole asset")
            body.prop(options, "visible_meshes_collide", text="Use visible meshes as collision")
            if options.visible_meshes_collide or any(o.dt_collider.role != "render" for o in asset_objects):
                body.prop(options, "actor")
                if options.actor == "dynamic":
                    body.prop(options, "mass")
                body.prop(options, "material")
            body.label(text="Static uses the authored mesh surface")
        ragdoll_row = layout.row()
        ragdoll_row.enabled = options.output_kind == "all"
        ragdoll_row.prop(options, "ragdoll_event")
        mover = layout.box()
        mover.prop(options, "use_mover")
        if options.use_mover:
            row = mover.row(align=True)
            row.prop(options, "mover_height")
            row.prop(options, "mover_radius")
            mover.prop(options, "mover_slope")
            mover.prop(options, "mover_filter")
        notes = _collision_review(scene, asset_objects) if asset_objects else []
        if notes:
            review = layout.box()
            review.label(text="Collision review")
            for message in notes[:3]:
                review.label(text=message)
        obj = context.object
        if obj is None:
            return
        collider = layout.box()
        collider.label(text="Active object: " + obj.name)
        if obj.type == "MESH":
            collider.prop(obj.dt_collider, "unit_actor")
            if obj.dt_collider.unit_actor != "none":
                collider.prop(obj.dt_collider, "actor_shape")
                collider.prop(obj.dt_collider, "actor_name")
                if obj.dt_collider.unit_actor == "custom":
                    collider.prop(obj.dt_collider, "actor_template")
                    collider.prop(obj.dt_collider, "actor_shape_template")
                if options.make_breed:
                    collider.prop(obj.dt_collider, "hit_zone_group")
                collider.prop(obj.dt_collider, "actor_spawn")
                return
        collider.prop(obj.dt_collider, "is_body")
        if obj.dt_collider.is_body:
            collider.prop(obj.dt_collider, "actor")
            if obj.dt_collider.actor == "dynamic":
                collider.prop(obj.dt_collider, "mass")
            collider.prop(obj.dt_collider, "material")
        if obj.type == "MESH":
            collider.prop(obj.dt_collider, "role")
            if options.visible_meshes_collide and obj.dt_collider.role == "render":
                collider.prop(obj.dt_collider, "use_asset_collision")
            if obj.dt_collider.role != "render":
                collider.prop(obj.dt_collider, "shape")
                if version2:
                    collider.prop(obj.dt_collider, "body")
            if collection and collection.all_objects.get(obj.name) == obj:
                collider.operator(DARKTIDE_OT_make_collision_proxy.bl_idname)
        joint = obj.dt_collider
        if version2 or obj.type == "EMPTY":
            box = layout.box()
            box.label(text="Joint")
            box.prop(joint, "joint_kind")
            if joint.joint_kind != "none":
                box.prop(joint, "body_a")
                box.prop(joint, "body_b")
                if joint.joint_kind in {"hinge", "ragdoll"}:
                    box.prop(joint, "hinge_limits")
                    if joint.hinge_limits:
                        box.prop(joint, "twist_min")
                        box.prop(joint, "twist_max")
                if joint.joint_kind == "ragdoll":
                    box.prop(joint, "swing_y")
                    box.prop(joint, "swing_z")
                if joint.joint_kind == "slider":
                    box.prop(joint, "slide_limits")
                    if joint.slide_limits:
                        box.prop(joint, "slide_min")
                        box.prop(joint, "slide_max")
                if joint.joint_kind != "fixed":
                    row = box.row(align=True)
                    row.prop(joint, "spring_stiffness")
                    row.prop(joint, "spring_damping")
                row = box.row(align=True)
                row.prop(joint, "break_force")
                row.prop(joint, "break_torque")


class DARKTIDE_PT_material(_SidebarPanel, bpy.types.Panel):
    bl_label = "Active Material"
    bl_idname = "DARKTIDE_PT_material"
    bl_order = 8

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.active_material is not None

    def draw(self, context):
        layout = self.layout
        material = context.object.active_material
        settings = material.dt_material
        layout.label(text=material.name, icon="MATERIAL")
        layout.prop(settings, "mode")
        if settings.mode not in ("external", "template"):
            layout.prop(settings, "double_sided")
        if settings.mode == "external":
            layout.prop(settings, "resource")
        elif settings.mode == "game_shader":
            if not prefs().game_folder.strip():
                layout.label(text="Set the Darktide folder in Setup first", icon="INFO")
            layout.prop(settings, "game_shader")
            preset = _game_shaders().get(settings.game_shader)
            if preset and (len(settings.shader_value_rows) != len(preset["variables"]) or
                           len(settings.shader_texture_rows) != len(preset["textures"])):
                layout.operator(DARKTIDE_OT_load_shader_settings.bl_idname, icon="FILE_REFRESH")
            elif preset:
                box = layout.box()
                box.label(text="Settings (tick to change, unticked keeps the game's)")
                for row in settings.shader_value_rows:
                    line = box.row(align=True)
                    line.prop(row, "changed", text="")
                    line.label(text=row.name)
                    values = line.row(align=True)
                    values.active = row.changed
                    if row.is_color:
                        values.prop(row, "color", text="")
                    else:
                        for index in range(row.floats):
                            values.prop(row, "value", index=index, text="")
                if settings.shader_texture_rows:
                    box = layout.box()
                    box.label(text="Textures (images from this material's own nodes)")
                    for row in settings.shader_texture_rows:
                        line = box.row(align=True)
                        line.label(text=row.name)
                        line.prop(row, "source", text="")
                if settings.shader_values or settings.shader_textures:
                    box = layout.box()
                    box.label(text="Other settings, typed by name")
                    box.prop(settings, "shader_values")
                    box.prop(settings, "shader_textures")
        elif settings.mode == "donor":
            layout.prop(settings, "donor_stream")
            for toggle, value in (("override_nm_r_blend", "nm_r_blend"),
                                  ("override_shared_blend", "shared_blend"),
                                  ("override_bc_blend", "bc_blend")):
                row = layout.row(align=True)
                row.prop(settings, toggle, text="")
                row.prop(settings, value)
        else:
            layout.prop(settings, "surface")


SIDEBAR_PANELS = (DARKTIDE_PT_setup, DARKTIDE_PT_asset_panel, DARKTIDE_PT_animation, DARKTIDE_PT_flow,
                  DARKTIDE_PT_skeleton, DARKTIDE_PT_game_unit, DARKTIDE_PT_object, DARKTIDE_PT_physics,
                  DARKTIDE_PT_material)


def _collection_data(collection):
    """The asset collection's custom properties as the unit's script data (Unit.get_data): text, numbers,
    true/false, groups (nested tables) and lists."""
    if collection is None:
        return {}

    def plain(value):
        if hasattr(value, "to_dict"):
            return {key: plain(item) for key, item in value.to_dict().items()}
        if hasattr(value, "to_list"):
            return [plain(item) for item in value.to_list()]
        if isinstance(value, dict):
            return {key: plain(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [plain(item) for item in value]
        if isinstance(value, (bool, str)):
            return value
        if isinstance(value, (int, float)):
            return float(value)
        raise ValueError("Unit data can hold text, numbers, true/false, groups and lists, not " + type(value).__name__)

    registered = set(collection.bl_rna.properties.keys())
    return {key: plain(value) for key, value in collection.items()
            if key not in registered and not key.startswith("_") and key != SCHEMA_KEY}


class DARKTIDE_OT_new_flow(bpy.types.Operator):
    """Make a new flow for this asset; edit it in the node editor (Darktide Flow)"""
    bl_idname = "darktide.new_flow"
    bl_label = "New Flow"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        options = _asset_options(context.scene)
        tree = bpy.data.node_groups.new("Unit flow", flow_editor.TREE)
        spawned = tree.nodes.new("DarktideFlowNode_unit_spawned")
        spawned.location = (-300, 0)
        options.flow_tree = tree
        self.report({"INFO"}, "Open a node editor and switch it to Darktide Flow to edit " + tree.name)
        return {"FINISHED"}


CLASSES = (DarktidePreferences, *fit_skeleton.CLASSES, *particle_effect.CLASSES, DarktideAnimationState, DarktideAnimationVariable, DarktideAnimationTransition,
           DarktideSceneSettings, DarktideCollectionSettings, DarktideShaderValue, DarktideShaderTexture, DarktideMaterialSettings, DarktideDangleSettings, DarktideAimSettings, DarktideColliderSettings,
           DARKTIDE_OT_create_asset_collection, DARKTIDE_OT_update_asset_collection,
           DARKTIDE_OT_add_animation_state, DARKTIDE_OT_remove_animation_state, DARKTIDE_OT_state_bones_from_selection, DARKTIDE_OT_load_shader_settings,
           DARKTIDE_OT_add_animation_variable, DARKTIDE_OT_remove_animation_variable,
           DARKTIDE_OT_add_animation_transition, DARKTIDE_OT_remove_animation_transition,
           DARKTIDE_OT_make_collision_proxy, DARKTIDE_OT_export_intermediate, DARKTIDE_OT_new_flow,
           DARKTIDE_OT_inspect, DARKTIDE_OT_build, DARKTIDE_OT_import_reference_skeleton, DARKTIDE_OT_import_game_unit, DARKTIDE_OT_import_game_collision,
           DARKTIDE_OT_import_game_events, DARKTIDE_OT_clear_game_events,
           *fit_skeleton.UI_CLASSES, *retarget.CLASSES, *SIDEBAR_PANELS, DARKTIDE_PT_bone_dangle, DARKTIDE_PT_bone_aim)


def register():
    flow_editor.register()
    particle_preview.register()
    state_machine_preview.register()
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.dt_asset = PointerProperty(type=DarktideSceneSettings)
    bpy.types.Object.dt_collider = PointerProperty(type=DarktideColliderSettings)
    bpy.types.Collection.dt_asset_identity = PointerProperty(type=DarktideCollectionSettings)
    bpy.types.Material.dt_material = PointerProperty(type=DarktideMaterialSettings)
    bpy.types.Bone.dt_dangle = PointerProperty(type=DarktideDangleSettings)
    bpy.types.Bone.dt_aim = PointerProperty(type=DarktideAimSettings)
    bpy.types.Object.dt_particles = PointerProperty(type=particle_effect.DarktideParticleSettings)


def unregister():
    del bpy.types.Bone.dt_dangle
    del bpy.types.Bone.dt_aim
    del bpy.types.Object.dt_particles
    del bpy.types.Object.dt_collider
    del bpy.types.Collection.dt_asset_identity
    del bpy.types.Material.dt_material
    del bpy.types.Scene.dt_asset
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    state_machine_preview.unregister()
    particle_preview.unregister()
    flow_editor.unregister()


if __name__ == "__main__":
    register()
