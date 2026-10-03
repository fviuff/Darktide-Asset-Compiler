#include "stingray/particles/particles_resource.h"

#include "stingray/murmur_hash.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace dtglb::stingray::particles {
namespace {
using json::Value;

constexpr std::uint32_t kVersion = 102;
constexpr std::uint32_t kNone = 0xffffffffu;
constexpr std::size_t kEffectHeader = 0x2c, kSystemHeader = 0x240, kCurve = 0x54, kGradient = 0xa4, kMaxChannels = 16;

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void fail(const std::string& what) { throw Failure(what); }

std::string hex(std::uint64_t value, int digits) {
    char buffer[24];
    std::snprintf(buffer, sizeof buffer, "#%0*llx", digits, static_cast<unsigned long long>(value));
    return buffer;
}
std::string hex_bytes(const std::uint8_t* data, std::size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) { out += digits[data[i] >> 4]; out += digits[data[i] & 15]; }
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Field layouts (dev notes: OPS.md, VIS.md). Ch = byte offset of a particle channel (0xffffffff = none),
// written as the channel's name; Vtx = byte offset into the visualizer's vertex, written as the vertex
// channel's name. State and Self are filled in by the encoder (runtime scratch offset, own system index).
enum class Kind { Ch, Vtx, U32, I32, F32, U8, Bool8, Bool32, Curve, Gradient, Vec3, Id32, Id64, Pad, Bytes, State, Self, Const };
struct Field { Kind kind; const char* name; std::size_t size = 0; std::uint32_t value = 0; };
using Layout = std::vector<Field>;

std::size_t field_size(const Field& field) {
    switch (field.kind) {
    case Kind::Curve: return kCurve;
    case Kind::Gradient: return kGradient;
    case Kind::Vec3: return 12;
    case Kind::Id64: return 8;
    case Kind::U8: return 1;
    case Kind::Pad: case Kind::Bytes: return field.size;
    default: return 4;
    }
}
std::size_t layout_size(const Layout& layout) {
    std::size_t size = 0;
    for (const auto& field : layout) size += field_size(field);
    return size;
}

Field CH(const char* n) { return {Kind::Ch, n}; }
Field VX(const char* n) { return {Kind::Vtx, n}; }
Field U(const char* n) { return {Kind::U32, n}; }
Field I(const char* n) { return {Kind::I32, n}; }
Field F(const char* n) { return {Kind::F32, n}; }
Field U8(const char* n) { return {Kind::U8, n}; }
Field B(const char* n) { return {Kind::Bool8, n}; }
Field B32(const char* n) { return {Kind::Bool32, n}; }
Field C(const char* n) { return {Kind::Curve, n}; }
Field G(const char* n) { return {Kind::Gradient, n}; }
Field V3(const char* n) { return {Kind::Vec3, n}; }
Field ID(const char* n) { return {Kind::Id32, n}; }
Field RES(const char* n) { return {Kind::Id64, n}; }
Field PAD(std::size_t n) { return {Kind::Pad, "", n}; }
Field STATE() { return {Kind::State, "state"}; }
Field SELF() { return {Kind::Self, "system"}; }
Field CONST(std::uint32_t v) { return {Kind::Const, "", 4, v}; }

struct OpSpec { const char* name; Layout layout; std::uint32_t state = 0; };

// initializers: u32 opcode, then operands (variable-length ones are handled in read/write_initializer)
const std::vector<OpSpec>& initializer_specs() {
    static const std::vector<OpSpec> specs = {
        {"zero", {CH("channel"), U("size")}},
        {"random_float", {CH("channel"), F("min"), F("max")}},
        {"velocity_cone", {CH("velocity"), F("speed_min"), F("speed_max"), F("theta_min"), F("theta_max"), V3("direction")}},
        {"position_sphere", {CH("position"), F("radius_min"), F("radius_max")}},
        {"position_box", {CH("position"), V3("min"), V3("max")}},
        {"random_int", {CH("channel"), I("min"), I("max")}},
        {"zero_velocity", {CH("velocity")}},
        {"copy", {CH("source"), CH("destination"), U("size")}},
        {"velocity_box", {CH("velocity"), V3("min"), V3("max")}},
        {"normal_box", {CH("tangent"), CH("binormal"), V3("min"), V3("max")}},
        {"tangent_box", {CH("tangent"), V3("min"), V3("max")}},
        {"position_cylinder", {CH("position"), F("radius_min"), F("radius_max"), F("height_min"), F("height_max"),
                               F("angle_min"), F("angle_max"), I("radius_variable"), I("height_variable"), I("angle_variable")}},
        {"velocity_cylinder", {CH("position"), CH("velocity"), F("radial_min"), F("radial_max"), F("z_min"), F("z_max"),
                               F("angular_min"), F("angular_max")}},
        {"position_mesh", {CH("position"), F("radius_min"), F("radius_max"), F("height_min"), F("height_max")}},
        {"position_mesh_skinned", {}},
        {"float", {CH("channel"), F("value")}},
        {"vector", {CH("channel"), V3("value")}},
        {"vector_language_program", {}},
        {"leap_number", {CH("leap_number")}},
        {"initializer_13", {CH("channel")}},
        {"spline_control_points", {{Kind::Bytes, "data", 0x40}}},
        {"multiply_by_variable", {CH("channel"), U("channel_size"), U("variable")}},
        {"rotation_align_to_local", {CH("tangent"), CH("binormal")}},
        {"runtime_position_mesh", {CH("position"), F("radius_min"), F("radius_max"), F("height_min"), F("height_max")}},
        {"initializer_18", {CH("channel")}},
    };
    return specs;
}
bool variable_initializer(std::uint32_t op) { return op == 0x0d || op == 0x0e || op == 0x11; }

// simulators: u8 opcode + u32 offset of their data in the system's data block; `state` = runtime scratch bytes
const std::vector<OpSpec>& simulator_specs() {
    static const std::vector<OpSpec> specs = {
        {"age_age", {CH("age"), CH("life")}},
        {"position_integrate", {CH("position"), CH("velocity")}},
        {"position_integrate_scaled", {CH("position"), CH("velocity"), CH("age"), CH("life"), C("scale"),
                                       B("over_system_lifetime"), I("scale_variable")}},
        {"velocity_accelerate", {CH("velocity"), V3("acceleration")}},
        {"advance_frame", {CH("channel"), I("start"), I("end"), B("loop"), F("speed")}},
        {"integrate_float_scaled", {CH("value"), CH("velocity"), CH("age"), CH("life"), C("scale")}},
        {"plane_collision", {CH("position"), CH("velocity"), F("offset"), F("restitution"), F("friction")}},
        {"rate_spawn", {CH("position"), CH("spawn"), F("particles_per_second"), U("target_system"), CH("velocity"),
                        F("inherit_velocity")}},
        {"trail_spawn", {CH("position"), CH("last_position"), F("particles_per_meter"), U("target_system"), CH("velocity"),
                         F("inherit_velocity")}},
        {"collision_spawn", {}},
        {"wind", {}},
        {"warp_box", {}},
        {"rate_emitter", {STATE(), F("rate_min"), F("rate_max"), C("rate"), SELF()}, 12},
        {"burst_emitter", {}, 8},
        {"grid_collision", {STATE(), CH("position"), CH("velocity"), CH("bounces"), F("restitution"), F("friction"),
                            F("offset"), F("grid_size"), U("grid_cells"), I("max_bounces")}},
        {"fast_forward_static", {}},
        {"debug_draw", {}},
        {"local_space", {STATE(), CH("position"), CH("tangent"), CH("binormal"), CH("velocity"), U("field_14")}, 0x44},
        {"ray", {CH("position"), CH("velocity"), CH("size_y"), U("start_variable"), U("end_variable")}},
        {"copy_variable_to_float", {CH("destination"), U("variable"), U("component")}},
        {"scale_float", {CH("destination"), CH("scale_by")}},
        {"spiral", {CH("position"), CH("velocity"), CH("spiral_rotation"), CH("spiral_speed"), CH("life"), CH("age"),
                    CH("center"), C("scale"), B32("over_system_lifetime"), B("use_center")}},
        {"query_collision", {CH("position"), CH("velocity"), CH("normal"), CH("offset"), U("queries_per_frame")}},
        {"collision_resolution", {CH("position"), CH("velocity"), CH("collision_plane_normal"), CH("collision_plane_offset"),
                                  CH("rotation"), CH("size"), U("rotation_mode"), B("kill_on_collision"),
                                  F("restitution"), F("friction")}},
        {"integrate_float", {CH("value"), CH("velocity")}},
        {"query_vector_field", {CH("position"), CH("output"), ID("vector_field")}},
        {"air_resistance", {CH("wind_velocity"), CH("velocity"), F("wind_coefficient"), F("noise_amplitude"), F("noise_period")}},
        {"vector_language_program", {}},
        {"spline_control_points", {}},
    };
    return specs;
}
// simulators whose data the codec keeps as bytes (never used by the game; layouts only known from VT2)
bool opaque_simulator(std::uint32_t op) { return op == 0x09 || op == 0x0a || op == 0x0b || op == 0x0f || op == 0x10 || op == 0x1b; }

// vertex writers: u32 opcode + body (VIS.md)
const std::vector<OpSpec>& writer_specs() {
    static const std::vector<OpSpec> specs = {
        {"copy_vector3", {CH("source"), VX("destination")}},
        {"write_float", {F("value"), VX("destination"), U("component")}},
        {"write_int", {U("value"), VX("destination")}},
        {"size", {CH("source"), CH("age"), CH("life"), VX("destination"), C("scale"), B("over_system_lifetime"), U("component")}},
        {"color", {CH("age"), CH("life"), C("opacity"), G("gradient"), VX("destination"), U("luminance"), CH("luminance_source")}},
        {"copy_float", {CH("source"), VX("destination"), U("component")}},
        {"rotate_by_velocity", {CH("velocity"), VX("destination")}},
        {"copy_float_and_floor", {CH("source"), VX("destination")}},
        {"distance_fade", {CH("position"), VX("color"), VX("hdr_color"), F("near_range"), F("near_fade"), F("far_range"),
                           F("far_fade")}},
        {"opacity_kill", {VX("color"), VX("hdr_color")}},
        {"stretch_by_velocity", {CH("velocity"), VX("destination"), F("velocity_min"), F("velocity_max"), F("stretch_min"),
                                 F("stretch_max")}},
        {"rotation_face_velocity", {CH("velocity"), VX("tangent"), VX("binormal"), F("velocity_threshold"), U("field_10"),
                                    U("field_14")}},
        {"rotation_align_to_velocity", {CH("velocity"), VX("tangent")}},
        {"uv_tile", {CH("frame"), VX("destination"), U("field_08")}},
        {"hdr_color", {CH("age"), CH("life"), C("opacity"), G("gradient"), VX("destination"), U("luminance"), CH("luminance_source")}},
        {"rotation_3d_from_2d", {CH("rotation"), VX("tangent"), VX("binormal")}},
        {"tangent_to_next", {VX("tangent"), VX("ribbon_distance"), CH("position"), CH("leap_number"), U("anchored_uv")}},
        {"snap_to_translation", {VX("ribbon_distance"), CH("position"), U("anchored_uv")}},
        {"local_age", {VX("destination"), U("component"), CH("age"), CH("life")}},
        {"rotation_3d", {CH("yaw"), CH("pitch"), CH("roll"), VX("destination")}},
        {"scale_3d", {CH("size_x"), CH("size_y"), CH("size_z"), C("scale_x"), C("scale_y"), C("scale_z"), CH("age"), CH("life"),
                      VX("destination")}},
    };
    return specs;
}

int find_spec(const std::vector<OpSpec>& specs, const std::string& name) {
    for (std::size_t i = 0; i < specs.size(); ++i)
        if (name == specs[i].name) return static_cast<int>(i);
    return -1;
}

// Visualizers: header fields, a block of up to 10 vertex channels, the vertex stride, then the writer list.
struct VisualizerSpec {
    const char* name;
    Layout before;               // fields before the channel count
    Layout after;                // fields between the channel block and the vertex stride
    std::size_t channels_at;     // payload offset of the channel count
    Layout tail;                 // fields after the payload size, up to the writers (and mesh material entries)
};
const VisualizerSpec kVisualizers[4] = {
    {"billboard", {{Kind::Const, "hash"}, CONST(0), RES("material"), CONST(1), B("sort"), U("mode")}, {}, 0x1c,
     {U("field_f8"), PAD(4)}},
    {"light", {}, {U("light_type"), U("flags"), F("falloff_start"), F("falloff_end"), F("spot_angle_start"), F("spot_angle_end"),
                   F("volumetric_intensity"), F("field_e8")}, 0x00, {PAD(4)}},
    {"mesh", {RES("unit"), U("field_08"), {Kind::Const, "material_count"}, RES("field_10"), CONST(0x100), U("flag_1c")}, {}, 0x20,
     {U("field_fc")}},
    {"ribbon", {{Kind::Const, "hash"}, CONST(0), RES("material"), CONST(1), U("field_14")}, {}, 0x18,
     {PAD(4), CH("leap_number"), PAD(4)}},
};
const Layout& gpu_layout() {
    static const Layout layout = {
        ID("name"), CONST(0), RES("material"), ID("name_2"), CONST(0), RES("material_2"), ID("name_3"), CONST(0), RES("material_3"),
        PAD(16), ID("field_40"), ID("field_44"), ID("field_48"), CONST(0), U("flags_50"), U("field_54"), U("field_58"),
        F("field_5c"), F("field_60"), F("field_64"), F("field_68"), F("field_6c"), U("field_70"), U("field_74"), U("field_78"),
        U("field_7c"), ID("field_80"), U("field_84"), PAD(0x18), CONST(0xa8), PAD(4)};
    return layout;
}
constexpr std::size_t kSizeAt[] = {0xf4, 0xf8, 0xf8, 0xf0, 0xa0};
constexpr std::uint32_t kVertexTypeSize[] = {4, 8, 12, 16, 4};
const char* const kVertexTypes[] = {"float1", "float2", "float3", "float4", "ubyte4"};
const char* component_name(std::uint32_t component) {
    switch (component) {
    case 0: return "position";
    case 2: return "tangent";
    case 3: return "binormal";
    case 5: return "texcoord";
    case 6: return "color";
    default: return nullptr;
    }
}

using Descriptor = std::array<std::uint32_t, 3>;   // component, type, set
std::uint32_t visualizer_hash(int type, const std::vector<Descriptor>& vertex, std::uint32_t mode, std::uint64_t material) {
    std::uint8_t flags[8] = {};
    for (const auto& channel : vertex) {
        if (channel[0] == 2) flags[0] = 1;
        if (channel[0] == 3) flags[1] = 1;
        if (type == 0 && channel[0] == 6) flags[2] = 1;
        if (type == 0 && channel[0] == 5 && channel[2] == 1) flags[3] = 1;
        if (type == 0 && channel[0] == 5 && channel[2] == 6) flags[4] = 1;
    }
    if (type == 0) flags[5] = static_cast<std::uint8_t>(mode & 1);
    constexpr std::uint64_t m = 0xc6a4a7935bd1e995ull;
    std::uint64_t k = murmur64(flags, 8, 0) * m;
    k = (k ^ (k >> 47)) * m;
    return static_cast<std::uint32_t>(((k ^ material) * m) >> 32);
}

// ---------------------------------------------------------------------------------------------------------
// Reading
struct In {
    const std::vector<std::uint8_t>& b;
    void need(std::size_t at, std::size_t n) const {
        if (at > b.size() || n > b.size() - at) fail("truncated at byte " + std::to_string(at));
    }
    std::uint8_t u8(std::size_t at) const { need(at, 1); return b[at]; }
    std::uint32_t u32(std::size_t at) const { need(at, 4); std::uint32_t v; std::memcpy(&v, &b[at], 4); return v; }
    std::uint64_t u64(std::size_t at) const { need(at, 8); std::uint64_t v; std::memcpy(&v, &b[at], 8); return v; }
    float f32(std::size_t at) const { need(at, 4); float v; std::memcpy(&v, &b[at], 4); return v; }
};

Value number(double v) { return Value::of(v); }
Value float_value(float f) {
    if (std::isfinite(f)) return Value::of(static_cast<double>(f));
    std::uint32_t bits; std::memcpy(&bits, &f, 4);
    return Value::of(hex(bits, 8));
}
Value floats(const In& in, std::size_t at, std::size_t count) {
    auto out = Value::array();
    for (std::size_t i = 0; i < count; ++i) out.push(float_value(in.f32(at + 4 * i)));
    return out;
}
bool same_float(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }

// curve: f32 x[10], f32 y[10], u32 n; unused x = 10000 + 10n, unused y = last y (all zero when n = 0)
Value read_curve(const In& in, std::size_t at, const std::string& where) {
    const std::uint32_t n = in.u32(at + 80);
    if (n > 10) fail(where + ": curve with " + std::to_string(n) + " points");
    for (std::size_t i = n; i < 10; ++i)
        if (!same_float(in.f32(at + 4 * i), n ? 10000.0f + 10.0f * static_cast<float>(n) : 0.0f) ||
            !same_float(in.f32(at + 40 + 4 * i), n ? in.f32(at + 40 + 4 * (n - 1)) : 0.0f))
            fail(where + ": curve padding differs");
    auto curve = Value::object();
    curve.set("x", floats(in, at, n));
    curve.set("y", floats(in, at + 40, n));
    return curve;
}
// gradient: f32 x[10], f32 rgb[10][3], u32 n; padded like a curve
Value read_gradient(const In& in, std::size_t at, const std::string& where) {
    const std::uint32_t n = in.u32(at + 160);
    if (n > 10) fail(where + ": gradient with " + std::to_string(n) + " points");
    for (std::size_t i = n; i < 10; ++i) {
        if (!same_float(in.f32(at + 4 * i), n ? 10000.0f + 10.0f * static_cast<float>(n) : 0.0f)) fail(where + ": gradient padding differs");
        for (std::size_t c = 0; c < 3; ++c)
            if (!same_float(in.f32(at + 40 + 12 * i + 4 * c), n ? in.f32(at + 40 + 12 * (n - 1) + 4 * c) : 0.0f))
                fail(where + ": gradient padding differs");
    }
    auto gradient = Value::object();
    gradient.set("x", floats(in, at, n));
    auto colors = Value::array();
    for (std::size_t i = 0; i < n; ++i) colors.push(floats(in, at + 40 + 12 * i, 3));
    gradient.set("rgb", std::move(colors));
    return gradient;
}

// particle channels of one system, named after the roles the components give them
struct Channels {
    std::vector<std::uint32_t> offsets, sizes;
    std::vector<std::string> names;
    std::uint32_t stride = 0;

