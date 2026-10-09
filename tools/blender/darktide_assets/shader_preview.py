"""The game's own shaders in the viewport.

material_shader() takes one of the game's materials, finds its compiled shader (in its own stream or its shader
provider's), picks the shader the engine picks for a particle billboard (the default context's first shader whose
condition passes, see condition_passes), translates the vertex and pixel stage to GLSL (shader_glsl) and loads the
material's values and textures. Inputs the engine fills from the level are fixed for the viewport: no fog or
shadows, a plain white light, and the scene depth far away (no soft fade where particles meet geometry).
"""
import json
import math
import os
import struct
import tempfile

import bpy
import gpu
import numpy as np

from . import shader_glsl
from .reference_skeleton import _murmur64

CACHE = os.path.join(tempfile.gettempdir(), "darktide_shaders")


def h32(name):
    return _murmur64(name) >> 32


# ---------------------------------------------------------------------------------------------------------
# Shader selection: condition_language programs (Darktide VM version 1, FUN_1407125c0)
# header u16 version (1), u16 instruction count, u16 instruction offset, u16 constant count; u32 constants at +8;
# u16 instructions: 0x2000 push, 0x0000 call intrinsic (argument count), 0x4000 compare (0 = equals operand,
# else operator), 0x6000 jump when the top is zero, 0x8000 end; 0x1000 marks an immediate operand.
_OPERATORS = {1: lambda a, b: a < b, 2: lambda a, b: a > b, 3: lambda a, b: a <= b, 4: lambda a, b: a >= b,
              5: lambda a, b: a == b, 6: lambda a, b: a != b, 7: lambda a, b: bool(a and b),
              8: lambda a, b: bool(a or b)}


def condition_passes(program, intrinsics):
    version, count, offset, _ = struct.unpack_from("<4H", program)
    if version != 1:
        raise ValueError("condition program version %d" % version)
    stack = []
    i = 0
    while i < count:
        word, = struct.unpack_from("<H", program, offset + 2 * i)
        op, arg = word & 0xe000, word & 0xfff
        operand = arg if word & 0x1000 else struct.unpack_from("<I", program, 8 + 4 * arg)[0]
        if op == 0x2000:
            stack.append(operand)
        elif op == 0x0000:
            name = stack.pop()
            args = stack[len(stack) - operand:]
            del stack[len(stack) - operand:]
            function = intrinsics.get(name)
            if function is None:
                raise ValueError("condition uses intrinsic #%08x" % name)
            stack.append(int(function(args)))
        elif op == 0x4000:
            if arg == 0:
                stack[-1] = int(stack[-1] == operand)
            else:
                b = stack.pop()
                stack[-1] = int(_OPERATORS[arg](stack[-1], b))
        elif op == 0x6000:
            if stack[-1] == 0:
                i = operand
                continue
        elif op == 0x8000:
            break
        i += 1
    return bool(stack[-1])


# billboard vertex channels by the names conditions use (FUN_14057fbc0)
def _channel_names(vertex):
    names = set()
    for channel in vertex:
        component, vtype, vset = channel["component"], channel["type"], channel.get("set", 0)
        if component == "texcoord":
            if vset == 1:
                names.add("rotation")
            elif vset == 6:
                names.add("pivot")
            elif vset == 0 and vtype in ("float1", "float3"):
                names.add("uv_animation")
            elif vset == 0 and vtype == "float2":
                names.add("uv_scale")
            elif vset == 5 and vtype == "float1":
                names.add("particle_id")
            elif vset == 8 and vtype == "float1":
                names.add("particle_age")
        elif component in ("tangent", "binormal"):
            names.add(component)
        elif component == "color":
            names.add("vertex_color")
    return {h32(name) for name in names}


# billboard flags (visualizer "mode"): has_intrinsic and the billboard_* intrinsics (FUN_140580480)
_FLAGS = {"lighting": 1, "pivot": 2, "raytracing": 4, "billboard_cross": 8, "billboard_random": 16}


