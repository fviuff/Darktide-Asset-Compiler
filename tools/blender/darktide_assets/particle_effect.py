"""Particle effects: an Empty holds a particle effect, edited like in the Stingray particle editor.

An effect has systems; a system has particle channels (the data each particle carries), initializers (run when a
particle is born), simulators (run every frame: emitters, ageing, forces, collisions...) and visualizers (how the
particles are drawn: billboard sprites, meshes, lights, ribbons, GPU particles) whose vertex writers feed the
material's shader. Start from scratch (New System) or from any of the game's effects (Import Game Effect).
Curves are Blender float curves and gradients colour ramps; the game draws straight lines between their points.

The compiler turns the description into the game's particles resource. The game's materials and units the
visualizers name are shipped as owned copies; each material can get new values and textures.
"""
import json
import math
import os
import struct
import tempfile

import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                       IntProperty, PointerProperty, StringProperty)

from .reference_skeleton import _murmur64

FLT_MAX = 3.4028234663852886e+38
_names = None
_schema = None


def schema():
    """{category: {type: [(field name, kind)]}} from particle_schema.json (compiler --particles-schema)."""
    global _schema
    if _schema is None:
        with open(os.path.join(os.path.dirname(__file__), "particle_schema.json"), "r", encoding="utf-8") as source:
            raw = json.load(source)
        _schema = {category: {item["type"]: [(f["name"], f["kind"]) for f in item["fields"]] for item in items}
                   for category, items in raw.items()}
    return _schema


_type_lists = {}


def _type_items(category):
    # Blender needs these lists kept alive while its menus show them
    if category not in _type_lists:
        _type_lists[category] = [(name, name.replace("_", " ").capitalize(), "") for name in schema()[category]]
    return _type_lists[category]


def _name_table():
    """IdString32 -> name for variable and texture names the game shader presets know."""
    global _names
    if _names is None:
        _names = {}
        with open(os.path.join(os.path.dirname(__file__), "game_shaders.json"), "r", encoding="utf-8") as source:
            for preset in json.load(source)["presets"]:
                for name in [variable["name"] for variable in preset["variables"]] + preset["textures"]:
                    if not name.startswith("#"):
                        _names[_murmur64(name) >> 32] = name
        with open(os.path.join(os.path.dirname(__file__), "shader_names.json"), "r", encoding="utf-8") as source:
            _names.update({int(value, 16): name for value, name in json.load(source).items()})
    return _names


def _cooked_body(path):
    with open(path, "rb") as source:
        blob = source.read()
    size = struct.unpack_from("<I", blob, 29)[0]
    return blob[38:38 + size]


def _material_stream(extract, game_bundle, material_hash):
    stub = _cooked_body(os.path.join(extract, "%016x.material" % material_hash))
    if not stub.startswith(b"data/"):
        raise ValueError("material %016x in the extract is not a stub naming its stream" % material_hash)
    path = os.path.join(game_bundle, *stub.split(b"\0")[0].decode().split("/"))
    with open(path, "rb") as source:
        return source.read()


def _material_summary(stream):
    """'name = values' for each variable and the texture channel names of a v61 material stream."""
    names = _name_table()
    label = lambda value: names.get(value, "#%08x" % value)
    offset = struct.unpack_from("<I", stream, 4)[0] + 20
    count = struct.unpack_from("<I", stream, offset)[0]
    offset += 4 + 4 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    textures = [label(struct.unpack_from("<I", stream, offset + 4 + i * 12)[0]) for i in range(count)]
    offset += 4 + 12 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    offset += 4 + 8 * count
    count = struct.unpack_from("<I", stream, offset)[0]
    records = [struct.unpack_from("<5I", stream, offset + 4 + i * 20) for i in range(count)]
    offset += 4 + 20 * count
    data = stream[offset + 4:offset + 4 + struct.unpack_from("<I", stream, offset)[0]]
    variables = []
    for klass, _, name, at, _ in records:
        if klass > 3:
            continue
        values = struct.unpack_from("<%df" % (klass + 1), data, at)
        variables.append(label(name) + " = " + ",".join("%g" % round(value, 3) for value in values))
    return variables, textures


# ---------------------------------------------------------------------------------------------------------
# Data. Component fields are generic: `kind` (from the schema) says which value holds the field.
U32 = 1 << 32


def _signed(value):
    value &= U32 - 1
    return value - U32 if value >= 1 << 31 else value


class DarktideFxField(bpy.types.PropertyGroup):
    kind: StringProperty()
    number: FloatProperty(name="", precision=4)
    integer: IntProperty(name="")
    flag: BoolProperty(name="")
    vector: FloatVectorProperty(name="", size=3, precision=4)
    text: StringProperty(name="")
    node: StringProperty()       # curve / gradient node in the system's node tree
    points: IntProperty(default=-1)  # curves/gradients with fewer than 2 points: 0 or 1 (kept in `vector`)


class DarktideFxComponent(bpy.types.PropertyGroup):
    category: StringProperty()
    type: StringProperty()
    fields: CollectionProperty(type=DarktideFxField)
    expanded: BoolProperty(default=True)


def _vertex_name_update(self, context):
    _rename_vertex(self)


class DarktideFxVertexChannel(bpy.types.PropertyGroup):
    component: EnumProperty(name="", items=[("position", "Position", ""), ("tangent", "Tangent", ""),
                                            ("binormal", "Binormal", ""), ("texcoord", "Texcoord", ""),
                                            ("color", "Color", "")], update=_vertex_name_update)
    type: EnumProperty(name="", items=[("float1", "float1", ""), ("float2", "float2", ""), ("float3", "float3", ""),
                                       ("float4", "float4", ""), ("ubyte4", "ubyte4", "")])
    set: IntProperty(name="Set", min=0, update=_vertex_name_update)


def _rename_vertex(channel):
    channel.name = channel.component + (str(channel.set) if channel.component == "texcoord" else "")