    int index_of(std::uint32_t offset) const {
        for (std::size_t i = 0; i < offsets.size(); ++i)
            if (offsets[i] == offset) return static_cast<int>(i);
        return -1;
    }
    void suggest(std::uint32_t offset, const std::string& role) {
        const int i = index_of(offset);
        if (i < 0 || !names[i].empty()) return;
        static const std::set<std::string> generic = {"channel", "source", "destination", "value", "output", "spawn",
                                                      "luminance_source"};
        if (generic.count(role)) return;
        for (const auto& used : names) if (used == role) return;
        names[i] = role;
    }
    void finish() {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i].empty()) {
                char buffer[16];
                std::snprintf(buffer, sizeof buffer, "c%02x", offsets[i]);
                names[i] = buffer;
            }
    }
    Value reference(std::uint32_t offset, const std::string& where) const {
        if (offset == kNone) return Value();
        const int i = index_of(offset);
        if (i >= 0) return Value::of(names[i]);
        for (std::size_t c = 0; c < offsets.size(); ++c)
            if (offset > offsets[c] && offset < offsets[c] + sizes[c])
                return Value::of(names[c] + "+" + std::to_string(offset - offsets[c]));
        if (offset >= stride) return number(offset);   // systems without channels point at 0
        fail(where + ": channel offset " + std::to_string(offset) + " is inside no channel");
    }
};

