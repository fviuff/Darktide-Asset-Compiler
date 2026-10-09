"""Import an MMD model (.pmx 2.0 / 2.1): mesh, UVs, normals, materials with their textures, the skeleton with its
weights, and vertex morphs as shape keys. Bones keep their MMD (usually Japanese) names; Auto-map Bones reads them.

MMD units are 8 cm, Y is up and the model faces -Z in a left-handed space: (x, y, z) becomes (x, z, y) * 0.08, and
each triangle's last two corners swap so its front stays the side the model's normals point to. SDEF weights are taken as two-bone,
QDEF as four-bone weights (what the game skins with). Physics (rigid bodies, joints), IK, toon and sphere maps are
MMD-only and left out.
"""
import os
import struct

import bpy
import numpy as np
from bpy_extras.io_utils import ImportHelper

SCALE = 0.08


class PmxError(ValueError):
    pass


class _Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, size):
        if self.pos + size > len(self.data):
            raise PmxError("the file ends early (not a complete .pmx)")
        chunk = self.data[self.pos:self.pos + size]
        self.pos += size
        return chunk

    def unpack(self, fmt):
        size = struct.calcsize(fmt)
        return struct.unpack_from(fmt, self.take(size))

    def u8(self):
        return self.take(1)[0]

    def i32(self):
        return self.unpack("<i")[0]

    def f32(self, count=1):
        values = self.unpack("<%df" % count)
        return values if count > 1 else values[0]


def _index_format(size, unsigned):
    if size == 4:
        return "<i"
    return {1: "<B", 2: "<H"}[size] if unsigned else {1: "<b", 2: "<h"}[size]


