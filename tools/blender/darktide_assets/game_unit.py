"""Import the nodes of any game unit (gear, weapon parts, props, characters) as named empties.

The game finds attach points (ap_*), effect points (fx_*) and animated nodes by name, so a replacement unit
has to carry the same names in the same places. Names come from every BONES file in the extract folder plus
common attach/effect words; a node whose name is unknown keeps its hash as "#1234abcd", which the compiler
writes back as that hash.
"""
import glob
import math
import os

import bmesh
import bpy
from mathutils import Matrix, Quaternion, Vector

from . import breed
from .reference_skeleton import ReferenceSkeletonError, _murmur64, _parse_bones, _parse_unit, _read_unit

_PART_WORDS = (
    "barrel", "muzzle", "magazine", "sight", "stock", "grip", "receiver", "flashlight", "bayonet", "bullet",
    "rail", "underbarrel", "trinket", "handle", "body", "blade", "head", "shaft", "pommel", "guard", "chain",
    "emblem", "scope", "lens", "slide", "hammer", "trigger", "bolt", "charge", "shield", "sling", "strap",
    "anim", "recharge", "release", "safety", "light", "eject", "reload", "overheat", "sweep", "block",
)
_EXTRA_NAMES = (
    "root_point", "lod", "lod_shadow", "ap_emblem_left", "ap_emblem_right", "ap_emblem_front", "ap_emblem_back",
    "fx_muzzle", "fx_eject", "fx_reload", "fx_overheat", "fx_sweep", "fx_block", "fx_special_active",
    "fx_weapon_special", "fx_charge", "fx_blade", "fx_shaft", "fx_shield", "fx_emit", "fx_source", "fx_base",
    "fx_display", "fx_data", "fx_left", "fx_right", "fx_center", "fx_engine", "fx_wpn_node_1", "fx_wpn_node_2",
)
_name_cache = {}


def _id32(name):
    return _murmur64(name) >> 32


def name_table(folder):
    """IdString32 -> name for every BONES name in the folder plus common attach/effect names."""
    key = os.path.normcase(os.path.abspath(folder))
    names = _name_cache.get(key)
    if names is None:
        names = {}
        for word in _PART_WORDS:
            for prefix in ("ap_", "fx_", "rp_"):
                for suffix in ("", "_01", "_02", "_03", "_04", "_05", "_06", "_07", "_08", "_09", "_10"):
                    names[_id32(prefix + word + suffix)] = prefix + word + suffix
        for name in _EXTRA_NAMES:
            names[_id32(name)] = name
        for path in glob.glob(os.path.join(folder, "*.bones")):
            try:
                with open(path, "rb") as source:
                    bone_names, _ = _parse_bones(source.read())
            except (OSError, ReferenceSkeletonError):
                continue
            for name in bone_names:
                names[_id32(name)] = name
        _name_cache[key] = names
    return names