struct VertexLayout {
    std::vector<std::uint32_t> offsets;
    std::vector<std::string> names;
    std::uint32_t stride = 0;
    Value reference(std::uint32_t offset, const std::string& where) const {
        if (offset == kNone) return Value();
        for (std::size_t i = 0; i < offsets.size(); ++i)
            if (offsets[i] == offset) return Value::of(names[i]);
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            const auto end = i + 1 < offsets.size() ? offsets[i + 1] : stride;
            if (offset > offsets[i] && offset < end) return Value::of(names[i] + "+" + std::to_string(offset - offsets[i]));
        }
        fail(where + ": vertex offset " + std::to_string(offset) + " is outside the vertex");
    }
};

struct ReadContext {
    const Channels* channels = nullptr;
    const VertexLayout* vertex = nullptr;
    std::uint32_t system = 0;      // own index
    std::uint32_t* state = nullptr; // running scratch offset
};

void read_fields(const In& in, std::size_t at, const Layout& layout, const ReadContext& context, Value& out,
                 const std::string& where) {
    for (const auto& field : layout) {
        const auto here = where + "." + field.name;
        switch (field.kind) {
        case Kind::Ch: out.set(field.name, context.channels->reference(in.u32(at), here)); break;
        case Kind::Vtx: out.set(field.name, context.vertex->reference(in.u32(at), here)); break;
        case Kind::U32: out.set(field.name, number(in.u32(at))); break;
        case Kind::I32: out.set(field.name, number(static_cast<std::int32_t>(in.u32(at)))); break;
        case Kind::F32: out.set(field.name, float_value(in.f32(at))); break;
        case Kind::U8: out.set(field.name, number(in.u8(at))); break;
        case Kind::Bool8:
            if (in.u8(at) > 1 || in.u8(at + 1) || in.u8(at + 2) || in.u8(at + 3)) fail(here + ": not a flag");
            out.set(field.name, Value::of(in.u8(at) != 0));
            break;
        case Kind::Bool32:
            if (in.u32(at) > 1) fail(here + ": not a flag");
            out.set(field.name, Value::of(in.u32(at) != 0));
            break;
        case Kind::Curve: out.set(field.name, read_curve(in, at, here)); break;
        case Kind::Gradient: out.set(field.name, read_gradient(in, at, here)); break;
        case Kind::Vec3: out.set(field.name, floats(in, at, 3)); break;
        case Kind::Id32: out.set(field.name, Value::of(hex(in.u32(at), 8))); break;
        case Kind::Id64: out.set(field.name, Value::of(hex(in.u64(at), 16))); break;
        case Kind::Pad:
            in.need(at, field.size);
            for (std::size_t i = 0; i < field.size; ++i)
                if (in.b[at + i]) fail(where + ": byte " + std::to_string(at + i) + " (padding) is " + std::to_string(in.b[at + i]));
            break;
        case Kind::Bytes: in.need(at, field.size); out.set(field.name, Value::of(hex_bytes(&in.b[at], field.size))); break;
        case Kind::State:
            if (in.u32(at) != *context.state) fail(here + ": scratch offset " + std::to_string(in.u32(at)) + " out of order");
            break;
        case Kind::Self:
            if (in.u32(at) != context.system) fail(here + ": names system " + std::to_string(in.u32(at)) + ", not its own");
            break;
        case Kind::Const:
            if (*field.name == 0 && in.u32(at) != field.value) fail(where + ": constant at +" + std::to_string(at) + " differs");
            break;
        }
        at += field_size(field);
    }
}

void collect_roles(const In& in, std::size_t at, const Layout& layout, Channels& channels) {
    for (const auto& field : layout) {
        if (field.kind == Kind::Ch && in.u32(at) != kNone) channels.suggest(in.u32(at), field.name);
        at += field_size(field);
    }
}

std::size_t initializer_size(const In& in, std::uint32_t op, std::size_t at) {
    switch (op) {
    case 0x0d: return 0x18 + 24ull * in.u32(at + 0x14);
    case 0x0e: return in.u32(at + 0x2c);
    case 0x11: return 0x48ull + in.u32(at + 0x44);
    default: return layout_size(initializer_specs()[op].layout);
    }
}

Value read_initializer(const In& in, std::size_t& at, const ReadContext& context, const std::string& where) {
    const auto op = in.u32(at);
    const auto& specs = initializer_specs();
    if (op >= specs.size()) fail(where + ": unknown initializer " + std::to_string(op));
    auto out = Value::object();
    out.set("type", Value::of(std::string(specs[op].name)));
    at += 4;
    const auto size = initializer_size(in, op, at);
    if (op == 0x0d) {
        read_fields(in, at, specs[op].layout, context, out, where);
        auto points = Value::array();
        for (std::uint32_t i = 0; i < in.u32(at + 0x14); ++i) points.push(floats(in, at + 0x18 + 24ull * i, 6));
        out.set("points", std::move(points));
    } else if (variable_initializer(op)) {
        in.need(at, size);
        out.set("data", Value::of(hex_bytes(&in.b[at], size)));
    } else {
        read_fields(in, at, specs[op].layout, context, out, where);
    }
    at += size;
    return out;
}

std::uint32_t simulator_state(std::uint32_t op, const Value& simulator) {
    if (op == 0x0e) return static_cast<std::uint32_t>(simulator.find("state_size")->number);
    return simulator_specs()[op].state;
}