def billboard_intrinsics(visualizer):
    channels = _channel_names(visualizer["vertex"])
    flags = visualizer.get("mode", 0)
    named = {h32(name): bit for name, bit in _FLAGS.items()}

    def flag(args):
        return any(flags & named.get(a, 0) for a in args)
    return {h32("has_visualizer_channels"): lambda args: all(a in channels for a in args),
            h32("has_intrinsic"): flag, h32("billboard_cross"): flag, h32("billboard_random"): flag,
            h32("object_type"): lambda args: all(a == h32("billboard_particle") for a in args),
            h32("defined"): lambda args: False}


# ---------------------------------------------------------------------------------------------------------
# Streams, export, translation

def _stub_stream(extract, game_bundle, resource):
    path = os.path.join(extract, "%016x.material" % resource)
    with open(path, "rb") as source:
        blob = source.read()
    size, = struct.unpack_from("<I", blob, 29)
    stub = blob[38:38 + size]
    if not stub.startswith(b"data/"):
        raise ValueError("material %016x in the extract is not a stub naming its stream" % resource)
    return os.path.join(game_bundle, *stub.split(b"\0")[0].decode().split("/"))


def _stream_parts(stream):
    """(provider, parent, variables {id32: floats}, textures {id32: resource}) of a v61 material stream."""
    material = struct.unpack_from("<I", stream, 4)[0]
    provider, parent = struct.unpack_from("<QQ", stream, material + 4)
    offset = material + 20
    count, = struct.unpack_from("<I", stream, offset)
    offset += 4 + 4 * count
    count, = struct.unpack_from("<I", stream, offset)
    textures = {name: resource for name, resource in
                (struct.unpack_from("<IQ", stream, offset + 4 + i * 12) for i in range(count))}
    offset += 4 + 12 * count
    count, = struct.unpack_from("<I", stream, offset)
    offset += 4 + 8 * count
    count, = struct.unpack_from("<I", stream, offset)
    records = [struct.unpack_from("<5I", stream, offset + 4 + i * 20) for i in range(count)]
    offset += 4 + 20 * count
    size, = struct.unpack_from("<I", stream, offset)
    data = stream[offset + 4:offset + 4 + size]
    variables = {}
    for klass, _, name, at, _ in records:
        if klass <= 3 and at + 4 * (klass + 1) <= len(data):
            variables[name] = list(struct.unpack_from("<%df" % (klass + 1), data, at))
    return provider, parent, variables, textures


class MaterialShader:
    """One material drawn with the game's shader for one billboard layout."""
    def __init__(self, program, index, pass_info, variables, textures):
        self.program = program
        self.variables = variables      # id32 -> floats (material, then its providers)
        self.texture_resources = textures
        self.pass_info = pass_info
        self.shader = None
        self.gpu_textures = {}
        self.error = None


_programs = {}


def _run(arguments):
    from . import _compiler_path, _run_compiler, _wine_tools, _windows_path
    compiler = _compiler_path()
    wine = _wine_tools(compiler)
    converted = [_windows_path(a, wine) if os.path.isabs(a) else a for a in arguments]
    result = _run_compiler(compiler, converted, wine, capture_output=True, text=True, timeout=300, check=False)
    if result.returncode:
        raise ValueError((result.stderr or result.stdout or "the compiler failed").strip()[:200])
    return result.stdout


def _export(stream_path):
    folder = os.path.join(CACHE, os.path.basename(stream_path))
    index_path = os.path.join(folder, "index.json")
    if not os.path.isfile(index_path):
        _run(["--shader-stages", os.path.abspath(stream_path), os.path.abspath(folder)])
    with open(index_path, "r", encoding="utf-8") as source:
        return folder, json.load(source)


def _pick_pass(shader):
    passes = shader["passes"]
    for wanted in ("hdr_transparent", "hdr_transparent_distortion"):
        for i, p in enumerate(passes):
            if p["layer"] == h32(wanted) and "vertex" in p and "pixel" in p:
                return i
    for i, p in enumerate(passes):
        states = dict((k, v) for k, v in p.get("render_states", []))
        if "vertex" in p and "pixel" in p and states.get(0x1e):
            return i
    return None