def read(path):
    """The model as plain data: vertices, faces, textures, materials, bones, vertex morphs."""
    with open(path, "rb") as source:
        r = _Reader(source.read())
    if r.take(4) != b"PMX ":
        raise PmxError("not a .pmx file (.pmd, the older MMD format, isn't read; open it in PMX Editor and save as .pmx)")
    version = r.f32()
    globals_ = r.take(r.u8())
    encoding, extra_uvs, vertex_size, texture_size, material_size, bone_size, morph_size, rigid_size = globals_[:8]
    codec = "utf-16-le" if encoding == 0 else "utf-8"

    def text():
        return r.take(r.i32()).decode(codec, "replace")

    def index(size, unsigned=False):
        return r.unpack(_index_format(size, unsigned))[0]

    model = {"version": version, "name": text(), "name_en": text()}
    text(), text()  # comments

    count = r.i32()
    positions = np.empty((count, 3), np.float32)
    normals = np.empty((count, 3), np.float32)
    uvs = np.empty((count, 2), np.float32)
    weights = []   # per vertex: [(bone, weight)]
    for v in range(count):
        values = r.f32(8)
        positions[v], normals[v], uvs[v] = values[0:3], values[3:6], values[6:8]
        r.take(16 * extra_uvs)
        kind = r.u8()
        if kind == 0:
            pairs = [(index(bone_size), 1.0)]
        elif kind in (1, 3):
            a, b = index(bone_size), index(bone_size)
            w = r.f32()
            pairs = [(a, w), (b, 1.0 - w)]
            if kind == 3:
                r.take(36)   # SDEF C, R0, R1
        elif kind in (2, 4):
            bones = [index(bone_size) for _ in range(4)]
            pairs = list(zip(bones, r.f32(4)))
        else:
            raise PmxError("unknown vertex weight type %d" % kind)
        r.take(4)  # edge scale
        weights.append(pairs)
    model.update(positions=positions, normals=normals, uvs=uvs, weights=weights)

    count = r.i32()
    fmt = _index_format(vertex_size, True)
    faces = np.frombuffer(r.take(count * vertex_size), dtype=np.dtype(fmt)).astype(np.int64)
    model["faces"] = faces.reshape(-1, 3)

    model["textures"] = [text() for _ in range(r.i32())]

    materials = []
    for _ in range(r.i32()):
        name, name_en = text(), text()
        diffuse = r.f32(4)
        r.take(12 + 4 + 12)       # specular, strength, ambient
        flags = r.u8()
        r.take(16 + 4)            # edge colour, size
        texture = index(texture_size)
        index(texture_size)       # sphere map
        r.u8()
        if r.u8() == 0:
            index(texture_size)   # own toon
        else:
            r.u8()                # shared toon
        text()                    # memo
        materials.append({"name": name, "name_en": name_en, "diffuse": diffuse, "texture": texture,
                          "double_sided": bool(flags & 1), "index_count": r.i32()})
    model["materials"] = materials

    bones = []
    for _ in range(r.i32()):
        name, name_en = text(), text()
        position = r.f32(3)
        parent = index(bone_size)
        r.i32()                   # deform layer
        flags = r.unpack("<H")[0]
        tail = ("bone", index(bone_size)) if flags & 0x1 else ("offset", r.f32(3))
        if flags & 0x300:
            index(bone_size)
            r.f32()               # inherited rotation / translation
        if flags & 0x400:
            r.f32(3)              # fixed axis
        if flags & 0x800:
            r.f32(6)              # local axes
        if flags & 0x2000:
            r.i32()               # external parent
        if flags & 0x20:
            index(bone_size)
            r.i32()
            r.f32()
            for _ in range(r.i32()):
                index(bone_size)
                if r.u8():
                    r.f32(6)
        bones.append({"name": name, "name_en": name_en, "position": position, "parent": parent, "tail": tail})
    model["bones"] = bones

    morphs = []
    for _ in range(r.i32()):
        name, name_en = text(), text()
        r.u8()                    # panel
        kind, count = r.u8(), r.i32()
        if kind == 1:
            fmt = _index_format(vertex_size, True)
            row = np.dtype([("v", fmt[1]), ("d", "<f4", 3)])
            entries = np.frombuffer(r.take(count * row.itemsize), dtype=row)
            morphs.append({"name": name, "vertices": entries["v"].astype(np.int64), "offsets": entries["d"]})
        else:
            size = {0: morph_size + 4, 2: bone_size + 28, 3: vertex_size + 16, 4: vertex_size + 16,
                    5: vertex_size + 16, 6: vertex_size + 16, 7: vertex_size + 16, 8: material_size + 113,
                    9: morph_size + 4, 10: rigid_size + 25}.get(kind)
            if size is None:
                raise PmxError("unknown morph type %d" % kind)
            r.take(count * size)
    model["morphs"] = morphs

    for _ in range(r.i32()):      # display frames
        text(), text()
        r.u8()
        for _ in range(r.i32()):
            index(bone_size if r.u8() == 0 else morph_size)

    # MMD physics: rigid bodies on bones (mode 0 follows its bone, 1 and 2 are simulated and drive it) and the
    # spring joints between them
    rigid_bodies = []
    for _ in range(r.i32()):
        name, name_en = text(), text()
        bone = index(bone_size)
        group, no_collide = r.u8(), r.unpack("<H")[0]
        shape = r.u8()
        size, position, rotation = r.f32(3), r.f32(3), r.f32(3)
        mass, linear_damping, angular_damping, restitution, friction = r.f32(5)
        rigid_bodies.append({"name": name, "bone": bone, "shape": shape, "size": size, "position": position,
                             "mass": mass, "linear_damping": linear_damping, "angular_damping": angular_damping,
                             "mode": r.u8()})
    joints = []
    for _ in range(r.i32()):
        name, name_en = text(), text()
        r.u8()                    # joint type (0 = spring 6dof)
        a, b = index(rigid_size), index(rigid_size)
        position, rotation = r.f32(3), r.f32(3)
        move_min, move_max, turn_min, turn_max = r.f32(3), r.f32(3), r.f32(3), r.f32(3)
        spring_move, spring_turn = r.f32(3), r.f32(3)
        joints.append({"name": name, "a": a, "b": b, "turn_min": turn_min, "turn_max": turn_max,
                       "spring_turn": spring_turn})
    model["rigid_bodies"], model["joints"] = rigid_bodies, joints
    return model