Value read_simulator(const In& in, std::uint32_t op, std::size_t at, std::size_t size, const ReadContext& context,
                     const std::string& where) {
    const auto& spec = simulator_specs()[op];
    auto out = Value::object();
    out.set("type", Value::of(std::string(spec.name)));
    if (op == 0x0d) {
        if (size != 88) fail(where + ": burst_emitter data is " + std::to_string(size) + " bytes");
        read_fields(in, at, {STATE()}, context, out, where);
        std::size_t used = 10;
        while (used > 0 && in.f32(at + 4 + 8 * (used - 1)) == 100.0f && in.u32(at + 8 + 8 * (used - 1)) == 0) --used;
        auto bursts = Value::array();
        for (std::size_t i = 0; i < used; ++i) {
            auto burst = Value::object();
            burst.set("time", float_value(in.f32(at + 4 + 8 * i)));
            burst.set("count", number(static_cast<std::int32_t>(in.u32(at + 8 + 8 * i))));
            bursts.push(std::move(burst));
        }
        out.set("bursts", std::move(bursts));
        read_fields(in, at + 84, {SELF()}, context, out, where);
    } else if (op == 0x1c) {
        if (size != 84) fail(where + ": spline_control_points data is " + std::to_string(size) + " bytes");
        read_fields(in, at, {CH("position"), CH("leap_number"), CH("age"), CH("life"), CH("velocity")}, context, out, where);
        const auto n = in.u32(at + 60);
        if (n > 10) fail(where + ": " + std::to_string(n) + " control points");
        auto points = Value::array();
        for (std::uint32_t i = 0; i < 10; ++i) {
            if (i < n) points.push(number(in.u32(at + 20 + 4 * i)));
            else if (in.u32(at + 20 + 4 * i)) fail(where + ": unused control point slot");
        }
        out.set("control_points", std::move(points));
        read_fields(in, at + 64, {U("fix_count"), U("spline_type"), U8("simulate_velocity"), U8("normalize_velocity"),
                                  {Kind::Pad, "", 2}, F("velocity_scale"), B("apply_system_transform")}, context, out, where);
    } else if (opaque_simulator(op)) {
        in.need(at, size);
        out.set("data", Value::of(hex_bytes(&in.b[at], size)));
    } else {
        if (layout_size(spec.layout) != size) fail(where + ": " + spec.name + " data is " + std::to_string(size) + " bytes");
        read_fields(in, at, spec.layout, context, out, where);
    }
    return out;
}

void read_vertex_block(const In& in, std::size_t at, int type, std::vector<Descriptor>& descriptors, VertexLayout& vertex,
                       Value& out, const std::string& where) {
    const auto count = in.u32(at);
    if (count > 10) fail(where + ": " + std::to_string(count) + " vertex channels");
    auto list = Value::array();
    std::map<std::string, int> seen;
    for (std::uint32_t i = 0; i < 10; ++i) {
        const auto d_at = at + 4 + 0x14 * i;
        const Descriptor d = {in.u32(d_at), in.u32(d_at + 4), in.u32(d_at + 8)};
        if (in.u32(d_at + 12) || in.u32(d_at + 16)) fail(where + ": vertex channel slot " + std::to_string(i));
        if (i >= count) {
            if (d[0] || d[1] || d[2]) fail(where + ": unused vertex channel slot is not empty");
            continue;
        }
        if (d[1] > 4 || !component_name(d[0])) fail(where + ": vertex channel " + std::to_string(i));
        descriptors.push_back(d);
        auto channel = Value::object();
        channel.set("component", Value::of(std::string(component_name(d[0]))));
        channel.set("type", Value::of(std::string(kVertexTypes[d[1]])));
        channel.set("set", number(d[2]));
        list.push(std::move(channel));
        std::string name = component_name(d[0]);
        if (d[0] == 5) name += std::to_string(d[2]);
        if (seen[name]++) name += "_" + std::to_string(seen[name]);
        vertex.offsets.push_back(vertex.stride);
        vertex.names.push_back(name);
        vertex.stride += kVertexTypeSize[d[1]];
    }
    (void)type;
    out.set("vertex", std::move(list));
}

std::size_t writer_count_at(int type) {
    const auto& spec = kVisualizers[type];
    return spec.channels_at + 4 + 10 * 0x14 + layout_size(spec.after) + 4;
}

Value read_visualizer(const In& in, std::size_t record, std::size_t end, const ReadContext& base, const std::string& where) {
    const auto type = static_cast<int>(in.u32(record));
    const std::size_t p = record + 4;
    auto out = Value::object();
    if (type == 4) {
        out.set("type", Value::of(std::string("gpu")));
        if (end - p != layout_size(gpu_layout())) fail(where + ": gpu visualizer is " + std::to_string(end - p) + " bytes");
        read_fields(in, p, gpu_layout(), base, out, where);
        return out;
    }
    const auto& spec = kVisualizers[type];
    out.set("type", Value::of(std::string(spec.name)));
    read_fields(in, p, spec.before, base, out, where);
    std::vector<Descriptor> descriptors;
    VertexLayout vertex;
    read_vertex_block(in, p + spec.channels_at, type, descriptors, vertex, out, where);
    const std::size_t after_at = p + spec.channels_at + 4 + 10 * 0x14;
    read_fields(in, after_at, spec.after, base, out, where);
    const std::size_t stride_at = after_at + layout_size(spec.after);
    if (in.u32(stride_at) != vertex.stride) fail(where + ": vertex stride differs from its channels");
    const auto writers = in.u32(stride_at + 4), first = in.u32(stride_at + 8), size = in.u32(stride_at + 12);
    if (p + size != end) fail(where + ": record size");
    std::size_t at = stride_at + 16;
    read_fields(in, at, spec.tail, base, out, where);
    at += layout_size(spec.tail);
    if (type == 0 || type == 3) {
        const auto material = in.u64(p + 8);
        const auto mode = type == 0 ? in.u32(p + 0x18) : 0u;
        if (in.u32(p) != visualizer_hash(type, descriptors, mode, material)) fail(where + ": " + spec.name + " hash differs");
    }
    if (type == 2) {
        const auto count = in.u32(p + 0x0c);
        auto materials = Value::array();
        for (std::uint32_t i = 0; i < count; ++i, at += 16) {
            auto entry = Value::object();
            entry.set("slot", Value::of(hex(in.u64(at), 16)));
            entry.set("material", Value::of(hex(in.u64(at + 8), 16)));
            materials.push(std::move(entry));
        }
        out.set("materials", std::move(materials));
    }
    if (p + first != at) fail(where + ": writers start at " + std::to_string(first));
    ReadContext context = base;
    context.vertex = &vertex;
    auto list = Value::array();
    const auto& specs = writer_specs();
    for (std::uint32_t i = 0; i < writers; ++i) {
        const auto op = in.u32(at);
        if (op >= specs.size()) fail(where + ": unknown vertex writer " + std::to_string(op));
        auto writer = Value::object();
        writer.set("type", Value::of(std::string(specs[op].name)));
        read_fields(in, at + 4, specs[op].layout, context, writer, where + ".writers[" + std::to_string(i) + "]");
        at += 4 + layout_size(specs[op].layout);
        list.push(std::move(writer));
    }
    if (at != end) fail(where + ": writers end " + std::to_string(end - at) + " bytes early");
    out.set("writers", std::move(list));
    return out;
}

void roles_from_writers(const In& in, std::size_t record, Channels& channels) {
    const auto type = in.u32(record);
    if (type > 3) return;
    const std::size_t p = record + 4, count_at = p + writer_count_at(static_cast<int>(type));
    std::size_t at = p + in.u32(count_at + 4);
    for (std::uint32_t i = 0; i < in.u32(count_at); ++i) {
        const auto op = in.u32(at);
        if (op >= writer_specs().size()) return;
        collect_roles(in, at + 4, writer_specs()[op].layout, channels);
        at += 4 + layout_size(writer_specs()[op].layout);
    }
}