def material_shader(material, visualizer, extract, game_folder):
    """MaterialShader for the material (resource hash) drawn by the billboard visualizer description."""
    game_bundle = os.path.join(game_folder, "bundle")
    stream_path = _stub_stream(extract, game_bundle, material)
    with open(stream_path, "rb") as source:
        stream = source.read()
    variables, textures = {}, {}
    chain = [stream]
    shader_path = stream_path
    while True:
        header = struct.unpack_from("<7I", chain[-1])
        provider, parent, own_variables, own_textures = _stream_parts(chain[-1])
        for name, value in own_variables.items():
            variables.setdefault(name, value)
        for name, resource in own_textures.items():
            textures.setdefault(name, resource)
        if header[4]:
            break
        if len(chain) > 4 or not (provider or parent):
            raise ValueError("material %016x has no compiled shader" % material)
        shader_path = _stub_stream(extract, game_bundle, provider or parent)
        with open(shader_path, "rb") as source:
            chain.append(source.read())
    folder, index = _export(shader_path)
    contexts = [c for c in index["contexts"] if c["name"] == h32("default")]
    if not contexts:
        raise ValueError("the material's shader has no default context")
    conditions = bytes.fromhex(index["conditions"])
    intrinsics = billboard_intrinsics(visualizer)
    chosen = None
    for name, condition in contexts[0]["shaders"]:
        if condition == 0xffffffff or condition_passes(conditions[condition:], intrinsics):
            chosen = name
            break
    if chosen is None:
        raise ValueError("no shader of the material fits this billboard")
    shader_index = next(i for i, s in enumerate(index["shaders"]) if s["name"] == chosen)
    shader = index["shaders"][shader_index]
    pass_index = _pick_pass(shader)
    if pass_index is None:
        raise ValueError("the material's shader has no pass the viewport can draw")
    pass_info = shader["passes"][pass_index]
    key = (folder, shader_index, pass_index)
    if key not in _programs:
        with open(os.path.join(folder, pass_info["vertex"]), "rb") as source:
            vertex = source.read()
        with open(os.path.join(folder, pass_info["pixel"]), "rb") as source:
            pixel = source.read()
        _programs[key] = shader_glsl.Program(vertex, pixel)
    return MaterialShader(_programs[key], shader_index, pass_info, variables, textures)


# ---------------------------------------------------------------------------------------------------------
# GPU side


def _gpu_shader(program):
    info = gpu.types.GPUShaderCreateInfo()
    for slot, (name, semantic) in enumerate(program.attributes):
        info.vertex_in(slot, "FLOAT" if name in ("dt_instance", "dt_vertex") else "VEC4", name)
    interface = gpu.types.GPUStageInterfaceInfo("dt_interface")
    for name, _, _, mode, kind in program.varyings:
        if kind == "uvec4":
            interface.flat("UVEC4", name)
        elif mode == "flat":
            interface.flat("VEC4", name)
        elif program_noperspective(program, name):
            interface.no_perspective("VEC4", name)
        else:
            interface.smooth("VEC4", name)
    info.vertex_out(interface)
    info.push_constant("VEC4", "dt_viewport")
    typedefs = []
    for slot, (name, buffer) in enumerate(sorted(program.cbuffers.items(), key=lambda kv: kv[1]["glsl"])):
        type_name = "DtBlock" + buffer["glsl"][5:]
        typedefs.append("struct %s { vec4 r[%d]; };" % (type_name, max(1, buffer["registers"])))
        info.uniform_buf(slot, type_name, buffer["glsl"])
    if typedefs:
        info.typedef_source("\n".join(typedefs) + "\n")
    kinds = {"2D": "FLOAT_2D", "3D": "FLOAT_3D", "CUBE": "FLOAT_CUBE", "1D": "FLOAT_1D", "2D_ARRAY": "FLOAT_2D_ARRAY"}
    for slot, texture in enumerate(sorted(program.textures.values(), key=lambda t: t["glsl"])):
        info.sampler(slot, kinds.get(texture["dim"], "FLOAT_2D"), texture["glsl"])
    info.fragment_out(0, "VEC4", "dt_frag")
    info.vertex_source(program.vertex_source)
    info.fragment_source(program.fragment_source)
    return gpu.shader.create_from_info(info)


def program_noperspective(program, name):
    for varying in program.varyings:
        if varying[0] == name:
            return varying[2] in (4, 5, 7)
    return False


_stubs = {}