def import_nodes(context, collection, folder, resource):
    """Create one empty per non-mesh node of the unit, named and placed like the game's."""
    stem = "%016x" % _murmur64(resource)
    unit_path = os.path.join(folder, stem + ".unit")
    if not os.path.isfile(unit_path):
        raise ValueError(resource + " is not in the extract folder (looked for " + stem + ".unit)")
    with open(unit_path, "rb") as source:
        nodes = _parse_unit(source.read())
    names = dict(name_table(folder))
    part = resource.rsplit("/", 1)[-1]
    names[_id32("rp_" + part)] = "rp_" + part

    taken = [names.get(node["hash"], "#%08x" % node["hash"]) for node in nodes if not node["mesh"]]
    clashes = sorted(name for name in taken if name in bpy.data.objects)
    if clashes:
        raise ValueError("Objects with these names already exist (the names must stay exact): " +
                         ", ".join(clashes[:6]))

    empties = {}
    for node in nodes:
        if node["mesh"]:
            continue
        name = names.get(node["hash"], "#%08x" % node["hash"])
        empty = bpy.data.objects.new(name, None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.03
        collection.objects.link(empty)
        empties[node["index"]] = empty
    for node in nodes:
        empty = empties.get(node["index"])
        if empty is None:
            continue
        parent_type, parent_index = node["parent_type"], node["parent_index"]
        # mesh nodes are not imported; hang a child of one on its nearest imported ancestor
        for _ in range(len(nodes)):
            if parent_type != 1 or parent_index in empties:
                break
            parent_type, parent_index = nodes[parent_index]["parent_type"], nodes[parent_index]["parent_index"]
        if node["index"] and parent_type == 1 and parent_index in empties:
            empty.parent = empties[parent_index]
        values = node["world"]
        empty.matrix_world = Matrix(tuple(tuple(values[row + column * 4] for column in range(4))
                                          for row in range(4)))
    context.view_layer.update()
    unknown = sum(1 for empty in empties.values() if empty.name.startswith("#"))
    return len(empties), unknown


# physics_properties names seen on game units (actor and shape templates, mover filters)
_TEMPLATE_NAMES = (
    "static", "dynamic", "keyframed", "default", "ragdoll", "minion_hit_box", "minion_afro", "filter_minion_mover",
    "filter_player_mover", "filter_companion_mover",
)
# the actor template of the game's enemy hit zones (keyframed, created at spawn; its name is not known)
HIT_ZONE_TEMPLATE = "#b36ba703"
HIT_ZONE_SHAPE_TEMPLATE = "minion_hit_box"
_SHAPES = {0: "sphere", 1: "box", 2: "capsule", 3: "geometry", 4: "convex"}


def _template_name(value):
    for name in _TEMPLATE_NAMES:
        if _id32(name) == value:
            return name
    return "#%08x" % value


def read_unit_physics(blob):
    """The scene nodes, UNIT actor records (name, templates, node, shapes, created at spawn) and movers of a game
    unit. movers is None when a section before them can't be read."""
    tail = read_unit_tail(blob)
    return tail["nodes"], tail["actors"], tail["movers"]


def read_unit_tail(blob):
    """nodes, actors, movers and state_machine (resource path, "" for none) of a game unit; movers and
    state_machine are None when a section before them can't be read."""
    nodes, reader = _read_unit(blob)

    def u32():
        return reader.u32()

    def f32():
        return reader.floats(1)[0]

    def actors():
        result = []
        for _ in range(u32()):
            name, template, node = u32(), u32(), u32()
            mass = f32()
            shapes = []
            for _ in range(u32()):
                kind, material, shape_template = u32(), u32(), u32()
                local = reader.floats(16)
                cooked = reader.take(u32())   # cooked PhysX triangle mesh / convex
                u32()               # shape node
                u32()
                values = ()
                if kind == 0:
                    values = (f32(),)
                elif kind == 1:
                    values = tuple(reader.floats(3))
                elif kind == 2:
                    values = (f32(), f32())
                elif kind == 5:
                    reader.take(21)
                elif kind not in (3, 4):
                    raise ReferenceSkeletonError("unknown actor shape type %d" % kind)
                shapes.append({"type": _SHAPES.get(kind, "other"), "kind": kind, "material": material,
                               "shape_template": shape_template, "local": local, "values": values,
                               "cooked": cooked})
            reader.take(24)
            spawn = reader.take(4)[0] == 1
            result.append({"name": name, "template": template, "node": node, "mass": mass,
                           "shapes": shapes, "spawn": spawn})
        return result

    unit_actors = actors()
    movers = state_machine = None
    try:
        if actors():                                      # actors_2
            raise ReferenceSkeletonError("second actor list")
        reader.u32_array()
        reader.take(32 * u32())                           # cameras
        reader.take(160 * u32())                          # lights
        reader.array()
        for _ in range(u32()):                            # LOD objects
            reader.take(16)
            for _ in range(u32()):
                reader.take(8)
                reader.u32_array()
                reader.take(8)
            reader.take(48)
            reader.u32_array()
            reader.take(5)
        if u32() or u32() or u32():                       # terrains, unused, joints
            raise ReferenceSkeletonError("terrain or joint records")
        movers = []
        for _ in range(u32()):
            name, height, radius, collision_filter, slope = u32(), f32(), f32(), u32(), f32()
            reader.take(8)
            movers.append({"name": name, "height": height, "radius": radius,
                           "collision_filter": collision_filter, "slope_limit": slope})
        if u32():                                         # unused
            raise ReferenceSkeletonError("unknown records")
        reader.take(1)                                    # animated
        size = u32()
        state_machine = reader.take(size).decode("utf-8")
    except (ReferenceSkeletonError, UnicodeDecodeError):
        pass
    return {"nodes": nodes, "actors": unit_actors, "movers": movers, "state_machine": state_machine}


def _shape_mesh(name, shape):
    """A mesh the compiler fits back to this exact shape (vertices in the actor node's frame)."""
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    kind, values = shape["type"], shape["values"]
    if "mesh" in shape:
        points, triangles = shape["mesh"]["vertices"], shape["mesh"]["triangles"]
        verts = [bm.verts.new(points[i:i + 3]) for i in range(0, len(points), 3)]
        for i in range(0, len(triangles), 3):
            try:
                bm.faces.new([verts[j] for j in triangles[i:i + 3]])
            except ValueError:
                pass    # a face bmesh already has
    elif kind == "sphere":
        bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=8, radius=values[0])
    elif kind == "box":
        bmesh.ops.create_cube(bm, size=1.0)
        for vertex in bm.verts:
            vertex.co = Vector((vertex.co.x * 2 * values[0], vertex.co.y * 2 * values[1], vertex.co.z * 2 * values[2]))
    else:
        # capsule along x (PhysX): a cylinder of the given length between two half-sphere caps
        radius, height = values
        bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=8, radius=radius)
        bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, "Y"))
        for vertex in bm.verts:
            if abs(vertex.co.x) > 1e-6:
                vertex.co.x += math.copysign(height * 0.5, vertex.co.x)
    local = shape["local"]
    bm.transform(Matrix(tuple(tuple(local[row + column * 4] for column in range(4)) for row in range(4))))
    bm.to_mesh(mesh)
    bm.free()
    return mesh