Value read_system(const In& in, std::size_t s, std::uint32_t index, const std::string& where) {
    auto out = Value::object();
    out.set("name", Value::of(hex(in.u32(s), 8)));
    out.set("capacity", number(in.u32(s + 4)));
    out.set("min_capacity_scaling", float_value(in.f32(s + 8)));
    Channels channels;
    const auto count = in.u32(s + 0x0c);
    if (count > kMaxChannels) fail(where + ": " + std::to_string(count) + " channels");
    std::uint32_t offset = 0;
    for (std::uint32_t i = 0; i < kMaxChannels; ++i) {
        const auto size = in.u32(s + 0x14 + 4 * i);
        if (i >= count) { if (size) fail(where + ": unused channel slot"); continue; }
        channels.offsets.push_back(offset);
        channels.sizes.push_back(size);
        channels.names.emplace_back();
        offset += size;
    }
    channels.stride = in.u32(s + 0x10);
    if (channels.stride != offset) fail(where + ": channel stride differs from the channel sizes");
    if (in.u32(s + 0x54)) fail(where + ": header word 0x54 is set");
    channels.suggest(in.u32(s + 0x58), "position");

    const auto initializers = in.u32(s + 0x214), simulators = in.u32(s + 0x21c), visualizers = in.u32(s + 0x22c);
    const std::size_t init_at = s + in.u32(s + 0x218), records = s + in.u32(s + 0x220), data = s + in.u32(s + 0x224),
                      vis_at = s + in.u32(s + 0x230), end = s + in.u32(s + 0x23c);
    if (in.u32(s + 0x218) != kSystemHeader) fail(where + ": initializers do not follow the header");
    if (records + 5ull * simulators != data) fail(where + ": simulator records do not end at the data block");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sims;
    for (std::uint32_t i = 0; i < simulators; ++i) {
        const auto op = in.u8(records + 5 * i);
        if (op >= simulator_specs().size()) fail(where + ": unknown simulator " + std::to_string(op));
        sims.emplace_back(op, in.u32(records + 5 * i + 1));
    }
    // channel names: simulators first (their roles are the most telling), then initializers, then writers
    for (const auto& [op, at] : sims) {
        if (op == 0x1c) collect_roles(in, data + at, {CH("position"), CH("leap_number"), CH("age"), CH("life"), CH("velocity")}, channels);
        else if (!opaque_simulator(op) && op != 0x0d) collect_roles(in, data + at, simulator_specs()[op].layout, channels);
    }
    {
        std::size_t at = init_at;
        for (std::uint32_t i = 0; i < initializers; ++i) {
            const auto op = in.u32(at);
            if (op >= initializer_specs().size()) fail(where + ": unknown initializer " + std::to_string(op));
            if (!variable_initializer(op) || op == 0x0d) collect_roles(in, at + 4, initializer_specs()[op].layout, channels);
            at += 4 + initializer_size(in, op, at + 4);
        }
    }
    {
        std::size_t at = vis_at;
        for (std::uint32_t i = 0; i < visualizers && at + 4 <= end; ++i) {
            const auto type = in.u32(at);
            if (type > 4) break;
            roles_from_writers(in, at, channels);
            at += 4 + in.u32(at + 4 + kSizeAt[type]);
        }
    }
    channels.finish();

    auto channel_list = Value::array();
    for (std::size_t i = 0; i < channels.names.size(); ++i) {
        auto channel = Value::object();
        channel.set("name", Value::of(channels.names[i]));
        channel.set("size", number(channels.sizes[i]));
        channel_list.push(std::move(channel));
    }
    out.set("channels", std::move(channel_list));
    out.set("position", channels.reference(in.u32(s + 0x58), where));
    out.set("channel_5c", channels.reference(in.u32(s + 0x5c), where));
    out.set("channel_60", channels.reference(in.u32(s + 0x60), where));
    out.set("collision_plane_offset", channels.reference(in.u32(s + 0x64), where));
    out.set("transform", floats(in, s + 0x68, 16));
    out.set("max_radius", float_value(in.f32(s + 0xa8)));
    if (in.u32(s + 0xac) > 1 || in.u32(s + 0xb0) > 1 || in.u32(s + 0xb4) > 1) fail(where + ": header flags");
    out.set("casts_shadows", Value::of(in.u32(s + 0xac) != 0));
    out.set("disable_culling", Value::of(in.u32(s + 0xb0) != 0));
    auto lod = Value::object();
    lod.set("enabled", Value::of(in.u32(s + 0xb4) != 0));
    lod.set("near", number(in.u32(s + 0xb8)));
    lod.set("far", number(in.u32(s + 0xbc)));
    lod.set("min_step", float_value(in.f32(s + 0xc0)));
    auto curves = Value::array();
    for (const std::size_t c : {0xc4u, 0x118u, 0x16cu, 0x1c0u}) curves.push(read_curve(in, s + c, where + ".simulation_lod"));
    lod.set("curves", std::move(curves));
    out.set("simulation_lod", std::move(lod));

    std::uint32_t state = 0;
    ReadContext context{&channels, nullptr, index, &state};
    auto init_list = Value::array();
    std::size_t at = init_at;
    for (std::uint32_t i = 0; i < initializers; ++i)
        init_list.push(read_initializer(in, at, context, where + ".initializers[" + std::to_string(i) + "]"));
    if (at != records) fail(where + ": initializers end " + std::to_string(records - at) + " bytes early");
    out.set("initializers", std::move(init_list));

    auto sim_list = Value::array();
    std::uint32_t expected = 0;
    for (std::size_t i = 0; i < sims.size(); ++i) {
        const auto swhere = where + ".simulators[" + std::to_string(i) + "]";
        if (sims[i].second != expected) fail(swhere + ": simulator data is not laid out in record order");
        const std::uint32_t next = i + 1 < sims.size() ? sims[i + 1].second : static_cast<std::uint32_t>(vis_at - data);
        auto simulator = read_simulator(in, sims[i].first, data + sims[i].second, next - sims[i].second, context, swhere);
        if (sims[i].first == 0x0e) {
            // grid_collision's scratch holds its grid; the rest of the system's scratch belongs to it
            std::uint32_t after = 0;
            for (std::size_t k = i + 1; k < sims.size(); ++k) after += simulator_specs()[sims[k].first].state;
            simulator.set("state_size", number(in.u32(s + 0x228) - state - after));
        }
        state += simulator_state(sims[i].first, simulator);
        sim_list.push(std::move(simulator));
        expected = next;
    }
    if (data + expected != vis_at) fail(where + ": simulator data does not end at the visualizers");
    if (in.u32(s + 0x228) != state) fail(where + ": scratch size " + std::to_string(in.u32(s + 0x228)) + " is not the simulators' " + std::to_string(state));
    out.set("simulators", std::move(sim_list));
    out.set("field_234", number(in.u32(s + 0x234)));
    out.set("field_238", number(in.u32(s + 0x238)));

    auto vis_list = Value::array();
    at = vis_at;
    for (std::uint32_t i = 0; i < visualizers; ++i) {
        const auto type = in.u32(at);
        if (type > 4) fail(where + ": unknown visualizer type " + std::to_string(type));
        const std::size_t record_end = at + 4 + in.u32(at + 4 + kSizeAt[type]);
        if (record_end > end) fail(where + ": visualizer runs past the system");
        vis_list.push(read_visualizer(in, at, record_end, context, where + ".visualizers[" + std::to_string(i) + "]"));
        at = record_end;
    }
    if (at != end) fail(where + ": visualizers end " + std::to_string(end - at) + " bytes early");
    out.set("visualizers", std::move(vis_list));
    return out;
}

// ---------------------------------------------------------------------------------------------------------
// Writing
struct Out {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) { const auto at = b.size(); b.resize(at + 4); std::memcpy(&b[at], &v, 4); }
    void u64(std::uint64_t v) { const auto at = b.size(); b.resize(at + 8); std::memcpy(&b[at], &v, 8); }
    void f32(float v) { const auto at = b.size(); b.resize(at + 4); std::memcpy(&b[at], &v, 4); }
    void put32(std::size_t at, std::uint32_t v) { std::memcpy(&b[at], &v, 4); }
    void putf(std::size_t at, float v) { std::memcpy(&b[at], &v, 4); }
    void zeros(std::size_t n) { b.resize(b.size() + n, 0); }
};