def _stub(kind, value, dim="2D"):
    key = (kind, tuple(value), dim)
    if key not in _stubs:
        data = gpu.types.Buffer("FLOAT", 4 * (6 if dim == "CUBE" else 1), list(value) * (6 if dim == "CUBE" else 1))
        if dim == "3D":
            _stubs[key] = gpu.types.GPUTexture((1, 1, 1), format="RGBA16F", data=data)
        elif dim == "CUBE":
            _stubs[key] = gpu.types.GPUTexture(1, is_cubemap=True, format="RGBA16F", data=data)
        else:
            _stubs[key] = gpu.types.GPUTexture((1, 1), format="RGBA16F", data=data)
    return _stubs[key]


# engine textures the viewport has no source for: what each one reads as
ENGINE_TEXTURES = {
    "linear_depth": (1.0e4, 1.0e4, 1.0e4, 1.0e4),          # nothing behind the particle: no soft fade
    "particle_lighting_atlas": (1.0, 1.0, 1.0, 1.0),        # plain white light
    "fog_volume": (0.0, 0.0, 0.0, 1.0),                     # no fog in scatter, full transmittance
    "global_diffuse_map": (1.0, 1.0, 1.0, 1.0),
    "global_specular_map": (1.0, 1.0, 1.0, 1.0),
}

_images = {}


def _texture_image(extract, resource):
    """A game texture as a Blender image (cached), decoded by the compiler."""
    if resource in _images:
        image = bpy.data.images.get(_images[resource])
        if image is not None:
            return image
    source = os.path.join(extract, "%016x.texture" % resource)
    if not os.path.isfile(source):
        raise ValueError("texture %016x is not in the extract folder" % resource)
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, "%016x.png" % resource)
    report = _run(["--texture-image", os.path.abspath(source), os.path.abspath(out)]).split()
    image = bpy.data.images.load(out, check_existing=False)
    image.name = "darktide preview %016x" % resource
    image.colorspace_settings.name = "sRGB" if "srgb" in report else "Non-Color"
    image.alpha_mode = "CHANNEL_PACKED"
    image.use_fake_user = False
    _images[resource] = image.name
    return image


def name_id(name):
    """IdString32 of a variable / texture channel name as the editor shows it (a name or '#' + 8 hex digits)."""
    return int(name[1:], 16) if name.startswith("#") else h32(name)


def prepare(material_shader_, extract, edits=None):
    """Create the GPU shader and textures (needs a GPU context, so from a draw callback). edits: the material's
    edited values {id32: floats} and textures {id32: image path}."""
    m = material_shader_
    if m.shader is not None or m.error:
        return
    try:
        m.shader = _gpu_shader(m.program)
        edits = edits or {}
        for key, texture in m.program.textures.items():
            kind, name = key
            if kind == "material":
                path = edits.get("textures", {}).get(h32(name))
                if path:
                    image = bpy.data.images.load(path, check_existing=True)
                else:
                    resource = m.texture_resources.get(h32(name))
                    image = _texture_image(extract, resource) if resource else None
                m.gpu_textures[texture["glsl"]] = gpu.texture.from_image(image) if image else _stub("black", (0, 0, 0, 0))
            else:
                value = ENGINE_TEXTURES.get(name, (0.0, 0.0, 0.0, 0.0))
                m.gpu_textures[texture["glsl"]] = _stub(name, value, texture["dim"])
        for name, values in (edits.get("variables") or {}).items():
            m.variables[name] = list(values)
    except Exception as exc:  # shader compile errors come back as plain exceptions
        m.error = str(exc).strip().splitlines()[-1][:200] if str(exc).strip() else type(exc).__name__
        m.shader = None


# ---------------------------------------------------------------------------------------------------------
# Constant buffers: engine values by name, then the material's