def import_collision(context, collection, folder, resource, options, ragdoll=None, decode=None):
    """Hit zones and the other actors of a game unit as shape meshes named like its actors, hung on the asset's
    bones by name, its mover into the asset settings, and with ragdoll (the compiler's --physics-collection of the
    unit) its ragdoll. decode turns cooked convex / triangle shapes into meshes ([(kind, bytes)] -> [{vertices,
    triangles}], the compiler's --physics-meshes); without it those are left out.
    Also adds the unit's other nodes the asset is missing, as empties placed like the game's.
    Returns (shapes made, shapes skipped, mover found, ragdoll bodies made, nodes added)."""
    stem = "%016x" % _murmur64(resource)
    unit_path = os.path.join(folder, stem + ".unit")
    if not os.path.isfile(unit_path):
        raise ValueError(resource + " is not in the extract folder (looked for " + stem + ".unit)")
    with open(unit_path, "rb") as source:
        nodes, actors, movers = read_unit_physics(source.read())
    names = dict(name_table(folder))
    for name in list(names.values()):
        if name.startswith("j_"):
            names[_id32("c_" + name[2:])] = "c_" + name[2:]
    for name in ("tagging", "r_afro", "c_afro", "smart_tagging") + _TEMPLATE_NAMES:
        names[_id32(name)] = name
    by_hash = {node["hash"]: node for node in nodes}
    rigs = [obj for obj in collection.all_objects if obj.type == "ARMATURE"]

    def label(value):
        return names.get(value, "#%08x" % value)

    planned = []
    skipped = 0
    for actor in actors:
        shapes = [shape for shape in actor["shapes"] if shape["type"] in ("sphere", "box", "capsule") or
                  (decode is not None and shape["type"] in ("convex", "geometry"))]
        node = by_hash.get(actor["node"])
        skipped += len(actor["shapes"]) - (len(shapes) if node else 0)
        if node is None:
            continue
        for index, shape in enumerate(shapes):
            planned.append((actor, node, index, shape))
    cooked = [shape for _, _, _, shape in planned if shape["type"] in ("convex", "geometry")]
    for shape, mesh in zip(cooked, decode([(shape["kind"], shape["cooked"]) for shape in cooked]) if cooked else []):
        shape["mesh"] = mesh
    names_taken = [label(actor["name"]) + ("_" + str(index) if index else "") for actor, _, index, _ in planned]
    # the game unit's other nodes (attach points, end bones, hashed helpers) the asset doesn't have: the breed's
    # items and scripts look them up by name
    bone_names = {bone.name for rig in rigs for bone in rig.data.bones}
    have = bone_names | {obj.name for obj in collection.all_objects} | set(names_taken)
    extra_nodes = [node for node in nodes if node["index"] and not node["mesh"] and label(node["hash"]) not in have]
    clashes = sorted(name for name in names_taken + [label(node["hash"]) for node in extra_nodes]
                     if name in bpy.data.objects)
    if clashes:
        raise ValueError("Objects with these names already exist (the names must stay exact): " + ", ".join(clashes[:6]))

    for (actor, node, index, shape), object_name in zip(planned, names_taken):
        actor_name = label(actor["name"])
        world = Matrix(tuple(tuple(node["world"][row + column * 4] for column in range(4)) for row in range(4)))
        # an actor sits straight on a bone (tagging on j_hips) or on a node of its own under one (c_head under
        # j_head); the shape object goes under that bone
        rig, bone_name, current = None, "", node
        for _ in range(len(nodes)):
            bone_name = label(current["hash"])
            rig = next((rig for rig in rigs if rig.data.bones.get(bone_name)), None)
            if rig is not None or current["parent_type"] != 1:
                break
            current = nodes[current["parent_index"]]
        obj = bpy.data.objects.new(object_name, _shape_mesh(object_name, shape))
        obj.display_type = "WIRE"
        obj.hide_render = True
        collection.objects.link(obj)
        if rig is not None:
            obj.parent = rig
            obj.parent_type = "BONE"
            obj.parent_bone = bone_name
        obj.matrix_world = world
        settings = obj.dt_collider
        template, shape_template = _template_name(actor["template"]), _template_name(shape["shape_template"])
        if template == HIT_ZONE_TEMPLATE and shape_template == HIT_ZONE_SHAPE_TEMPLATE:
            settings.unit_actor = "hit_zone"
        else:
            settings.unit_actor = "custom"
            settings.actor_template = template
            settings.actor_shape_template = shape_template
        settings.actor_shape = shape["type"]
        settings.actor_spawn = actor["spawn"]
        # extra shapes of one actor become actors of their own, named after the object
        settings.actor_name = actor_name if index == 0 and object_name != actor_name else ""
    added = {}
    for node in extra_nodes:     # parents come before their children
        name = label(node["hash"])
        empty = bpy.data.objects.new(name, None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.03
        collection.objects.link(empty)
        added[name] = empty
        current = node
        for _ in range(len(nodes)):
            if current["parent_type"] != 1:
                break
            current = nodes[current["parent_index"]]
            parent_name = label(current["hash"])
            rig = next((rig for rig in rigs if rig.data.bones.get(parent_name)), None)
            if rig is not None:
                empty.parent, empty.parent_type, empty.parent_bone = rig, "BONE", parent_name
                break
            if parent_name in added:
                empty.parent = added[parent_name]
                break
        empty.matrix_world = Matrix(tuple(tuple(node["world"][row + column * 4] for column in range(4))
                                          for row in range(4)))
    # a character the game's scripts drive needs its whole skeleton (weapon attach and aim nodes and the like),
    # not just the bones the mesh uses
    options.only_weighted_bones = False
    # a breed loads its base unit as a package of that name when a mission loads
    options.unit_package = True
    base = breed.breed_for_unit(resource)
    if base:
        options.breed_base = base
    ragdoll_bodies = _import_ragdoll(collection, ragdoll, rigs, label) if ragdoll else 0
    if ragdoll_bodies:
        options.ragdoll_event = options.ragdoll_event.strip() or "ragdoll"
    if movers:
        mover = movers[0]
        options.use_mover = True
        options.mover_height = mover["height"]
        options.mover_radius = mover["radius"]
        options.mover_slope = mover["slope_limit"]
        options.mover_filter = _template_name(mover["collision_filter"])
    context.view_layer.update()
    return len(planned), skipped, bool(movers), ragdoll_bodies, len(added)


def _pose_matrix(pose):
    x, y, z, w = pose["q"]
    return Matrix.Translation(pose["p"]) @ Quaternion((w, x, y, z)).to_matrix().to_4x4()


def _import_ragdoll(collection, ragdoll, rigs, label):
    """The game unit's ragdoll (compiler --physics-collection): a collision body per bone (simulated, its mass and
    shapes) and a ragdoll joint per connection with the game's limits and springs. Returns the bodies made."""
    bodies = []
    names = [("ragdoll_" + label(body["node"])) for body in ragdoll["bodies"]]
    clashes = sorted(name for name in names if name in bpy.data.objects)
    if clashes:
        raise ValueError("Objects with these names already exist (the names must stay exact): " + ", ".join(clashes[:6]))
    for body, name in zip(ragdoll["bodies"], names):
        bone_name = label(body["node"])
        rig = next((rig for rig in rigs if rig.data.bones.get(bone_name)), None)
        pose = _pose_matrix(body["pose"])
        owner = None
        for index, shape in enumerate(body["shapes"]):
            if shape["type"] == "convex":
                mesh = bpy.data.meshes.new(name)
                points = shape["vertices"]
                triangles = shape["triangles"]
                mesh.from_pydata([tuple(points[i:i + 3]) for i in range(0, len(points), 3)], [],
                                 [tuple(triangles[i:i + 3]) for i in range(0, len(triangles), 3)])
                mesh.update()
                kind = "convex"
            elif shape["type"] in ("sphere", "box", "capsule"):
                values = {"sphere": lambda: (shape["radius"],), "box": lambda: tuple(shape["half_extents"]),
                          "capsule": lambda: (shape["radius"], shape["half_height"] * 2)}[shape["type"]]()
                mesh = _shape_mesh(name, {"type": shape["type"], "values": values,
                                          "local": (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)})
                kind = shape["type"]
            else:
                continue
            obj = bpy.data.objects.new(name if index == 0 else name + "_" + str(index), mesh)
            obj.display_type = "WIRE"
            obj.hide_render = True
            collection.objects.link(obj)
            if rig is not None:
                obj.parent = rig
                obj.parent_type = "BONE"
                obj.parent_bone = bone_name
            obj.matrix_world = pose @ _pose_matrix(shape["local"])
            settings = obj.dt_collider
            settings.role = "collision"
            settings.shape = kind
            if owner is None:
                owner = obj
                settings.is_body = True
                settings.actor = "dynamic" if body["dynamic"] and not body["kinematic"] else "keyframed"
                settings.mass = body["mass"] if body["mass"] > 0 else 1.0
            else:
                settings.body = owner
        bodies.append((owner, pose))
    for number, joint in enumerate(ragdoll["joints"]):
        if not (0 <= joint["body0"] < len(bodies) and 0 <= joint["body1"] < len(bodies)):
            continue
        (first, first_pose), (second, _) = bodies[joint["body0"]], bodies[joint["body1"]]
        if first is None or second is None:
            continue
        empty = bpy.data.objects.new("ragdoll_joint_" + second.name[len("ragdoll_"):], None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.05
        collection.objects.link(empty)
        if second.parent is not None:
            empty.parent = second.parent
            empty.parent_type = second.parent_type
            empty.parent_bone = second.parent_bone
        empty.matrix_world = first_pose @ _pose_matrix(joint["frame0"])
        settings = empty.dt_collider
        settings.joint_kind = "ragdoll"
        settings.body_a, settings.body_b = first, second
        motion = joint["motion"]
        settings.hinge_limits = True
        settings.twist_min, settings.twist_max = (0.0, 0.0) if motion[3] == "locked" else joint["twist"]
        settings.swing_y = 0.0 if motion[4] == "locked" else joint["swing"][0]
        settings.swing_z = 0.0 if motion[5] == "locked" else joint["swing"][1]
        settings.spring_stiffness, settings.spring_damping = joint["swing_drive"]
    return sum(1 for owner, _ in bodies if owner is not None)