class DarktideFxVisualizer(bpy.types.PropertyGroup):
    type: StringProperty()
    fields: CollectionProperty(type=DarktideFxField)
    vertex: CollectionProperty(type=DarktideFxVertexChannel)
    writers: CollectionProperty(type=DarktideFxComponent)
    materials: StringProperty(name="Materials", description="Mesh visualizer material slots, as JSON")
    expanded: BoolProperty(default=True)


class DarktideFxChannel(bpy.types.PropertyGroup):
    size: IntProperty(name="Size", min=0, default=4, description="Bytes per particle (4 = a number, 16 = a vector)")


class DarktideFxVariable(bpy.types.PropertyGroup):
    value: FloatVectorProperty(name="", size=3)


class DarktideFxSystem(bpy.types.PropertyGroup):
    capacity: IntProperty(name="Max particles", min=0, default=100)
    min_capacity_scaling: FloatProperty(name="Min capacity scaling", min=0, max=1)
    channels: CollectionProperty(type=DarktideFxChannel)
    position: StringProperty(name="Position channel")
    channel_5c: StringProperty(name="Channel 5c")
    channel_60: StringProperty(name="Channel 60")
    collision_plane_offset: StringProperty(name="Collision plane offset")
    numbered_channels: StringProperty(description="channel fields stored as numbers (systems without channels)")
    transform: FloatVectorProperty(name="Transform", size=16, default=(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
    max_radius: FloatProperty(name="Max radius", min=0, default=0.5, description="Largest particle size, for culling")
    casts_shadows: BoolProperty(name="Casts shadows")
    disable_culling: BoolProperty(name="Disable culling")
    lod_enabled: BoolProperty(name="Simulation LOD")
    lod_near: IntProperty(name="Near", min=0)
    lod_far: IntProperty(name="Far", min=0)
    lod_min_step: FloatProperty(name="Min step", min=0, precision=5)
    lod_curves: CollectionProperty(type=DarktideFxField)
    initializers: CollectionProperty(type=DarktideFxComponent)
    simulators: CollectionProperty(type=DarktideFxComponent)
    visualizers: CollectionProperty(type=DarktideFxVisualizer)
    field_234: IntProperty(name="Field 234")
    field_238: IntProperty(name="Field 238", default=1)
    tree: PointerProperty(type=bpy.types.NodeTree)
    show_settings: BoolProperty(name="Settings")
    show_channels: BoolProperty(name="Channels", default=True)


class DarktideParticleMaterial(bpy.types.PropertyGroup):
    material_hash: StringProperty(name="Material")
    info_values: StringProperty(name="Current values")
    info_textures: StringProperty(name="Texture channels")
    values: StringProperty(name="Values", description="Variables to change, e.g. color=1,0.2,0.1; intensity=4")
    textures: StringProperty(name="Textures",
                             description="Channels to replace with your images, e.g. diffuse_map=//fire.png")


class DarktideParticleSettings(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="Particle effect", description="This Empty is a particle effect")
    resource_name: StringProperty(name="Resource name",
                                  description="Name of your effect inside the asset's folder (lowercase, digits, _ or -)")
    effect: StringProperty(name="Game effect",
                           description="Resource path of one of the game's effects, e.g. content/fx/particles/environment/brazier_01")
    life_time: FloatProperty(name="Duration", min=0, default=FLT_MAX,
                             description="Seconds the effect plays; very large = until stopped from Lua")
    looping: BoolProperty(name="Plays until stopped", default=True)
    culling_distance: FloatProperty(name="Culling distance", min=0, default=50)
    use_random_seed: BoolProperty(name="Random seed", default=True)
    flag_10: BoolProperty(name="Flag 10")
    field_14: IntProperty(name="Field 14", min=0)
    field_18: FloatProperty(name="Field 18", default=30)
    casts_shadows: BoolProperty(name="Casts shadows")
    disable_culling: BoolProperty(name="Disable culling")
    variables: CollectionProperty(type=DarktideFxVariable)
    systems: CollectionProperty(type=DarktideFxSystem)
    active_system: IntProperty()
    materials: CollectionProperty(type=DarktideParticleMaterial)


# ---------------------------------------------------------------------------------------------------------
# Curves and gradients live as nodes in a node tree per system.
def _tree(system):
    if system.tree is None:
        system.tree = bpy.data.node_groups.new("Darktide FX curves", "ShaderNodeTree")
    return system.tree


def _node(system, field):
    return _tree(system).nodes.get(field.node) if field.node else None


def _make_node(system, field):
    if field.node and _node(system, field):
        return _node(system, field)
    node = _tree(system).nodes.new("ShaderNodeFloatCurve" if field.kind == "curve" else "ShaderNodeValToRGB")
    if field.kind == "curve":
        node.mapping.use_clip = False
        for point in node.mapping.curves[0].points:
            point.handle_type = "VECTOR"
    field.node = node.name
    return node


def _set_curve(system, field, value):
    xs, ys = value["x"], value["y"]
    field.points = len(xs) if len(xs) < 2 else -1
    if len(xs) < 2:
        field.vector = (xs[0], ys[0], 0) if xs else (0, 0, 0)
        return
    node = _make_node(system, field)
    curve = node.mapping.curves[0]
    while len(curve.points) > 2:
        curve.points.remove(curve.points[-1])
    curve.points[0].location = (xs[0], 0)
    curve.points[1].location = (xs[-1], 0)
    for x in xs[1:-1]:
        curve.points.new(x, 0)
    node.mapping.update()
    # values after all positions: points at the same x keep the game's order
    for point, y in zip(curve.points, ys):
        point.location = (point.location[0], y)
        point.handle_type = "VECTOR"
    node.mapping.update()


def _get_curve(system, field):
    if field.points >= 0:
        return {"x": [field.vector[0]] * field.points, "y": [field.vector[1]] * field.points}
    node = _make_node(system, field)
    points = [(p.location[0], p.location[1]) for p in node.mapping.curves[0].points]  # kept sorted by Blender
    if len(points) > 10:
        raise ValueError("A particle curve has up to 10 points (" + str(len(points)) + " found)")
    return {"x": [p[0] for p in points], "y": [p[1] for p in points]}


def _set_gradient(system, field, value):
    xs, colors = value["x"], value["rgb"]
    field.points = 0 if not xs else -1
    if not xs:
        return
    ramp = _make_node(system, field).color_ramp
    ramp.interpolation = "LINEAR"
    while len(ramp.elements) > 1:
        ramp.elements.remove(ramp.elements[-1])
    ramp.elements[0].position = xs[0]
    for x in xs[1:]:
        ramp.elements.new(x)
    # colours after all positions: stops at the same position keep the game's order
    for element, rgb in zip(ramp.elements, colors):
        element.color = (rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, 1.0)


def _get_gradient(system, field):
    if field.points == 0:
        return {"x": [], "rgb": []}
    elements = list(_make_node(system, field).color_ramp.elements)  # kept sorted by Blender
    if len(elements) > 10:
        raise ValueError("A particle gradient has up to 10 colours (" + str(len(elements)) + " found)")
    to_byte = lambda c: float(round(c * 255.0, 3))
    return {"x": [e.position for e in elements],
            "rgb": [[to_byte(e.color[0]), to_byte(e.color[1]), to_byte(e.color[2])] for e in elements]}


# ---------------------------------------------------------------------------------------------------------
# Description <-> editor
def _set_field(system, field, kind, value):
    field.kind = kind
    if kind in ("float",):
        field.number = value if not isinstance(value, str) else struct.unpack("<f", struct.pack("<I", int(value[1:], 16)))[0]
    elif kind in ("uint", "int", "byte"):
        field.integer = _signed(int(value))
    elif kind == "bool":
        field.flag = bool(value)
    elif kind == "vector3":
        field.vector = value
    elif kind == "curve":
        _set_curve(system, field, value)
    elif kind == "gradient":
        _set_gradient(system, field, value)
    elif kind == "channel" and isinstance(value, (int, float)):
        field.text = "%d" % value
    elif kind in ("bursts", "control_points", "points"):
        field.text = json.dumps(value)
    else:
        field.text = "" if value is None else str(value)


def _get_field(system, field):
    kind = field.kind
    if kind == "float":
        return field.number
    if kind in ("uint", "byte"):
        return field.integer & (U32 - 1)
    if kind == "int":
        return field.integer
    if kind == "bool":
        return field.flag
    if kind == "vector3":
        return list(field.vector)
    if kind == "curve":
        return _get_curve(system, field)
    if kind == "gradient":
        return _get_gradient(system, field)
    if kind in ("channel", "vertex"):
        text = field.text.strip()
        return None if not text else int(text) if text.isdigit() else text
    if kind in ("bursts", "control_points", "points"):
        return json.loads(field.text or "[]")
    return field.text.strip()


def _fill_component(system, component, category, item):
    component.category, component.type = category, item["type"]
    component.fields.clear()
    for name, kind in schema()[category][item["type"]]:
        field = component.fields.add()
        field.name = name
        _set_field(system, field, kind, item.get(name, _default(kind, name)))


def _component_json(system, component):
    item = {"type": component.type}
    for field in component.fields:
        item[field.name] = _get_field(system, field)
    return item


def _default(kind, name):
    if kind == "curve":
        return {"x": [0, 1], "y": [1, 1]}
    if kind == "gradient":
        return {"x": [0], "rgb": [[255, 255, 255]]}
    if kind == "vector3":
        return [0, 0, 1] if name == "direction" else [0, 0, 0]
    if kind in ("float",):
        return 1.0 if name in ("max", "speed_max", "rate_max", "rate_min", "value", "scale_by") else 0.0
    if kind in ("uint", "int", "byte"):
        return -1 if name.endswith("_variable") else 0
    if kind == "bool":
        return False
    if kind == "bursts":
        return [{"time": 0, "count": 10}]
    if kind in ("control_points", "points"):
        return []
    if kind == "channel":
        return name if name in ("position", "velocity", "age", "life") else None
    if kind == "vertex":
        return "position" if name == "destination" else None
    return ""


def fill_settings(settings, effect):
    """Load an effect description into the editor."""
    settings.life_time = min(effect["life_time"], FLT_MAX) if not isinstance(effect["life_time"], str) else FLT_MAX
    settings.looping = settings.life_time >= 1e30
    settings.culling_distance = effect["culling_distance"] if not isinstance(effect["culling_distance"], str) else FLT_MAX
    for name in ("use_random_seed", "flag_10", "field_14", "field_18", "casts_shadows", "disable_culling"):
        setattr(settings, name, effect[name])
    settings.variables.clear()
    for variable in effect["variables"]:
        item = settings.variables.add()
        item.name, item.value = variable["name"], variable["value"]
    for system in settings.systems:
        if system.tree is not None:
            bpy.data.node_groups.remove(system.tree)
    settings.systems.clear()
    for data in effect["systems"]:
        system = settings.systems.add()
        system.name = data["name"]
        system.capacity, system.min_capacity_scaling = data["capacity"], data["min_capacity_scaling"]
        for channel in data["channels"]:
            item = system.channels.add()
            item.name, item.size = channel["name"], channel["size"]
        numbered = []
        for name in ("position", "channel_5c", "channel_60", "collision_plane_offset"):
            value = data[name]
            if isinstance(value, (int, float)):
                numbered.append(name)
                value = "%d" % value
            setattr(system, name, value or "")
        system.numbered_channels = ",".join(numbered)
        system.transform = data["transform"]
        system.max_radius = data["max_radius"]
        system.casts_shadows, system.disable_culling = data["casts_shadows"], data["disable_culling"]
        lod = data["simulation_lod"]
        system.lod_enabled, system.lod_near, system.lod_far = lod["enabled"], lod["near"], lod["far"]
        system.lod_min_step = lod["min_step"]
        system.lod_curves.clear()
        for curve in lod["curves"]:
            field = system.lod_curves.add()
            _set_field(system, field, "curve", curve)
        for category, key in (("initializers", "initializers"), ("simulators", "simulators")):
            for item in data[key]:
                _fill_component(system, getattr(system, key).add(), category, item)
        system.field_234, system.field_238 = _signed(data["field_234"]), _signed(data["field_238"])
        for item in data["visualizers"]:
            visualizer = system.visualizers.add()
            visualizer.type = item["type"]
            for name, kind in schema()["visualizers"][item["type"]]:
                field = visualizer.fields.add()
                field.name = name
                _set_field(system, field, kind, item[name])
            for channel in item.get("vertex", []):
                vertex = visualizer.vertex.add()
                vertex.component, vertex.type, vertex.set = channel["component"], channel["type"], channel["set"]
                _rename_vertex(vertex)
            for writer in item.get("writers", []):
                _fill_component(system, visualizer.writers.add(), "writers", writer)
            visualizer.materials = json.dumps(item["materials"]) if "materials" in item else ""
    settings.active_system = 0


def description(settings):
    """The effect description the compiler reads."""
    systems = []
    for system in settings.systems:
        numbered = set(filter(None, system.numbered_channels.split(",")))
        reference = lambda name: (int(getattr(system, name)) if name in numbered
                                  else getattr(system, name).strip() or None)
        visualizers = []
        for visualizer in system.visualizers:
            item = {"type": visualizer.type}
            for field in visualizer.fields:
                item[field.name] = _get_field(system, field)
            if visualizer.type != "gpu":
                item["vertex"] = [{"component": v.component, "type": v.type, "set": v.set} for v in visualizer.vertex]
                item["writers"] = [_component_json(system, writer) for writer in visualizer.writers]
            if visualizer.type == "mesh":
                item["materials"] = json.loads(visualizer.materials or "[]")
            visualizers.append(item)
        systems.append({
            "name": system.name,
            "capacity": system.capacity,
            "min_capacity_scaling": system.min_capacity_scaling,
            "channels": [{"name": c.name, "size": c.size} for c in system.channels],
            "position": reference("position"),
            "channel_5c": reference("channel_5c"),
            "channel_60": reference("channel_60"),
            "collision_plane_offset": reference("collision_plane_offset"),
            "transform": list(system.transform),
            "max_radius": system.max_radius,
            "casts_shadows": system.casts_shadows,
            "disable_culling": system.disable_culling,
            "simulation_lod": {"enabled": system.lod_enabled, "near": system.lod_near, "far": system.lod_far,
                               "min_step": system.lod_min_step,
                               "curves": [_get_curve(system, curve) for curve in system.lod_curves]},
            "initializers": [_component_json(system, c) for c in system.initializers],
            "simulators": [_component_json(system, c) for c in system.simulators],
            "field_234": system.field_234 & (U32 - 1),
            "field_238": system.field_238 & (U32 - 1),
            "visualizers": visualizers,
        })
    return {"life_time": FLT_MAX if settings.looping else settings.life_time,
            "culling_distance": settings.culling_distance,
            "use_random_seed": settings.use_random_seed, "flag_10": settings.flag_10, "field_14": settings.field_14,
            "field_18": settings.field_18, "casts_shadows": settings.casts_shadows,
            "disable_culling": settings.disable_culling,
            "variables": [{"name": v.name, "value": list(v.value)} for v in settings.variables],
            "systems": systems}


def referenced_materials(effect):
    """Game material ids ("#<16 hex>") the description's visualizers draw with, in order."""
    found = []

    def walk(value):
        if isinstance(value, dict):
            for key, member in value.items():
                if (key == "material" or key.startswith("material_")) and isinstance(member, str) \
                        and member.startswith("#") and len(member) == 17 and member not in found:
                    found.append(member)
                walk(member)
        elif isinstance(value, list):
            for item in value:
                walk(item)
    walk(effect)
    return [int(value[1:], 16) for value in found]


def import_game_effect(settings, extract, decode):
    """Load one of the game's effects into the editor; decode(path) -> description (the compiler's --particles)."""
    effect_path = settings.effect.strip().lower()
    path = os.path.join(extract, "%016x.particles" % _murmur64(effect_path))
    if not os.path.isfile(path):
        raise ValueError(effect_path + " is not in the extract folder (extract with limn's 'particles material texture unit')")
    fill_settings(settings, decode(path))


def load_materials(settings, extract, game_folder):
    """List the game's materials the effect draws with, keeping edits already made."""
    found = referenced_materials(description(settings))
    previous = {item.material_hash: (item.values, item.textures) for item in settings.materials}
    settings.materials.clear()
    bundle = os.path.join(bpy.path.abspath(game_folder), "bundle")
    for material_hash in found:
        item = settings.materials.add()
        item.material_hash = "%016x" % material_hash
        try:
            variables, textures = _material_summary(_material_stream(extract, bundle, material_hash))
            item.info_values, item.info_textures = "; ".join(variables), ", ".join(textures)
        except (OSError, ValueError, struct.error):
            item.info_values = "not one of the game's materials in the extract"
        item.values, item.textures = previous.get(item.material_hash, ("", ""))
    return len(found)


def _parse_values(text):
    result = {}
    for part in text.replace("\n", ";").split(";"):
        if not part.strip():
            continue
        if "=" not in part:
            raise ValueError("Expected name=value in '" + part.strip() + "'")
        name, value = (item.strip() for item in part.split("=", 1))
        numbers = [float(item) for item in value.replace(" ", ",").split(",") if item.strip()]
        if not 1 <= len(numbers) <= 4 or not all(math.isfinite(number) for number in numbers):
            raise ValueError("'" + name + "' needs 1 to 4 finite numbers")
        result[name] = numbers[0] if len(numbers) == 1 else numbers
    return result


def _parse_textures(text, path_converter):
    result = {}
    for part in text.replace("\n", ";").split(";"):
        if not part.strip():
            continue
        if "=" not in part:
            raise ValueError("Expected channel=image file in '" + part.strip() + "'")
        channel, image = (item.strip() for item in part.split("=", 1))
        path = os.path.abspath(bpy.path.abspath(image))
        if not os.path.isfile(path):
            raise ValueError("Texture image not found: " + path)
        result[channel] = path_converter(path)
    return result


def node_extras(obj, extract, path_converter):
    """The particles record the compiler reads from this Empty's node extras."""
    settings = obj.dt_particles
    name = settings.resource_name.strip()
    if not name:
        raise ValueError(obj.name + ": give the particle effect a resource name")
    if not settings.systems:
        raise ValueError(obj.name + ": the particle effect has no systems (New System or Import Game Effect)")
    materials = {}
    for item in settings.materials:
        values, textures = _parse_values(item.values), _parse_textures(item.textures, path_converter)
        if values or textures:
            materials[item.material_hash] = {"variables": values, "textures": textures}
    # as text: Blender's custom properties cannot hold the description's 32-bit unsigned numbers
    return {"name": name, "effect": json.dumps(description(settings)), "extract": path_converter(extract),
            "materials": materials}


# ---------------------------------------------------------------------------------------------------------
# A system made from scratch: sprites rising from a point, fading out. Its look is the game's spark sprite
# (material and the vertex layout its shader reads); everything else is plain authored values.
STARTER_MATERIAL = "#df0fd310e5eeb0d8"
STARTER = {
    "capacity": 100, "min_capacity_scaling": 0,
    "channels": [["position", 16], ["velocity", 16], ["age", 4], ["life", 4], ["size", 4], ["length", 4], ["random", 4]],
    "initializers": [
        {"type": "position_sphere", "position": "position", "radius_min": 0, "radius_max": 0.05},
        {"type": "random_float", "channel": "size", "min": 0.01, "max": 0.02},
        {"type": "random_float", "channel": "length", "min": 0.05, "max": 0.1},
        {"type": "velocity_cone", "velocity": "velocity", "speed_min": 1, "speed_max": 2, "theta_min": 0, "theta_max": 0.3,
         "direction": [0, 0, 1]},
        {"type": "random_float", "channel": "random", "min": 0, "max": 1},
        {"type": "random_float", "channel": "life", "min": 1, "max": 1.5},
        {"type": "zero", "channel": "age", "size": 4},
    ],
    "simulators": [
        {"type": "age_age", "age": "age", "life": "life"},
        {"type": "rate_emitter", "rate_min": 20, "rate_max": 20, "rate": {"x": [0, 1], "y": [1, 1]}},
        {"type": "velocity_accelerate", "velocity": "velocity", "acceleration": [0, 0, -2]},
        {"type": "position_integrate", "position": "position", "velocity": "velocity"},
    ],
    "visualizer": {
        "type": "billboard", "material": STARTER_MATERIAL, "sort": False, "mode": 0, "field_f8": 0,
        "vertex": [["position", "float3", 0], ["color", "ubyte4", 0], ["texcoord", "float2", 7], ["texcoord", "float1", 5],
                   ["texcoord", "float1", 8], ["tangent", "float3", 0]],
        "writers": [
            {"type": "size", "source": "size", "age": "age", "life": "life", "destination": "texcoord7",
             "scale": {"x": [0, 1], "y": [1, 1]}, "over_system_lifetime": False, "component": 0},
            {"type": "size", "source": "length", "age": "age", "life": "life", "destination": "texcoord7",
             "scale": {"x": [0, 1], "y": [1, 1]}, "over_system_lifetime": False, "component": 1},
            {"type": "color", "age": "age", "life": "life", "opacity": {"x": [0, 1], "y": [1, 0]},
             "gradient": {"x": [0], "rgb": [[255, 255, 255]]}, "destination": "color", "luminance": 0,
             "luminance_source": "position"},
            {"type": "rotation_align_to_velocity", "velocity": "velocity", "tangent": "tangent"},
            {"type": "copy_float", "source": "random", "destination": "texcoord5", "component": 0},
            {"type": "copy_vector3", "source": "position", "destination": "position"},
            {"type": "local_age", "destination": "texcoord8", "component": 0, "age": "age", "life": "life"},
        ],
    },
}


def add_starter_system(settings, name):
    system = settings.systems.add()
    system.name = name
    system.capacity = STARTER["capacity"]
    for channel_name, size in STARTER["channels"]:
        channel = system.channels.add()
        channel.name, channel.size = channel_name, size
    system.position = "position"
    system.max_radius = 0.2
    for _ in range(4):
        _set_field(system, system.lod_curves.add(), "curve", {"x": [], "y": []})
    for category in ("initializers", "simulators"):
        for item in STARTER[category]:
            _fill_component(system, getattr(system, category).add(), category, item)
    data = STARTER["visualizer"]
    visualizer = system.visualizers.add()
    visualizer.type = data["type"]
    for field_name, kind in schema()["visualizers"]["billboard"]:
        field = visualizer.fields.add()
        field.name = field_name
        _set_field(system, field, kind, data[field_name])
    for component, vtype, vset in data["vertex"]:
        vertex = visualizer.vertex.add()
        vertex.component, vertex.type, vertex.set = component, vtype, vset
        _rename_vertex(vertex)
    for writer in data["writers"]:
        _fill_component(system, visualizer.writers.add(), "writers", writer)
    settings.active_system = len(settings.systems) - 1
    return system


# ---------------------------------------------------------------------------------------------------------
# Operators
def _target(context, path):
    """The collection (or component) an operator acts on, from a path like 'systems.0.initializers'."""
    value = context.object.dt_particles
    for part in path.split("."):
        value = value[int(part)] if part.isdigit() else getattr(value, part)
    return value


def _system_of(context, path):
    parts = path.split(".")
    return context.object.dt_particles.systems[int(parts[1])]


class DARKTIDE_OT_fx_import_game_effect(bpy.types.Operator):
    bl_idname = "darktide.fx_import_game_effect"
    bl_label = "Import Game Effect"
    bl_description = "Load one of the game's effects into the editor (replaces the systems here)"

    def execute(self, context):
        from . import _compiler_path, _run_compiler, _wine_tools, _windows_path
        settings = context.object.dt_particles
        scene_settings = context.scene.dt_asset
        extract = bpy.path.abspath(scene_settings.game_extract_folder.strip())
        if not extract or not os.path.isdir(extract):
            self.report({"ERROR"}, "Set 'Game extract folder' to your limn extract first")
            return {"CANCELLED"}

        def decode(path):
            compiler = _compiler_path(scene_settings)
            wine = _wine_tools(compiler)
            with tempfile.TemporaryDirectory() as folder:
                out = os.path.join(folder, "effect.json")
                result = _run_compiler(compiler, ["--particles", _windows_path(path, wine), _windows_path(out, wine)],
                                       wine, capture_output=True, text=True, timeout=120, check=False)
                if result.returncode:
                    raise ValueError((result.stderr or result.stdout or "the compiler could not read the effect").strip()[:200])
                with open(out, "r", encoding="utf-8") as source:
                    return json.load(source)
        try:
            import_game_effect(settings, extract, decode)
            load_materials(settings, extract, scene_settings.game_folder)
        except (OSError, ValueError, KeyError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        if not settings.resource_name.strip():
            settings.resource_name = "".join(c if c.isalnum() or c in "_-" else "_" for c in context.object.name.lower())
        self.report({"INFO"}, "Imported %d system(s) drawing with %d material(s)" % (len(settings.systems), len(settings.materials)))
        return {"FINISHED"}


class DARKTIDE_OT_fx_load_materials(bpy.types.Operator):
    bl_idname = "darktide.load_particle_effect"
    bl_label = "Load Effect Materials"
    bl_description = "List the game's materials the effect draws with, with their values and texture channels"

    def execute(self, context):
        settings = context.object.dt_particles
        scene_settings = context.scene.dt_asset
        extract = bpy.path.abspath(scene_settings.game_extract_folder.strip())
        if not extract or not os.path.isdir(extract):
            self.report({"ERROR"}, "Set 'Game extract folder' to your limn extract first")
            return {"CANCELLED"}
        try:
            count = load_materials(settings, extract, scene_settings.game_folder)
        except (OSError, ValueError, struct.error) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        if not settings.resource_name.strip():
            settings.resource_name = "".join(c if c.isalnum() or c in "_-" else "_" for c in context.object.name.lower())
        self.report({"INFO"}, "The effect draws with " + str(count) + " material(s)")
        return {"FINISHED"}


class DARKTIDE_OT_fx_new_system(bpy.types.Operator):
    bl_idname = "darktide.fx_new_system"
    bl_label = "New System"
    bl_description = "Add a particle system made from scratch: sprites rising from a point (edit everything below)"

    def execute(self, context):
        settings = context.object.dt_particles
        add_starter_system(settings, "system_%d" % (len(settings.systems) + 1))
        if not settings.resource_name.strip():
            settings.resource_name = "".join(c if c.isalnum() or c in "_-" else "_" for c in context.object.name.lower())
        return {"FINISHED"}


class DARKTIDE_OT_fx_remove_system(bpy.types.Operator):
    bl_idname = "darktide.fx_remove_system"
    bl_label = "Remove System"

    def execute(self, context):
        settings = context.object.dt_particles
        if settings.systems:
            system = settings.systems[settings.active_system]
            if system.tree is not None:
                bpy.data.node_groups.remove(system.tree)
            settings.systems.remove(settings.active_system)
            settings.active_system = max(0, min(settings.active_system, len(settings.systems) - 1))
        return {"FINISHED"}


class DARKTIDE_OT_fx_add(bpy.types.Operator):
    bl_idname = "darktide.fx_add"
    bl_label = "Add"
    bl_description = "Add an item to this list"
    path: StringProperty()
    category: StringProperty()
    type: StringProperty()

    def execute(self, context):
        target = _target(context, self.path)
        system = _system_of(context, self.path)
        item = target.add()
        if self.category in ("initializers", "simulators", "writers"):
            _fill_component(system, item, self.category, {"type": self.type})
        elif self.category == "visualizers":
            item.type = self.type
            for name, kind in schema()["visualizers"][self.type]:
                field = item.fields.add()
                field.name = name
                _set_field(system, field, kind, _default(kind, name))
        elif self.category == "vertex":
            _rename_vertex(item)
        elif self.category == "channels":
            item.name = "channel_%d" % len(target)
        return {"FINISHED"}


class DARKTIDE_OT_fx_remove(bpy.types.Operator):
    bl_idname = "darktide.fx_remove"
    bl_label = "Remove"
    path: StringProperty()
    index: IntProperty()

    def execute(self, context):
        _target(context, self.path).remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_fx_move(bpy.types.Operator):
    bl_idname = "darktide.fx_move"
    bl_label = "Move"
    path: StringProperty()
    index: IntProperty()
    direction: IntProperty()

    def execute(self, context):
        target = _target(context, self.path)
        new = self.index + self.direction
        if 0 <= new < len(target):
            target.move(self.index, new)
        return {"FINISHED"}


class DARKTIDE_OT_fx_set_type(bpy.types.Operator):
    bl_idname = "darktide.fx_set_type"
    bl_label = "Type"
    bl_description = "Change this component's type (fields with the same name keep their values)"
    bl_property = "type"
    path: StringProperty()
    category: StringProperty()
    type: EnumProperty(items=lambda self, context: _type_items(self.category or "initializers"))

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        component = _target(context, self.path)
        system = _system_of(context, self.path)
        kept = {field.name: _get_field(system, field) for field in component.fields}
        item = {"type": self.type}
        item.update({name: kept[name] for name, _ in schema()[self.category][self.type] if name in kept})
        _fill_component(system, component, self.category, item)
        return {"FINISHED"}


class DARKTIDE_OT_fx_curve_points(bpy.types.Operator):
    bl_idname = "darktide.fx_curve_points"
    bl_label = "Edit as Curve"
    bl_description = "Turn this constant into a curve (or a curve into the game's empty curve)"
    path: StringProperty()
    empty: BoolProperty()

    def execute(self, context):
        field = _target(context, self.path)
        system = _system_of(context, self.path)
        if self.empty:
            (_set_curve if field.kind == "curve" else _set_gradient)(
                system, field, {"x": [], "y": []} if field.kind == "curve" else {"x": [], "rgb": []})
        elif field.kind == "curve":
            y = field.vector[1] if field.points == 1 else 1.0
            _set_curve(system, field, {"x": [0, 1], "y": [y, y]})
        else:
            _set_gradient(system, field, {"x": [0], "rgb": [[255, 255, 255]]})
        return {"FINISHED"}


# ---------------------------------------------------------------------------------------------------------
# Drawing
def _draw_field(layout, system, field, path, vertex_owner):
    row = layout.row(align=True)
    label = field.name.replace("_", " ").capitalize()
    kind = field.kind
    if kind == "channel":
        row.prop_search(field, "text", system, "channels", text=label)
    elif kind == "vertex":
        row.prop_search(field, "text", vertex_owner, "vertex", text=label)
    elif kind == "float":
        row.prop(field, "number", text=label)
    elif kind in ("uint", "int", "byte"):
        row.prop(field, "integer", text=label)
    elif kind == "bool":
        row.prop(field, "flag", text=label)
    elif kind == "vector3":
        row.label(text=label)
        row.prop(field, "vector", text="")
    elif kind in ("curve", "gradient"):
        column = layout.column(align=True)
        head = column.row(align=True)
        head.label(text=label)
        if field.points >= 0:
            if kind == "curve" and field.points == 1:
                head.prop(field, "vector", index=1, text="Constant")
            else:
                head.label(text="(empty)")
            op = head.operator(DARKTIDE_OT_fx_curve_points.bl_idname, text="", icon="FCURVE")
            op.path, op.empty = path, False
        else:
            op = head.operator(DARKTIDE_OT_fx_curve_points.bl_idname, text="", icon="X")
            op.path, op.empty = path, True
            node = _node(system, field)
            if node is None:
                op = column.operator(DARKTIDE_OT_fx_curve_points.bl_idname, text="Make " + kind, icon="FCURVE")
                op.path, op.empty = path, False
            elif kind == "curve":
                column.template_curve_mapping(node, "mapping")
            else:
                column.template_color_ramp(node, "color_ramp", expand=True)
    elif kind == "bursts":
        row.prop(field, "text", text=label)
        layout.label(text='e.g. [{"time": 0, "count": 30}, {"time": 1.5, "count": 10}]')
    else:
        row.prop(field, "text", text=label)


def _draw_components(layout, system, items, path, category, vertex_owner=None):
    for index, component in enumerate(items):
        box = layout.box()
        head = box.row(align=True)
        head.prop(component, "expanded", text="", icon="TRIA_DOWN" if component.expanded else "TRIA_RIGHT", emboss=False)
        op = head.operator(DARKTIDE_OT_fx_set_type.bl_idname, text=component.type.replace("_", " ").capitalize())
        op.path, op.category = "%s.%d" % (path, index), category
        for icon, direction in (("TRIA_UP", -1), ("TRIA_DOWN", 1)):
            move = head.operator(DARKTIDE_OT_fx_move.bl_idname, text="", icon=icon)
            move.path, move.index, move.direction = path, index, direction
        remove = head.operator(DARKTIDE_OT_fx_remove.bl_idname, text="", icon="X")
        remove.path, remove.index = path, index
        if component.expanded:
            for f, field in enumerate(component.fields):
                _draw_field(box, system, field, "%s.%d.fields.%d" % (path, index, f), vertex_owner)
    row = layout.row()
    op = row.operator(DARKTIDE_OT_fx_add.bl_idname, text="Add " + category[:-1].replace("_", " "), icon="ADD")
    op.path, op.category, op.type = path, category, next(iter(schema()[category]))


def _draw_system(layout, settings, index):
    system = settings.systems[index]
    path = "systems.%d" % index
    layout.prop(system, "name", text="System")
    row = layout.row()
    row.prop(system, "capacity")
    row.prop(system, "max_radius")
    box = layout.box()
    box.prop(system, "show_settings", icon="TRIA_DOWN" if system.show_settings else "TRIA_RIGHT", emboss=False)
    if system.show_settings:
        box.prop_search(system, "position", system, "channels")
        box.prop(system, "min_capacity_scaling")
        row = box.row()
        row.prop(system, "casts_shadows")
        row.prop(system, "disable_culling")
        box.prop(system, "lod_enabled")
        if system.lod_enabled:
            row = box.row()
            row.prop(system, "lod_near")
            row.prop(system, "lod_far")
            row.prop(system, "lod_min_step")
            for c, curve in enumerate(system.lod_curves):
                _draw_field(box, system, curve, "%s.lod_curves.%d" % (path, c), None)
        box.prop_search(system, "collision_plane_offset", system, "channels")
        row = box.row()
        row.prop(system, "field_234")
        row.prop(system, "field_238")
        box.label(text="Transform (emitter offset)")
        grid = box.grid_flow(columns=4, align=True)
        for i in range(16):
            grid.prop(system, "transform", index=i, text="")
    box = layout.box()
    box.prop(system, "show_channels", icon="TRIA_DOWN" if system.show_channels else "TRIA_RIGHT", emboss=False)
    if system.show_channels:
        for c, channel in enumerate(system.channels):
            row = box.row(align=True)
            row.prop(channel, "name", text="")
            row.prop(channel, "size", text="Bytes")
            remove = row.operator(DARKTIDE_OT_fx_remove.bl_idname, text="", icon="X")
            remove.path, remove.index = path + ".channels", c
        op = box.operator(DARKTIDE_OT_fx_add.bl_idname, text="Add channel", icon="ADD")
        op.path, op.category = path + ".channels", "channels"
    layout.label(text="Initializers (when a particle is born)")
    _draw_components(layout, system, system.initializers, path + ".initializers", "initializers")
    layout.label(text="Simulators (every frame)")
    _draw_components(layout, system, system.simulators, path + ".simulators", "simulators")
    layout.label(text="Visualizers (how particles are drawn)")
    for v, visualizer in enumerate(system.visualizers):
        vpath = "%s.visualizers.%d" % (path, v)
        box = layout.box()
        head = box.row(align=True)
        head.prop(visualizer, "expanded", text="", icon="TRIA_DOWN" if visualizer.expanded else "TRIA_RIGHT", emboss=False)
        head.label(text=visualizer.type.capitalize())
        remove = head.operator(DARKTIDE_OT_fx_remove.bl_idname, text="", icon="X")
        remove.path, remove.index = path + ".visualizers", v
        if not visualizer.expanded:
            continue
        for f, field in enumerate(visualizer.fields):
            _draw_field(box, system, field, "%s.fields.%d" % (vpath, f), visualizer)
        if visualizer.type == "gpu":
            continue
        if visualizer.type == "mesh":
            box.prop(visualizer, "materials")
        box.label(text="Vertex channels (what the material's shader reads)")
        for c, vertex in enumerate(visualizer.vertex):
            row = box.row(align=True)
            row.prop(vertex, "component")
            row.prop(vertex, "type")
            if vertex.component == "texcoord":
                row.prop(vertex, "set")
            remove = row.operator(DARKTIDE_OT_fx_remove.bl_idname, text="", icon="X")
            remove.path, remove.index = vpath + ".vertex", c
        op = box.operator(DARKTIDE_OT_fx_add.bl_idname, text="Add vertex channel", icon="ADD")
        op.path, op.category = vpath + ".vertex", "vertex"
        box.label(text="Vertex writers")
        _draw_components(box, system, visualizer.writers, vpath + ".writers", "writers", visualizer)
    row = layout.row(align=True)
    for vtype in ("billboard", "mesh", "light", "ribbon"):
        op = row.operator(DARKTIDE_OT_fx_add.bl_idname, text=vtype.capitalize(), icon="ADD")
        op.path, op.category, op.type = path + ".visualizers", "visualizers", vtype


class DARKTIDE_UL_fx_systems(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        layout.prop(item, "name", text="", emboss=False, icon="PARTICLES")


class DARKTIDE_PT_particle_effect(bpy.types.Panel):
    bl_label = "Darktide Particle Effect"
    bl_idname = "DARKTIDE_PT_particle_effect"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.type == "EMPTY"

    def draw_header(self, context):
        self.layout.prop(context.object.dt_particles, "enabled", text="")

    def draw(self, context):
        settings = context.object.dt_particles
        layout = self.layout
        layout.active = settings.enabled
        layout.prop(settings, "resource_name")
        row = layout.row(align=True)
        row.prop(settings, "effect")
        row.operator(DARKTIDE_OT_fx_import_game_effect.bl_idname, text="Import", icon="IMPORT")
        box = layout.box()
        box.label(text="Effect")
        row = box.row()
        row.prop(settings, "looping")
        if not settings.looping:
            row.prop(settings, "life_time")
        row = box.row()
        row.prop(settings, "culling_distance")
        row.prop(settings, "disable_culling")
        row = box.row()
        row.prop(settings, "use_random_seed")
        row.prop(settings, "casts_shadows")
        row = box.row()
        row.prop(settings, "flag_10")
        row.prop(settings, "field_14")
        row.prop(settings, "field_18")
        if settings.variables:
            box.label(text="Variables (set from Lua with World.set_particles_variable)")
            for variable in settings.variables:
                row = box.row()
                row.prop(variable, "name", text="")
                row.prop(variable, "value")
        row = layout.row()
        row.template_list("DARKTIDE_UL_fx_systems", "", settings, "systems", settings, "active_system", rows=3)
        column = row.column(align=True)
        column.operator(DARKTIDE_OT_fx_new_system.bl_idname, text="", icon="ADD")
        column.operator(DARKTIDE_OT_fx_remove_system.bl_idname, text="", icon="REMOVE")
        if settings.systems and 0 <= settings.active_system < len(settings.systems):
            _draw_system(layout, settings, settings.active_system)
        box = layout.box()
        row = box.row()
        row.label(text="Materials")
        row.operator(DARKTIDE_OT_fx_load_materials.bl_idname, text="Load", icon="FILE_REFRESH")
        for item in settings.materials:
            material = box.box()
            material.label(text="Material " + item.material_hash)
            if item.info_values:
                material.label(text=item.info_values[:120])
            if item.info_textures:
                material.label(text="Textures: " + item.info_textures[:110])
            material.prop(item, "values")
            material.prop(item, "textures")


def draw(layout, obj):
    """Line in the sidebar's object box; the editor itself is in Properties > Object."""
    row = layout.row()
    row.prop(obj.dt_particles, "enabled")
    if obj.dt_particles.enabled:
        row.label(text="(edit in Properties > Object)")


CLASSES = (DarktideFxField, DarktideFxComponent, DarktideFxVertexChannel, DarktideFxVisualizer, DarktideFxChannel,
           DarktideFxVariable, DarktideFxSystem, DarktideParticleMaterial, DarktideParticleSettings,
           DARKTIDE_OT_fx_import_game_effect, DARKTIDE_OT_fx_load_materials, DARKTIDE_OT_fx_new_system,
           DARKTIDE_OT_fx_remove_system, DARKTIDE_OT_fx_add, DARKTIDE_OT_fx_remove, DARKTIDE_OT_fx_move,
           DARKTIDE_OT_fx_set_type, DARKTIDE_OT_fx_curve_points, DARKTIDE_UL_fx_systems, DARKTIDE_PT_particle_effect)