const Value& member(const Value& object, const char* name, const std::string& where) {
    const auto* v = object.find(name);
    if (!v) fail(where + ": missing '" + name + "'");
    return *v;
}
double as_number(const Value& v, const std::string& where) {
    if (!v.is_number()) fail(where + ": expected a number");
    return v.number;
}
std::uint32_t as_u32(const Value& v, const std::string& where) {
    const double n = as_number(v, where);
    if (n < 0 || n > 4294967295.0 || n != std::floor(n)) fail(where + ": expected a whole number from 0");
    return static_cast<std::uint32_t>(n);
}
std::int32_t as_i32(const Value& v, const std::string& where) {
    const double n = as_number(v, where);
    if (n < -2147483648.0 || n > 2147483647.0 || n != std::floor(n)) fail(where + ": expected a whole number");
    return static_cast<std::int32_t>(n);
}
float as_f32(const Value& v, const std::string& where) {
    if (v.is_string() && v.string.size() == 9 && v.string[0] == '#') {
        const auto bits = static_cast<std::uint32_t>(std::stoul(v.string.substr(1), nullptr, 16));
        float f; std::memcpy(&f, &bits, 4); return f;
    }
    return static_cast<float>(as_number(v, where));
}
bool as_bool(const Value& v, const std::string& where) {
    if (v.kind != Value::Kind::Bool) fail(where + ": expected true or false");
    return v.boolean;
}
// "#<hex>" is the id itself; any other text is a name, hashed the way the game does
std::uint64_t as_id(const Value& v, int digits, const std::string& where) {
    if (!v.is_string() || v.string.empty()) fail(where + ": expected a name");
    if (v.string[0] == '#' && v.string.size() == static_cast<std::size_t>(digits) + 1)
        return std::stoull(v.string.substr(1), nullptr, 16);
    return digits == 8 ? id32_from_id64(v.string) : id64(v.string);
}
std::vector<std::uint8_t> as_bytes(const Value& v, std::size_t size, const std::string& where) {
    if (!v.is_string() || (size && v.string.size() != size * 2) || v.string.size() % 2) fail(where + ": expected hex bytes");
    std::vector<std::uint8_t> out(v.string.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint8_t>(std::stoul(v.string.substr(2 * i, 2), nullptr, 16));
    return out;
}
const Value& list(const Value& object, const char* name, const std::string& where) {
    const auto& v = member(object, name, where);
    if (!v.is_array()) fail(where + ": '" + name + "' is a list");
    return v;
}

struct ChannelMap {
    std::map<std::string, std::uint32_t> offset;
    std::uint32_t stride = 0;
    const char* what = "channel";
    std::uint32_t resolve(const Value& v, const std::string& where) const {
        if (v.kind == Value::Kind::Null) return kNone;
        if (v.is_number()) return as_u32(v, where);
        if (!v.is_string()) fail(where + ": expected a " + what + " name");
        std::string name = v.string;
        std::uint32_t extra = 0;
        if (const auto plus = name.find('+'); plus != std::string::npos) {
            extra = static_cast<std::uint32_t>(std::stoul(name.substr(plus + 1)));
            name.resize(plus);
        }
        const auto found = offset.find(name);
        if (found == offset.end()) fail(where + ": no " + what + " named '" + name + "'");
        return found->second + extra;
    }
};

struct WriteContext {
    const ChannelMap* channels = nullptr;
    const ChannelMap* vertex = nullptr;
    std::uint32_t system = 0;
    std::uint32_t state = 0;
};

void write_points(Out& out, const Value& x, std::size_t n, const std::string& where) {
    for (std::size_t i = 0; i < 10; ++i)
        out.f32(i < n ? as_f32(x.items[i], where) : n ? 10000.0f + 10.0f * static_cast<float>(n) : 0.0f);
}
void write_curve(Out& out, const Value& curve, const std::string& where) {
    const auto& x = member(curve, "x", where);
    const auto& y = member(curve, "y", where);
    if (!x.is_array() || !y.is_array() || x.items.size() != y.items.size() || x.items.size() > 10)
        fail(where + ": a curve has up to 10 points, as matching x and y lists");
    const auto n = x.items.size();
    write_points(out, x, n, where);
    for (std::size_t i = 0; i < 10; ++i) out.f32(n ? as_f32(y.items[i < n ? i : n - 1], where) : 0.0f);
    out.u32(static_cast<std::uint32_t>(n));
}
void write_floats(Out& out, const Value& v, std::size_t count, const std::string& where) {
    if (!v.is_array() || v.items.size() != count) fail(where + ": expected " + std::to_string(count) + " numbers");
    for (const auto& item : v.items) out.f32(as_f32(item, where));
}
void write_gradient(Out& out, const Value& gradient, const std::string& where) {
    const auto& x = member(gradient, "x", where);
    const auto& rgb = member(gradient, "rgb", where);
    if (!x.is_array() || !rgb.is_array() || x.items.size() != rgb.items.size() || x.items.size() > 10)
        fail(where + ": a gradient has up to 10 points, as matching x and rgb lists");
    const auto n = x.items.size();
    write_points(out, x, n, where);
    for (std::size_t i = 0; i < 10; ++i) {
        if (n) write_floats(out, rgb.items[i < n ? i : n - 1], 3, where);
        else out.zeros(12);
    }
    out.u32(static_cast<std::uint32_t>(n));
}

void write_fields(Out& out, const Value& in, const Layout& layout, const WriteContext& context, const std::string& where) {
    for (const auto& field : layout) {
        const auto here = where + "." + field.name;
        switch (field.kind) {
        case Kind::Pad: out.zeros(field.size); continue;
        case Kind::State: out.u32(context.state); continue;
        case Kind::Self: out.u32(context.system); continue;
        case Kind::Const: out.u32(field.value); continue;
        default: break;
        }
        const auto& v = member(in, field.name, where);
        switch (field.kind) {
        case Kind::Ch: out.u32(context.channels->resolve(v, here)); break;
        case Kind::Vtx: out.u32(context.vertex->resolve(v, here)); break;
        case Kind::U32: out.u32(as_u32(v, here)); break;
        case Kind::I32: out.u32(static_cast<std::uint32_t>(as_i32(v, here))); break;
        case Kind::F32: out.f32(as_f32(v, here)); break;
        case Kind::U8: { const auto n = as_u32(v, here); if (n > 255) fail(here + ": 0..255"); out.u8(static_cast<std::uint8_t>(n)); break; }
        case Kind::Bool8: out.u8(as_bool(v, here) ? 1 : 0); out.zeros(3); break;
        case Kind::Bool32: out.u32(as_bool(v, here) ? 1 : 0); break;
        case Kind::Curve: write_curve(out, v, here); break;
        case Kind::Gradient: write_gradient(out, v, here); break;
        case Kind::Vec3: write_floats(out, v, 3, here); break;
        case Kind::Id32: out.u32(static_cast<std::uint32_t>(as_id(v, 8, here))); break;
        case Kind::Id64: out.u64(as_id(v, 16, here)); break;
        case Kind::Bytes: { const auto bytes = as_bytes(v, field.size, here); out.b.insert(out.b.end(), bytes.begin(), bytes.end()); break; }
        default: break;
        }
    }
}

int op_of(const std::vector<OpSpec>& specs, const Value& item, const std::string& where) {
    const auto& type = member(item, "type", where);
    const int op = type.is_string() ? find_spec(specs, type.string) : -1;
    if (op < 0) fail(where + ": unknown type '" + (type.is_string() ? type.string : std::string()) + "'");
    return op;
}

void write_visualizer(Out& out, const Value& v, const WriteContext& base, const std::string& where) {
    const auto& type_name = member(v, "type", where);
    int type = -1;
    for (int t = 0; t < 4; ++t) if (type_name.is_string() && type_name.string == kVisualizers[t].name) type = t;
    if (type_name.is_string() && type_name.string == "gpu") type = 4;
    if (type < 0) fail(where + ": visualizer types are billboard, light, mesh, ribbon and gpu");
    out.u32(static_cast<std::uint32_t>(type));
    const std::size_t p = out.b.size();
    if (type == 4) { write_fields(out, v, gpu_layout(), base, where); return; }
    const auto& spec = kVisualizers[type];
    // vertex channels first: the hash and the writers need them
    const auto& channel_list = list(v, "vertex", where);
    if (channel_list.items.size() > 10) fail(where + ": up to 10 vertex channels");
    std::vector<Descriptor> descriptors;
    ChannelMap vertex;
    vertex.what = "vertex channel";
    std::map<std::string, int> seen;
    for (const auto& channel : channel_list.items) {
        const auto& component = member(channel, "component", where);
        const auto& vtype = member(channel, "type", where);
        std::uint32_t c = 99, t = 99;
        for (std::uint32_t i = 0; i < 7; ++i) if (component_name(i) && component.is_string() && component.string == component_name(i)) c = i;
        for (std::uint32_t i = 0; i < 5; ++i) if (vtype.is_string() && vtype.string == kVertexTypes[i]) t = i;
        if (c == 99 || t == 99)
            fail(where + ": a vertex channel has a component (position, tangent, binormal, texcoord, color) and a type (float1..float4, ubyte4)");
        const auto set = as_u32(member(channel, "set", where), where + ".set");
        descriptors.push_back({c, t, set});
        std::string name = component_name(c);
        if (c == 5) name += std::to_string(set);
        if (seen[name]++) name += "_" + std::to_string(seen[name]);
        vertex.offset[name] = vertex.stride;
        vertex.stride += kVertexTypeSize[t];
    }
    const Value* materials = type == 2 ? &list(v, "materials", where) : nullptr;
    write_fields(out, v, spec.before, base, where);
    if (type == 0 || type == 3) {
        const auto material = as_id(member(v, "material", where), 16, where + ".material");
        const auto mode = type == 0 ? as_u32(member(v, "mode", where), where + ".mode") : 0u;
        out.put32(p, visualizer_hash(type, descriptors, mode, material));
    }
    if (type == 2) out.put32(p + 0x0c, static_cast<std::uint32_t>(materials->items.size()));
    out.u32(static_cast<std::uint32_t>(descriptors.size()));
    for (std::size_t i = 0; i < 10; ++i) {
        if (i < descriptors.size()) { out.u32(descriptors[i][0]); out.u32(descriptors[i][1]); out.u32(descriptors[i][2]); }
        else out.zeros(12);
        out.zeros(8);
    }
    write_fields(out, v, spec.after, base, where);
    out.u32(vertex.stride);
    const auto& writers = list(v, "writers", where);
    out.u32(static_cast<std::uint32_t>(writers.items.size()));
    const std::size_t first_at = out.b.size();
    out.u32(0);
    const std::size_t size_at = out.b.size();
    out.u32(0);
    write_fields(out, v, spec.tail, base, where);
    if (materials)
        for (const auto& entry : materials->items) {
            out.u64(as_id(member(entry, "slot", where), 16, where + ".materials.slot"));
            out.u64(as_id(member(entry, "material", where), 16, where + ".materials.material"));
        }
    out.put32(first_at, static_cast<std::uint32_t>(out.b.size() - p));
    WriteContext context = base;
    context.vertex = &vertex;
    for (std::size_t i = 0; i < writers.items.size(); ++i) {
        const auto wwhere = where + ".writers[" + std::to_string(i) + "]";
        const int op = op_of(writer_specs(), writers.items[i], wwhere);
        out.u32(static_cast<std::uint32_t>(op));
        write_fields(out, writers.items[i], writer_specs()[op].layout, context, wwhere);
    }
    out.put32(size_at, static_cast<std::uint32_t>(out.b.size() - p));
}

void write_system(Out& out, const Value& v, std::uint32_t index, const std::string& where) {
    const std::size_t s = out.b.size();
    out.zeros(kSystemHeader);
    out.put32(s, static_cast<std::uint32_t>(as_id(member(v, "name", where), 8, where + ".name")));
    out.put32(s + 4, as_u32(member(v, "capacity", where), where + ".capacity"));
    out.putf(s + 8, as_f32(member(v, "min_capacity_scaling", where), where));
    ChannelMap channels;
    const auto& channel_list = list(v, "channels", where);
    if (channel_list.items.size() > kMaxChannels) fail(where + ": a system has up to 16 channels");
    for (std::size_t i = 0; i < channel_list.items.size(); ++i) {
        const auto& c = channel_list.items[i];
        const auto& name = member(c, "name", where);
        const auto size = as_u32(member(c, "size", where), where + ".channels.size");
        if (!name.is_string() || name.string.empty() || name.string.find('+') != std::string::npos || channels.offset.count(name.string))
            fail(where + ": channel names are unique and have no '+'");
        channels.offset[name.string] = channels.stride;
        out.put32(s + 0x14 + 4 * i, size);
        channels.stride += size;
    }
    out.put32(s + 0x0c, static_cast<std::uint32_t>(channel_list.items.size()));
    out.put32(s + 0x10, channels.stride);
    out.put32(s + 0x58, channels.resolve(member(v, "position", where), where + ".position"));
    out.put32(s + 0x5c, channels.resolve(member(v, "channel_5c", where), where + ".channel_5c"));
    out.put32(s + 0x60, channels.resolve(member(v, "channel_60", where), where + ".channel_60"));
    out.put32(s + 0x64, channels.resolve(member(v, "collision_plane_offset", where), where + ".collision_plane_offset"));
    const auto& transform = member(v, "transform", where);
    if (!transform.is_array() || transform.items.size() != 16) fail(where + ": transform is 16 numbers");
    for (std::size_t i = 0; i < 16; ++i) out.putf(s + 0x68 + 4 * i, as_f32(transform.items[i], where + ".transform"));
    out.putf(s + 0xa8, as_f32(member(v, "max_radius", where), where + ".max_radius"));
    out.put32(s + 0xac, as_bool(member(v, "casts_shadows", where), where + ".casts_shadows") ? 1 : 0);
    out.put32(s + 0xb0, as_bool(member(v, "disable_culling", where), where + ".disable_culling") ? 1 : 0);
    {
        const auto lwhere = where + ".simulation_lod";
        const auto& lod = member(v, "simulation_lod", where);
        out.put32(s + 0xb4, as_bool(member(lod, "enabled", lwhere), lwhere) ? 1 : 0);
        out.put32(s + 0xb8, as_u32(member(lod, "near", lwhere), lwhere + ".near"));
        out.put32(s + 0xbc, as_u32(member(lod, "far", lwhere), lwhere + ".far"));
        out.putf(s + 0xc0, as_f32(member(lod, "min_step", lwhere), lwhere + ".min_step"));
        const auto& curves = list(lod, "curves", lwhere);
        if (curves.items.size() != 4) fail(lwhere + ": 4 curves");
        std::size_t c = 0;
        for (const std::size_t at : {0xc4u, 0x118u, 0x16cu, 0x1c0u}) {
            Out curve;
            write_curve(curve, curves.items[c++], lwhere);
            std::memcpy(&out.b[s + at], curve.b.data(), kCurve);
        }
    }
    WriteContext context{&channels, nullptr, index, 0};
    const auto& inits = list(v, "initializers", where);
    out.put32(s + 0x214, static_cast<std::uint32_t>(inits.items.size()));
    out.put32(s + 0x218, static_cast<std::uint32_t>(kSystemHeader));
    for (std::size_t i = 0; i < inits.items.size(); ++i) {
        const auto iwhere = where + ".initializers[" + std::to_string(i) + "]";
        const auto& item = inits.items[i];
        const auto op = static_cast<std::uint32_t>(op_of(initializer_specs(), item, iwhere));
        out.u32(op);
        if (op == 0x0d) {
            write_fields(out, item, initializer_specs()[op].layout, context, iwhere);
            const auto& points = list(item, "points", iwhere);
            out.u32(static_cast<std::uint32_t>(points.items.size()));
            for (const auto& point : points.items) write_floats(out, point, 6, iwhere + ".points");
        } else if (variable_initializer(op)) {
            const auto data = as_bytes(member(item, "data", iwhere), 0, iwhere + ".data");
            out.b.insert(out.b.end(), data.begin(), data.end());
        } else {
            write_fields(out, item, initializer_specs()[op].layout, context, iwhere);
        }
    }
    // simulators: records, then their data in record order; scratch offsets in record order too
    const auto& sims = list(v, "simulators", where);
    out.put32(s + 0x21c, static_cast<std::uint32_t>(sims.items.size()));
    out.put32(s + 0x220, static_cast<std::uint32_t>(out.b.size() - s));
    const std::size_t records = out.b.size();
    out.zeros(5 * sims.items.size());
    const std::size_t data = out.b.size();
    out.put32(s + 0x224, static_cast<std::uint32_t>(data - s));
    for (std::size_t i = 0; i < sims.items.size(); ++i) {
        const auto swhere = where + ".simulators[" + std::to_string(i) + "]";
        const auto& item = sims.items[i];
        const auto op = static_cast<std::uint32_t>(op_of(simulator_specs(), item, swhere));
        out.b[records + 5 * i] = static_cast<std::uint8_t>(op);
        const auto offset = static_cast<std::uint32_t>(out.b.size() - data);
        std::memcpy(&out.b[records + 5 * i + 1], &offset, 4);
        if (op == 0x0d) {
            write_fields(out, item, {STATE()}, context, swhere);
            const auto& bursts = list(item, "bursts", swhere);
            if (bursts.items.size() > 10) fail(swhere + ": up to 10 bursts");
            for (std::size_t k = 0; k < 10; ++k) {
                if (k < bursts.items.size()) {
                    out.f32(as_f32(member(bursts.items[k], "time", swhere), swhere + ".time"));
                    out.u32(static_cast<std::uint32_t>(as_i32(member(bursts.items[k], "count", swhere), swhere + ".count")));
                } else { out.f32(100.0f); out.u32(0); }
            }
            write_fields(out, item, {SELF()}, context, swhere);
        } else if (op == 0x1c) {
            write_fields(out, item, {CH("position"), CH("leap_number"), CH("age"), CH("life"), CH("velocity")}, context, swhere);
            const auto& points = list(item, "control_points", swhere);
            if (points.items.size() > 10) fail(swhere + ": up to 10 control points");
            for (std::size_t k = 0; k < 10; ++k) out.u32(k < points.items.size() ? as_u32(points.items[k], swhere) : 0u);
            out.u32(static_cast<std::uint32_t>(points.items.size()));
            write_fields(out, item, {U("fix_count"), U("spline_type"), U8("simulate_velocity"), U8("normalize_velocity"),
                                     PAD(2), F("velocity_scale"), B("apply_system_transform")}, context, swhere);
        } else if (opaque_simulator(op)) {
            const auto bytes = as_bytes(member(item, "data", swhere), 0, swhere + ".data");
            out.b.insert(out.b.end(), bytes.begin(), bytes.end());
        } else {
            write_fields(out, item, simulator_specs()[op].layout, context, swhere);
        }
        if (op == 0x0e) context.state += as_u32(member(item, "state_size", swhere), swhere + ".state_size");
        else context.state += simulator_specs()[op].state;
    }
    out.put32(s + 0x228, context.state);
    out.put32(s + 0x234, as_u32(member(v, "field_234", where), where));
    out.put32(s + 0x238, as_u32(member(v, "field_238", where), where));
    const auto& vis = list(v, "visualizers", where);
    out.put32(s + 0x22c, static_cast<std::uint32_t>(vis.items.size()));
    out.put32(s + 0x230, static_cast<std::uint32_t>(out.b.size() - s));
    for (std::size_t i = 0; i < vis.items.size(); ++i)
        write_visualizer(out, vis.items[i], context, where + ".visualizers[" + std::to_string(i) + "]");
    out.put32(s + 0x23c, static_cast<std::uint32_t>(out.b.size() - s));
}

void collect_references(const Value& v, std::vector<Reference>& out) {
    if (v.is_object()) {
        for (const auto& [name, member] : v.members) {
            const bool material = name.rfind("material", 0) == 0 && name.find('_') == std::string::npos;
            const bool numbered = name.rfind("material_", 0) == 0 && name.size() == 10;
            if ((material || numbered || name == "unit") && member.is_string() && member.string.size() == 17 && member.string[0] == '#')
                out.push_back({name == "unit" ? "unit" : "material", std::stoull(member.string.substr(1), nullptr, 16)});
            collect_references(member, out);
        }
    } else if (v.is_array()) {
        for (const auto& item : v.items) collect_references(item, out);
    }
}
}