def engine_values(region, space, time_now, dt):
    """Camera and frame values the engine sets, in the engine's conventions (row vectors, D3D clip space)."""
    view_b = np.array(region.view_matrix, dtype=np.float64)
    window_b = np.array(region.window_matrix, dtype=np.float64)
    inverse_b = np.linalg.inv(view_b)
    right, up, back, position = inverse_b[:3, 0], inverse_b[:3, 1], inverse_b[:3, 2], inverse_b[:3, 3]
    world = np.identity(4)
    world[0, :3], world[1, :3], world[2, :3], world[3, :3] = right, -back, up, position
    view = np.linalg.inv(world)
    # engine view space (x right, y forward, z up) -> Blender view space, GL clip -> D3D clip
    to_blender = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)
    to_d3d = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0.5, 0.5], [0, 0, 0, 1]], dtype=np.float64)
    projection = (to_d3d @ window_b @ to_blender).T
    view_projection = view @ projection
    near, far = (space.clip_start, space.clip_end) if space is not None else (0.01, 1000.0)
    width, height = region_size()
    values = {
        "camera_world": world, "camera_last_world": world, "camera_view": view, "camera_last_view": view,
        "camera_inv_view": world, "camera_last_inv_view": world,
        "camera_projection": projection, "camera_last_projection": projection,
        "camera_inv_projection": np.linalg.inv(projection), "camera_last_inv_projection": np.linalg.inv(projection),
        "camera_view_projection": view_projection, "camera_last_view_projection": view_projection,
        "camera_inv_view_projection": np.linalg.inv(view_projection),
        "camera_last_inv_view_projection": np.linalg.inv(view_projection),
        "camera_pos": position, "camera_near_far": (near, far, 0.0),
        "camera_unprojection": (1.0 / projection[0, 0], 1.0 / projection[2, 1] if projection[2, 1] else 1.0, 0.0),
        "time": time_now, "delta_time": dt, "frame_number": float(int(time_now * 60.0)),
        "back_buffer_size": (width, height), "output_rt_size": (width, height), "viewport": (0.0, 0.0, width, height),
        "gamma": 2.2, "reverse_z": 0.0, "is_editor": 1.0,
        # c_billboard
        "view": view, "view_proj": view_projection,
        # global_environment_settings: no fog, white light from above, plain ambient
        "fog_enabled": 0.0, "sun_enabled": 1.0, "sun_direction": (0.0, 0.0, -1.0), "sun_color": (1.0, 1.0, 1.0),
        "ambient_enabled": 1.0, "global_ambient_tint": (1.0, 1.0, 1.0), "local_ambient_tint": (1.0, 1.0, 1.0),
        "emissive_intensity": 1.0, "global_roughness_multiplier": 1.0, "metallic_roughness_multiplier": 1.0,
        "non_metallic_roughness_multiplier": 1.0, "global_reflectance_multiplier": 1.0,
        "volumetric_lighting_enabled": 0.0, "skydome_intensity": 1.0,
    }
    return values


_region_size = [1.0, 1.0]


def region_size():
    return tuple(_region_size)


def set_region_size(width, height):
    _region_size[0], _region_size[1] = float(width), float(height)


def cbuffer_data(buffer, engine, variables):
    registers = np.zeros((max(1, buffer["registers"]), 4), dtype=np.float32)
    flat = registers.reshape(-1)
    for name, offset, element, count, matrix in buffer["layout"]:
        if name.startswith("__BINDLESS_"):
            continue
        value = engine.get(name)
        if value is None:
            stored = variables.get(h32(name))
            if stored is None:
                continue
            value = stored
        if matrix:
            matrices = np.asarray(value, dtype=np.float32).reshape(-1, 4, 4)
            column_major = matrix[2] == 2 if len(matrix) > 2 else True
            for k in range(min(len(matrices), count // 4 if count >= 4 else 1)):
                block = matrices[k].T if column_major else matrices[k]
                start = offset // 16 + 4 * k
                if start + 4 <= len(registers):
                    registers[start:start + 4] = block
            continue
        values = np.asarray(value, dtype=np.float32).reshape(-1)
        width = element.count if element.kind == "vector" else 1
        if count > 1:
            for k in range(count):
                chunk = values[k * width:(k + 1) * width]
                at = offset // 4 + 4 * k
                flat[at:at + len(chunk)] = chunk
        else:
            at = offset // 4
            chunk = values[:width]
            flat[at:at + len(chunk)] = chunk
    return registers


class Uniforms:
    def __init__(self):
        self.buffers = {}

    def get(self, name, registers):
        data = gpu.types.Buffer("FLOAT", registers.size, registers.reshape(-1).tolist())
        buffer = self.buffers.get(name)
        if buffer is None or buffer[1] != registers.size:
            buffer = (gpu.types.GPUUniformBuf(data), registers.size)
            self.buffers[name] = buffer
        else:
            buffer[0].update(data)
        return buffer[0]