def _convert(vectors):
    """MMD (x, y, z) -> Blender (x, z, y) in metres."""
    return vectors[:, [0, 2, 1]] * SCALE


def _material(name, info, image):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.use_backface_culling = not info["double_sided"]
    tree = material.node_tree
    bsdf = next(node for node in tree.nodes if node.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Roughness"].default_value = 0.8
    r, g, b, alpha = info["diffuse"]
    if image is None:
        bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
    else:
        node = tree.nodes.new("ShaderNodeTexImage")
        node.image = image
        node.location = (-500, 250)
        tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
        # cut-outs (hair strands, lashes) have pixels below half alpha: alpha clip, as the game draws them
        pixels = np.empty(len(image.pixels), np.float32)
        image.pixels.foreach_get(pixels)
        if image.channels == 4 and pixels.size and (pixels[3::4] < 0.5).any():
            clip = tree.nodes.new("ShaderNodeMath")
            clip.operation = "ROUND"
            clip.location = (-200, 0)
            tree.links.new(node.outputs["Alpha"], clip.inputs[0])
            tree.links.new(clip.outputs[0], bsdf.inputs["Alpha"])
    if alpha < 0.999 and image is None:
        bsdf.inputs["Alpha"].default_value = alpha
    return material


def import_model(context, path):
    model = read(path)
    folder = os.path.dirname(os.path.abspath(path))
    name = os.path.splitext(os.path.basename(path))[0]
    collection = context.collection

    # skeleton
    armature = bpy.data.armatures.new(name)
    rig = bpy.data.objects.new(name, armature)
    collection.objects.link(rig)
    context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    heads = _convert(np.array([bone["position"] for bone in model["bones"]], np.float32))
    edit_bones = []
    for i, bone in enumerate(model["bones"]):
        edit = armature.edit_bones.new(bone["name"] or "bone_%d" % i)
        edit_bones.append(edit)
    for i, bone in enumerate(model["bones"]):
        edit = edit_bones[i]
        head = heads[i]
        kind, value = bone["tail"]
        if kind == "bone":
            tail = heads[value] if 0 <= value < len(heads) and value != i else head
        else:
            tail = head + _convert(np.array([value], np.float32))[0]
        if np.linalg.norm(tail - head) < 1e-4:
            tail = head + np.array((0.0, 0.0, 0.02), np.float32)
        edit.head, edit.tail = head.tolist(), tail.tolist()
        if 0 <= bone["parent"] < len(edit_bones) and bone["parent"] != i:
            edit.parent = edit_bones[bone["parent"]]
    bone_names = [edit.name for edit in edit_bones]
    bpy.ops.object.mode_set(mode="OBJECT")

    # mesh
    mesh = bpy.data.meshes.new(name)
    faces = model["faces"][:, [0, 2, 1]]
    mesh.vertices.add(len(model["positions"]))
    mesh.vertices.foreach_set("co", _convert(model["positions"]).ravel())
    mesh.loops.add(faces.size)
    mesh.loops.foreach_set("vertex_index", faces.ravel().astype(np.int32))
    mesh.polygons.add(len(faces))
    mesh.polygons.foreach_set("loop_start", np.arange(0, faces.size, 3, dtype=np.int32))
    mesh.polygons.foreach_set("loop_total", np.full(len(faces), 3, np.int32))
    uv = mesh.uv_layers.new(name="UVMap")
    loop_uvs = model["uvs"][faces.ravel()]
    loop_uvs[:, 1] = 1.0 - loop_uvs[:, 1]
    uv.data.foreach_set("uv", loop_uvs.ravel())
    material_index = np.empty(len(faces), np.int32)
    start = 0
    images = {}
    for slot, info in enumerate(model["materials"]):
        triangles = info["index_count"] // 3
        material_index[start:start + triangles] = slot
        start += triangles
        image = None
        if 0 <= info["texture"] < len(model["textures"]):
            texture_path = os.path.join(folder, model["textures"][info["texture"]].replace("\\", os.sep))
            if texture_path not in images:
                images[texture_path] = (bpy.data.images.load(texture_path, check_existing=True)
                                        if os.path.isfile(texture_path) else None)
            image = images[texture_path]
        mesh.materials.append(_material(info["name"] or "material_%d" % slot, info, image))
    mesh.polygons.foreach_set("material_index", material_index)
    mesh.update()
    mesh.validate()
    mesh.normals_split_custom_set_from_vertices(_convert(model["normals"]) / SCALE)

    body = bpy.data.objects.new(name + "_mesh", mesh)
    collection.objects.link(body)
    body.parent = rig
    modifier = body.modifiers.new("Armature", "ARMATURE")
    modifier.object = rig
    groups = [body.vertex_groups.new(name=bone_name) for bone_name in bone_names]
    for vertex, pairs in enumerate(model["weights"]):
        for bone, weight in pairs:
            if weight > 0.0 and 0 <= bone < len(groups):
                groups[bone].add((vertex,), weight, "ADD")

    # vertex morphs -> shape keys
    if model["morphs"]:
        body.shape_key_add(name="Basis")
        base = _convert(model["positions"])
        for morph in model["morphs"]:
            key = body.shape_key_add(name=morph["name"] or "morph", from_mix=False)
            coords = base.copy()
            valid = morph["vertices"] < len(coords)
            coords[morph["vertices"][valid]] += _convert(morph["offsets"][valid].astype(np.float32))
            key.data.foreach_set("co", coords.ravel())
    for obj in context.selected_objects:
        obj.select_set(False)
    rig.select_set(True)
    body.select_set(True)
    context.view_layer.objects.active = rig
    return rig, body, model


class DARKTIDE_OT_import_pmx(bpy.types.Operator, ImportHelper):
    bl_idname = "darktide.import_pmx"
    bl_label = "Import MMD Model (.pmx)"
    bl_description = ("Import an MMD model (.pmx): mesh, textures, skeleton and weights, morphs as shape keys. Then "
                      "New Asset from Selection, pick a skeleton preset, Auto-map Bones and Fit to Darktide Skeleton")
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".pmx"
    filter_glob: bpy.props.StringProperty(default="*.pmx", options={"HIDDEN"})

    def execute(self, context):
        if context.object and context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            rig, body, model = import_model(context, self.filepath)
        except (OSError, PmxError, struct.error) as exc:
            self.report({"ERROR"}, "Couldn't read " + os.path.basename(self.filepath) + ": " + str(exc))
            return {"CANCELLED"}
        missing = [t for t in model["textures"] if not os.path.isfile(
            os.path.join(os.path.dirname(self.filepath), t.replace("\\", os.sep)))]
        text = "Imported %s: %d vertices, %d bones, %d materials, %d shape keys" % (
            rig.name, len(model["positions"]), len(model["bones"]), len(model["materials"]), len(model["morphs"]))
        if missing:
            text += "; textures not found next to the .pmx: " + ", ".join(missing[:3])
        self.report({"WARNING"} if missing else {"INFO"}, text)
        return {"FINISHED"}


def _menu(self, context):
    self.layout.operator(DARKTIDE_OT_import_pmx.bl_idname, text="MMD Model (.pmx)")


def register():
    bpy.utils.register_class(DARKTIDE_OT_import_pmx)
    bpy.types.TOPBAR_MT_file_import.append(_menu)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu)
    bpy.utils.unregister_class(DARKTIDE_OT_import_pmx)