bool decode(const std::vector<std::uint8_t>& body, json::Value& effect, std::string& error) {
    try {
        const In in{body};
        if (in.u32(0) != kVersion) fail("not a version 102 particles resource");
        effect = Value::object();
        effect.set("life_time", float_value(in.f32(0x04)));
        effect.set("culling_distance", float_value(in.f32(0x08)));
        for (const std::size_t at : {0x0cu, 0x10u, 0x1cu, 0x20u})
            if (in.u32(at) > 1) fail("effect header flag at " + std::to_string(at));
        effect.set("use_random_seed", Value::of(in.u32(0x0c) != 0));
        effect.set("flag_10", Value::of(in.u32(0x10) != 0));
        effect.set("field_14", number(in.u32(0x14)));
        effect.set("field_18", float_value(in.f32(0x18)));
        effect.set("casts_shadows", Value::of(in.u32(0x1c) != 0));
        effect.set("disable_culling", Value::of(in.u32(0x20) != 0));
        const auto variables = in.u32(0x24), systems = in.u32(0x28);
        auto variable_list = Value::array();
        for (std::uint32_t i = 0; i < variables; ++i) {
            auto variable = Value::object();
            variable.set("name", Value::of(hex(in.u32(kEffectHeader + 4ull * i), 8)));
            variable.set("value", floats(in, kEffectHeader + 4ull * variables + 12ull * i, 3));
            variable_list.push(std::move(variable));
        }
        effect.set("variables", std::move(variable_list));
        auto system_list = Value::array();
        std::size_t at = kEffectHeader + 16ull * variables;
        for (std::uint32_t i = 0; i < systems; ++i) {
            const auto stride = in.u32(at + 0x23c);
            in.need(at, stride);
            system_list.push(read_system(in, at, i, "systems[" + std::to_string(i) + "]"));
            at += stride;
        }
        if (at != body.size()) fail(std::to_string(body.size() - at) + " bytes after the last system");
        effect.set("systems", std::move(system_list));
        return true;
    } catch (const Failure& failure) {
        error = failure.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("particles: ") + e.what();
        return false;
    }
}

bool encode(const json::Value& effect, std::vector<std::uint8_t>& body, std::string& error) {
    try {
        if (!effect.is_object()) fail("a particle effect is a JSON object");
        Out out;
        out.u32(kVersion);
        out.f32(as_f32(member(effect, "life_time", "effect"), "life_time"));
        out.f32(as_f32(member(effect, "culling_distance", "effect"), "culling_distance"));
        out.u32(as_bool(member(effect, "use_random_seed", "effect"), "use_random_seed") ? 1 : 0);
        out.u32(as_bool(member(effect, "flag_10", "effect"), "flag_10") ? 1 : 0);
        out.u32(as_u32(member(effect, "field_14", "effect"), "field_14"));
        out.f32(as_f32(member(effect, "field_18", "effect"), "field_18"));
        out.u32(as_bool(member(effect, "casts_shadows", "effect"), "casts_shadows") ? 1 : 0);
        out.u32(as_bool(member(effect, "disable_culling", "effect"), "disable_culling") ? 1 : 0);
        const auto& variables = list(effect, "variables", "effect");
        const auto& systems = list(effect, "systems", "effect");
        out.u32(static_cast<std::uint32_t>(variables.items.size()));
        out.u32(static_cast<std::uint32_t>(systems.items.size()));
        for (const auto& variable : variables.items)
            out.u32(static_cast<std::uint32_t>(as_id(member(variable, "name", "variables"), 8, "variables.name")));
        for (const auto& variable : variables.items) write_floats(out, member(variable, "value", "variables"), 3, "variables.value");
        for (std::size_t i = 0; i < systems.items.size(); ++i)
            write_system(out, systems.items[i], static_cast<std::uint32_t>(i), "systems[" + std::to_string(i) + "]");
        body = std::move(out.b);
        return true;
    } catch (const Failure& failure) {
        error = failure.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("particles: ") + e.what();
        return false;
    }
}

namespace {
const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Ch: return "channel";
    case Kind::Vtx: return "vertex";
    case Kind::U32: return "uint";
    case Kind::I32: return "int";
    case Kind::F32: return "float";
    case Kind::U8: return "byte";
    case Kind::Bool8: case Kind::Bool32: return "bool";
    case Kind::Curve: return "curve";
    case Kind::Gradient: return "gradient";
    case Kind::Vec3: return "vector3";
    case Kind::Id32: return "name";
    case Kind::Id64: return "resource";
    case Kind::Bytes: return "bytes";
    default: return nullptr;   // filled in by the encoder
    }
}
Value schema_field(const std::string& name, const std::string& kind) {
    auto field = Value::object();
    field.set("name", Value::of(name));
    field.set("kind", Value::of(kind));
    return field;
}
Value schema_fields(const Layout& layout) {
    auto fields = Value::array();
    for (const auto& field : layout)
        if (const char* kind = kind_name(field.kind)) fields.push(schema_field(field.name, kind));
    return fields;
}
Value schema_item(const char* type, Value fields) {
    auto item = Value::object();
    item.set("type", Value::of(std::string(type)));
    item.set("fields", std::move(fields));
    return item;
}
}

json::Value schema() {
    // kinds with their own JSON shape: bursts [{time, count}], control_points [variable indices],
    // points [[x, y, z, nx, ny, nz]], bytes (hex)
    auto out = Value::object();
    auto initializers = Value::array();
    for (std::uint32_t op = 0; op < initializer_specs().size(); ++op) {
        const auto& spec = initializer_specs()[op];
        auto fields = op == 0x0e || op == 0x11 ? schema_fields({{Kind::Bytes, "data"}}) : schema_fields(spec.layout);
        if (op == 0x0d) fields.push(schema_field("points", "points"));
        initializers.push(schema_item(spec.name, std::move(fields)));
    }
    out.set("initializers", std::move(initializers));
    auto simulators = Value::array();
    for (std::uint32_t op = 0; op < simulator_specs().size(); ++op) {
        const auto& spec = simulator_specs()[op];
        Value fields;
        if (opaque_simulator(op)) {
            fields = schema_fields({{Kind::Bytes, "data"}});
        } else if (op == 0x0d) {
            fields = Value::array();
            fields.push(schema_field("bursts", "bursts"));
        } else if (op == 0x1c) {
            fields = schema_fields({CH("position"), CH("leap_number"), CH("age"), CH("life"), CH("velocity")});
            fields.push(schema_field("control_points", "control_points"));
            for (auto& field : schema_fields({U("fix_count"), U("spline_type"), U8("simulate_velocity"), U8("normalize_velocity"),
                                              F("velocity_scale"), B("apply_system_transform")}).items)
                fields.push(std::move(field));
        } else {
            fields = schema_fields(spec.layout);
            if (op == 0x0e) fields.push(schema_field("state_size", "uint"));
        }
        simulators.push(schema_item(spec.name, std::move(fields)));
    }
    out.set("simulators", std::move(simulators));
    auto writers = Value::array();
    for (const auto& spec : writer_specs()) writers.push(schema_item(spec.name, schema_fields(spec.layout)));
    out.set("writers", std::move(writers));
    auto visualizers = Value::array();
    for (const auto& spec : kVisualizers) {
        Layout fields = spec.before;
        fields.insert(fields.end(), spec.after.begin(), spec.after.end());
        fields.insert(fields.end(), spec.tail.begin(), spec.tail.end());
        visualizers.push(schema_item(spec.name, schema_fields(fields)));
    }
    visualizers.push(schema_item("gpu", schema_fields(gpu_layout())));
    out.set("visualizers", std::move(visualizers));
    return out;
}

std::vector<Reference> resource_references(const json::Value& effect) {
    std::vector<Reference> out;
    collect_references(effect, out);
    return out;
}

}
