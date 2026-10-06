#include "stingray/resource_name.h"
#include "stingray/bones/bone_identity.h"
#include "stingray/unit/unit_identity.h"
#include "stingray/unit/unit_v115.h"
#include "stingray/physics/authored_physics.h"
#include "stingray/binary_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "stingray/physics/physx_cooking.h"
#include "stingray/flow/flow_authoring.h"
#include "stingray/flow/flow_resource.h"
#include "stingray/unit/script_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <numeric>
#include <set>
#include <map>
#include <vector>

namespace dtglb::stingray::unit {
namespace {

constexpr std::uint32_t kUnitVersion = 0x73u;
constexpr std::uint32_t kMeshGeometryVersion = 1u;
constexpr std::uint32_t kSimpleStaticRenderFlags = 0x000c2003u;
constexpr std::uint32_t kMeshObjectKind = 3u;
constexpr std::uint32_t kMeshObjectEnabled = 1u;
constexpr std::uint32_t kMaxStaticTexcoordSet = 6u;
// LOD object word after the flags: Darktide-only, 0 or 1 in the game's units (0 in 65% of them).
constexpr std::uint32_t kLodObjectX = 0u;
constexpr std::size_t kMaxSceneNodes = 65535u;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct PackedChannel {
    std::uint32_t component = 0;
    std::uint32_t type = 0;
    std::uint32_t set = 0;
    std::uint32_t stream = 0;
};
struct PackedGeometry {
    std::uint32_t vertices = 0;
    std::vector<std::vector<std::uint8_t>> streams;
    std::vector<std::uint32_t> strides;
    std::vector<PackedChannel> channels;
};
struct SceneGraphNode {
    std::array<float, 9> rotation{};
    std::array<float, 3> position{};
    std::array<float, 3> scale{};
    Matrix4 world{};
    std::uint16_t parent_type = 0;
    std::uint16_t parent_index = 0;
    std::uint32_t name_hash = 0;
};

const VertexChannel* find_channel(const Primitive& p, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    for (const auto& channel : p.channels) {
        if (channel.semantic == semantic && channel.set == set) return &channel;
    }
    return nullptr;
}

std::uint32_t vertex_count(const Primitive& p) {
    const auto* position = find_channel(p, VertexChannel::Semantic::Position);
    if (!position || !position->components) return 0;
    return static_cast<std::uint32_t>(position->values.size() / position->components);
}

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 normalize(Vec3 v, Vec3 fallback = {0, 0, 1}) {
    const float n = length(v);
    if (!(n > 1e-30f) || !std::isfinite(n)) return fallback;
    return mul(v, 1.0f / n);
}

Vec3 row3(const VertexChannel& channel, std::uint32_t index) {
    const auto base = static_cast<std::size_t>(index) * channel.components;
    return {channel.values[base], channel.values[base + 1], channel.values[base + 2]};
}

Vec2 row2(const VertexChannel& channel, std::uint32_t index) {
    const auto base = static_cast<std::size_t>(index) * channel.components;
    return {channel.values[base], channel.values[base + 1]};
}

bool finite_channel(const VertexChannel& channel, std::uint32_t vertices, std::uint32_t minimum_components) {
    if (channel.components < minimum_components) return false;
    const auto needed = static_cast<std::size_t>(vertices) * channel.components;
    if (channel.values.size() < needed) return false;
    return std::all_of(channel.values.begin(), channel.values.begin() + static_cast<std::ptrdiff_t>(needed),
                       [](float value) { return std::isfinite(value); });
}

bool float_to_half(float value, std::uint16_t& output) {
    if (!std::isfinite(value) || std::fabs(value) > 65504.0f) return false;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;
    int half_exponent = static_cast<int>(exponent) - 127 + 15;

    if (exponent == 0) {
        output = static_cast<std::uint16_t>(sign);
        return true;
    }
    if (half_exponent <= 0) {
        if (half_exponent < -10) {
            output = static_cast<std::uint16_t>(sign);
            return true;
        }
        mantissa |= 0x800000u;
        const int shift = 14 - half_exponent;
        std::uint32_t half_mantissa = mantissa >> shift;
        const std::uint32_t remainder_mask = (1u << shift) - 1u;
        const std::uint32_t remainder = mantissa & remainder_mask;
        const std::uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half_mantissa & 1u))) ++half_mantissa;
        output = static_cast<std::uint16_t>(sign | half_mantissa);
        return true;
    }

    std::uint32_t half_mantissa = mantissa >> 13;
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half_mantissa & 1u))) {
        ++half_mantissa;
        if (half_mantissa == 0x400u) {
            half_mantissa = 0;
            ++half_exponent;
        }
    }
    if (half_exponent >= 31) return false;
    output = static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(half_exponent) << 10) | half_mantissa);
    return true;
}

bool append_half(std::vector<std::uint8_t>& out, float value) {
    std::uint16_t half = 0;
    if (!float_to_half(value, half)) return false;
    out.push_back(static_cast<std::uint8_t>(half & 0xffu));
    out.push_back(static_cast<std::uint8_t>(half >> 8));
    return true;
}

Vec2 oct_encode(Vec3 value) {
    value = normalize(value);
    const float denominator = std::fabs(value.x) + std::fabs(value.y) + std::fabs(value.z);
    if (!(denominator > 1e-30f)) return {0.5f, 0.5f};
    float x = value.x / denominator;
    float y = value.y / denominator;
    const float z = value.z / denominator;
    if (z < 0.0f) {
        const float ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}

void tangent_frame(const std::vector<Vec3>& positions,
                   const std::vector<Vec2>& uv,
                   const std::vector<Vec3>& normals,
                   const std::vector<std::uint32_t>& indices,
                   std::vector<Vec3>& tangents,
                   std::vector<Vec3>& bitangents) {
    std::vector<Vec3> tangent_accum(positions.size());
    std::vector<Vec3> bitangent_accum(positions.size());
    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
        const auto i0 = indices[triangle];
        const auto i1 = indices[triangle + 1];
        const auto i2 = indices[triangle + 2];
        const Vec3 e1 = sub(positions[i1], positions[i0]);
        const Vec3 e2 = sub(positions[i2], positions[i0]);
        const float du1 = uv[i1].x - uv[i0].x;
        const float dv1 = uv[i1].y - uv[i0].y;
        const float du2 = uv[i2].x - uv[i0].x;
        const float dv2 = uv[i2].y - uv[i0].y;
        const float denominator = du1 * dv2 - du2 * dv1;
        const float area = length(cross(e1, e2));
        const float weight = area > 1e-20f ? area : 1.0f;
        Vec3 tangent{};
        Vec3 bitangent{};
        if (std::fabs(denominator) > 1e-20f) {
            const float inv = 1.0f / denominator;
            tangent = mul(sub(mul(e1, dv2), mul(e2, dv1)), inv);
            bitangent = mul(sub(mul(e2, du1), mul(e1, du2)), inv);
        } else {
            tangent = length(e1) > 1e-20f ? e1 : e2;
            bitangent = cross(normals[i0], tangent);
        }
        tangent = mul(tangent, weight);
        bitangent = mul(bitangent, weight);
        for (const auto index : {i0, i1, i2}) {
            tangent_accum[index] = add(tangent_accum[index], tangent);
            bitangent_accum[index] = add(bitangent_accum[index], bitangent);
        }
    }

    tangents.resize(positions.size());
    bitangents.resize(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const Vec3 n = normalize(normals[i]);
        Vec3 tangent = sub(tangent_accum[i], mul(n, dot(n, tangent_accum[i])));
        if (!(length(tangent) > 1e-20f)) {
            const Vec3 helper = std::fabs(n.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            tangent = cross(helper, n);
        }
        tangent = normalize(tangent);
        const Vec3 cross_nt = normalize(cross(n, tangent));
        const float handedness = dot(cross_nt, bitangent_accum[i]) < 0.0f ? -1.0f : 1.0f;
        tangents[i] = tangent;
        bitangents[i] = mul(cross_nt, handedness);
    }
}

bool build_packed_geometry(const Primitive& p, PackedGeometry& packed, std::string& error, bool skinned) {
    const std::uint32_t vertices = vertex_count(p);
    if (vertices == 0 || vertices > 65536u) {
        error = "current packed static profile requires 1..65536 vertices per primitive";
        return false;
    }
    if (p.indices.empty() || p.indices.size() % 3 != 0) {
        error = "current packed static profile requires a non-empty triangle index stream";
        return false;
    }
    if (std::any_of(p.indices.begin(), p.indices.end(), [vertices](std::uint32_t index) {
            return index >= vertices || index > 65535u;
        })) {
        error = "current packed static profile requires uint16-safe indices";
        return false;
    }

    // Native glTF skins retain node-local source channels; rigid-animation lowering
    // deliberately copies its world-baked rest geometry into the same source channel.
    // Unskinned geometry uses the existing world-baked channels.
    const Primitive local = skinned && !p.source_channels.empty() ? [&]() { Primitive copy = p; copy.channels = p.source_channels; return copy; }() : p;
    const auto* position_channel = find_channel(local, VertexChannel::Semantic::Position);
    const auto* normal_channel = find_channel(local, VertexChannel::Semantic::Normal);
    if (!position_channel || !normal_channel ||
        !finite_channel(*position_channel, vertices, 3) || !finite_channel(*normal_channel, vertices, 3)) {
        error = "current packed static profile requires finite POSITION and NORMAL channels";
        return false;
    }

    std::uint32_t highest_uv_set = 0;
    bool has_uv = false;
    for (const auto& channel : local.channels) {
        if (channel.semantic != VertexChannel::Semantic::Texcoord) continue;
        if (channel.set > kMaxStaticTexcoordSet) {
            error = "current packed static profile supports TEXCOORD_0 through TEXCOORD_6";
            return false;
        }
        highest_uv_set = std::max(highest_uv_set, channel.set);
        has_uv = true;
    }
    const std::uint32_t uv_count = has_uv ? highest_uv_set + 1u : 1u;
    if (skinned && (uv_count < 1u || uv_count > 2u)) {
        error = "skinned MeshGeometry supports exactly one or two TEXCOORD sets";
        return false;
    }
    if (skinned) {
        for (std::uint32_t set = 0; set < 2; ++set) {
            const auto* ji = find_channel(local, VertexChannel::Semantic::BlendIndices, set);
            const auto* wi = find_channel(local, VertexChannel::Semantic::BlendWeights, set);
            if (set == 0 && (!ji || !wi)) { error = "skinned geometry requires JOINTS_0 and WEIGHTS_0"; return false; }
            if (!ji && !wi) continue;
            if (!ji || !wi || !finite_channel(*ji, vertices, 4) || !finite_channel(*wi, vertices, 4)) { error = "skinned JOINTS/WEIGHTS channels have inconsistent or non-finite values"; return false; }
            for (float value : ji->values) if (value < 0.0f || value > 255.0f || std::floor(value) != value) { error = "skinned joint index must be a nonnegative uint8 value"; return false; }
            for (float value : wi->values) if (value < 0.0f || value > 1.0f) { error = "skinned blend weight must be in [0,1]"; return false; }
        }
    }

    std::vector<Vec3> positions(vertices);
    std::vector<Vec3> normals(vertices);
    for (std::uint32_t i = 0; i < vertices; ++i) {
        positions[i] = row3(*position_channel, i);
        normals[i] = normalize(row3(*normal_channel, i));
    }

    std::vector<std::vector<Vec2>> uv_sets(uv_count, std::vector<Vec2>(vertices));
    for (std::uint32_t set = 0; set < uv_count; ++set) {
        const auto* uv_channel = find_channel(local, VertexChannel::Semantic::Texcoord, set);
        if (!uv_channel) continue;
        if (!finite_channel(*uv_channel, vertices, 2)) {
            error = "current packed static TEXCOORD channel has inconsistent or non-finite values";
            return false;
        }
        for (std::uint32_t i = 0; i < vertices; ++i) uv_sets[set][i] = row2(*uv_channel, i);
    }

    std::vector<Vec3> tangents;
    std::vector<Vec3> bitangents;
    const auto* tangent_channel = find_channel(local, VertexChannel::Semantic::Tangent);
    if (tangent_channel && finite_channel(*tangent_channel, vertices, 4)) {
        tangents.resize(vertices);
        bitangents.resize(vertices);
        for (std::uint32_t i = 0; i < vertices; ++i) {
            const auto base = static_cast<std::size_t>(i) * tangent_channel->components;
            Vec3 tangent{tangent_channel->values[base], tangent_channel->values[base + 1], tangent_channel->values[base + 2]};
            tangent = normalize(sub(tangent, mul(normals[i], dot(normals[i], tangent))));
            const float handedness = tangent_channel->values[base + 3] < 0.0f ? -1.0f : 1.0f;
            tangents[i] = tangent;
            bitangents[i] = mul(normalize(cross(normals[i], tangent)), handedness);
        }
    } else {
        tangent_frame(positions, uv_sets[0], normals, p.indices, tangents, bitangents);
    }

    packed = {};
    packed.vertices = vertices;
    const std::uint32_t uv_base = 3u + uv_count;
    packed.streams.resize(skinned ? uv_base + 2u : uv_base);
    packed.strides = {8u, 8u, 4u};
    packed.strides.insert(packed.strides.end(), uv_count, 4u);
    if (skinned) { packed.strides.push_back(8u); packed.strides.push_back(4u); }
    for (std::size_t stream = 0; stream < packed.streams.size(); ++stream) {
        packed.streams[stream].reserve(static_cast<std::size_t>(vertices) * packed.strides[stream]);
    }

    for (std::uint32_t i = 0; i < vertices; ++i) {
        const Vec2 tangent_oct = oct_encode(tangents[i]);
        const Vec2 negative_bitangent_oct = oct_encode(mul(bitangents[i], -1.0f));
        for (float value : {tangent_oct.x, tangent_oct.y, negative_bitangent_oct.x, negative_bitangent_oct.y}) {
            if (!append_half(packed.streams[0], value)) { error = "packed tangent frame exceeds binary16 range"; return false; }
        }
        for (float value : {positions[i].x, positions[i].y, positions[i].z, 1.0f}) {
            if (!append_half(packed.streams[1], value)) { error = "packed position exceeds binary16 range"; return false; }
        }
        const Vec2 normal_oct = oct_encode(normals[i]);
        if (!append_half(packed.streams[2], normal_oct.x) || !append_half(packed.streams[2], normal_oct.y)) {
            error = "packed normal exceeds binary16 range";
            return false;
        }
        for (std::uint32_t set = 0; set < uv_count; ++set) {
            if (!append_half(packed.streams[3u + set], uv_sets[set][i].x) ||
                !append_half(packed.streams[3u + set], uv_sets[set][i].y)) {
                error = "packed TEXCOORD exceeds binary16 range";
                return false;
            }
        }
        if (skinned) {
            std::array<float, 4> weights{};
            std::array<std::uint32_t, 4> joints{};
            std::vector<std::pair<float, std::uint32_t>> influences;
            for (std::uint32_t set = 0; set < 2; ++set) {
                const auto* ji = find_channel(local, VertexChannel::Semantic::BlendIndices, set);
                const auto* wi = find_channel(local, VertexChannel::Semantic::BlendWeights, set);
                if (!ji || !wi || ji->components < 4 || wi->components < 4) continue;
                const auto jb = static_cast<std::size_t>(i) * ji->components;
                const auto wb = static_cast<std::size_t>(i) * wi->components;
                for (std::uint32_t k = 0; k < 4; ++k) if (std::isfinite(wi->values[wb+k]) && wi->values[wb+k] > 0.0f)
                    influences.emplace_back(wi->values[wb+k], static_cast<std::uint32_t>(std::max(0.0f, ji->values[jb+k])));
            }
            if (influences.empty()) { error = "skinned geometry requires JOINTS_0 and WEIGHTS_0"; return false; }
            std::sort(influences.begin(), influences.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
            std::vector<std::pair<float, std::uint32_t>> merged;
            for (const auto& influence : influences) { auto found = std::find_if(merged.begin(), merged.end(), [&](const auto& x) { return x.second == influence.second; }); if (found != merged.end()) found->first += influence.first; else merged.push_back(influence); }
            std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
            const float total = std::accumulate(merged.begin(), merged.begin() + std::min<std::size_t>(4, merged.size()), 0.0f, [](float x, const auto& v) { return x + v.first; });
            for (std::size_t k = 0; k < 4; ++k) { if (k < merged.size() && total > 0) { weights[k] = merged[k].first / total; joints[k] = merged[k].second; } else { weights[k] = k == 0 ? 1.0f : 0.0f; joints[k] = 0; } if (joints[k] > 255u || !append_half(packed.streams[uv_base], weights[k])) { error = "skinned joint palette exceeds UBYTE4 range"; return false; } packed.streams[uv_base + 1].push_back(static_cast<std::uint8_t>(joints[k])); }
        }
    }

    packed.channels = {
        {4u, 17u, 0u, 0u},
        {0u, 17u, 0u, 1u},
        {1u, 15u, 0u, 2u},
    };
    for (std::uint32_t set = 0; set < uv_count; ++set) packed.channels.push_back({5u, 15u, set, 3u + set});
    if (skinned) { packed.channels.push_back({8u, 17u, 0u, uv_base}); packed.channels.push_back({7u, 19u, 0u, uv_base + 1u}); }
    return true;
}

std::array<float, 10> bounds10(const Primitive& p, bool skinned = false) {
    const auto& bounds_min = skinned ? p.source_bounds_min : p.bounds_min;
    const auto& bounds_max = skinned ? p.source_bounds_max : p.bounds_max;
    std::array<float, 10> out{
        bounds_min[0], bounds_min[1], bounds_min[2],
        bounds_max[0], bounds_max[1], bounds_max[2],
        (bounds_min[0] + bounds_max[0]) * 0.5f,
        (bounds_min[1] + bounds_max[1]) * 0.5f,
        (bounds_min[2] + bounds_max[2]) * 0.5f,
        0.0f,
    };
    const Primitive local = skinned && !p.source_channels.empty() ? [&]() { Primitive copy = p; copy.channels = p.source_channels; return copy; }() : p;
    const auto* position = find_channel(local, VertexChannel::Semantic::Position);
    if (!position || position->components < 3) return out;
    float radius2 = 0.0f;
    for (std::size_t i = 0; i + 2 < position->values.size(); i += position->components) {
        const float x = position->values[i] - out[6];
        const float y = position->values[i + 1] - out[7];
        const float z = position->values[i + 2] - out[8];
        radius2 = std::max(radius2, x * x + y * y + z * z);
    }
    out[9] = std::sqrt(radius2);
    return out;
}

bool write_mesh_geometry(BinaryWriter& w, const Primitive& p, const std::string& material_slot, std::string& error, bool skinned) {
    PackedGeometry packed;
    if (!build_packed_geometry(p, packed, error, skinned)) return false;

    w.u32(kMeshGeometryVersion);
    w.u32(static_cast<std::uint32_t>(packed.streams.size()));
    for (std::size_t i = 0; i < packed.streams.size(); ++i) {
        w.blob(packed.streams[i]);
        w.u32(0);
        w.u32(0);
        w.u32(packed.vertices);
        w.u32(packed.strides[i]);
    }

    w.u32(static_cast<std::uint32_t>(packed.channels.size()));
    for (const auto& channel : packed.channels) {
        w.u32(channel.component);
        w.u32(channel.type);
        w.u32(channel.set);
        w.u32(channel.stream);
        w.u8(0);
    }

    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(p.indices.size()));
    std::vector<std::uint8_t> index_bytes(p.indices.size() * 2);
    for (std::size_t i = 0; i < p.indices.size(); ++i) {
        const auto value = static_cast<std::uint16_t>(p.indices[i]);
        index_bytes[i * 2] = static_cast<std::uint8_t>(value & 0xffu);
        index_bytes[i * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    w.blob(index_bytes);

    w.u32(1);
    w.u32(0);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(p.indices.size() / 3));
    w.u32(0);
    for (float value : bounds10(p, skinned)) w.f32(value);

    w.u32(1);
    w.u32(id32_from_id64(material_slot));
    w.u32(0);
    return true;
}

Matrix4 identity_matrix() {
    return Matrix4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}

Matrix4 multiply_matrix(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result[static_cast<std::size_t>(column * 4 + row)] +=
                    a[static_cast<std::size_t>(k * 4 + row)] * b[static_cast<std::size_t>(column * 4 + k)];
            }
        }
    }
    return result;
}

bool decompose_scene_matrix(const Matrix4& matrix,
                            const std::string& label,
                            std::array<float, 9>& rotation,
                            std::array<float, 3>& position,
                            std::array<float, 3>& scale,
                            std::string& error) {
    if (!std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); })) {
        error = label + " has non-finite transform data";
        return false;
    }
    if (std::fabs(matrix[3]) > 1e-6f || std::fabs(matrix[7]) > 1e-6f ||
        std::fabs(matrix[11]) > 1e-6f || std::fabs(matrix[15] - 1.0f) > 1e-6f) {
        error = label + " uses a non-affine transform";
        return false;
    }

    std::array<Vec3, 3> axes{{
        {matrix[0], matrix[1], matrix[2]},
        {matrix[4], matrix[5], matrix[6]},
        {matrix[8], matrix[9], matrix[10]},
    }};
    scale = {length(axes[0]), length(axes[1]), length(axes[2])};
    if (scale[0] <= 1e-12f || scale[1] <= 1e-12f || scale[2] <= 1e-12f) {
        error = label + " has a zero-scale axis; authored UNIT nodes require invertible TRS transforms";
        return false;
    }
    for (std::size_t i = 0; i < axes.size(); ++i) axes[i] = mul(axes[i], 1.0f / scale[i]);
    if (std::max({std::fabs(dot(axes[0], axes[1])), std::fabs(dot(axes[0], axes[2])),
                  std::fabs(dot(axes[1], axes[2]))}) > 2e-5f) {
        error = label + " contains shear; authored UNIT nodes currently preserve ordinary glTF TRS only";
        return false;
    }
    const float determinant = dot(axes[0], cross(axes[1], axes[2]));
    if (std::fabs(std::fabs(determinant) - 1.0f) > 2e-5f) {
        error = label + " rotation basis is not orthonormal";
        return false;
    }
    if (determinant < 0.0f) {
        scale[0] = -scale[0];
        axes[0] = mul(axes[0], -1.0f);
    }
    rotation = {
        axes[0].x, axes[0].y, axes[0].z,
        axes[1].x, axes[1].y, axes[1].z,
        axes[2].x, axes[2].y, axes[2].z,
    };
    position = {matrix[12], matrix[13], matrix[14]};
    return true;
}

bool collect_active_node_order(const Scene& scene,
                               std::vector<int>& order,
                               std::vector<int>& parent_source,
                               std::string& error) {
    std::set<int> included;
    std::function<bool(int)> include_scene_tree = [&](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size()) {
            error = "authored UNIT node index is out of range";
            return false;
        }
        if (!included.insert(index).second) return true;
        for (const int child : scene.nodes[static_cast<std::size_t>(index)].children) {
            if (!include_scene_tree(child)) return false;
        }
        return true;
    };
    for (const int root : scene.scene_roots) {
        if (!include_scene_tree(root)) return false;
    }
    for (const auto& primitive : scene.primitives) {
        if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= scene.nodes.size()) continue;
        const int skin_index = scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin;
        if (skin_index < 0 || static_cast<std::size_t>(skin_index) >= scene.skins.size()) continue;
        for (const int joint : scene.skins[static_cast<std::size_t>(skin_index)].joints) {
            int current = joint;
            std::set<int> ancestry;
            while (current >= 0) {
                if (static_cast<std::size_t>(current) >= scene.nodes.size()) {
                    error = "skin joint ancestry contains an out-of-range node";
                    return false;
                }
                if (!ancestry.insert(current).second) {
                    error = "cycle detected in skin joint ancestry";
                    return false;
                }
                included.insert(current);
                current = scene.nodes[static_cast<std::size_t>(current)].parent;
            }
        }
    }

    parent_source.assign(scene.nodes.size(), -2);
    std::vector<std::uint8_t> state(scene.nodes.size(), 0);
    std::function<bool(int, int)> visit = [&](int index, int parent) {
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size()) {
            error = "authored UNIT node index is out of range";
            return false;
        }
        if (state[static_cast<std::size_t>(index)] == 1) {
            error = "cycle detected in authored UNIT node hierarchy";
            return false;
        }
        if (state[static_cast<std::size_t>(index)] == 2) {
            if (parent_source[static_cast<std::size_t>(index)] != parent) {
                error = "authored UNIT node is referenced by more than one parent";
                return false;
            }
            return true;
        }
        state[static_cast<std::size_t>(index)] = 1;
        parent_source[static_cast<std::size_t>(index)] = parent;
        order.push_back(index);
        const auto& node = scene.nodes[static_cast<std::size_t>(index)];
        for (int child : node.children) {
            if (!included.count(child)) continue;
            if (!visit(child, index)) return false;
        }
        state[static_cast<std::size_t>(index)] = 2;
        return true;
    };
    for (int root : scene.scene_roots) {
        if (!visit(root, -1)) return false;
    }
    for (const int index : included) {
        if (state[static_cast<std::size_t>(index)] != 0) continue;
        const int parent = scene.nodes[static_cast<std::size_t>(index)].parent;
        if (parent < 0 || !included.count(parent)) {
            if (!visit(index, -1)) return false;
        }
    }
    for (const int index : included) {
        if (state[static_cast<std::size_t>(index)] == 0) {
            error = "active skin joint hierarchy has no reachable root";
            return false;
        }
    }
    return true;
}

// The scene node an unskinned mesh renders under, or -1 for the unit root. Like the game's units, a mesh hangs
// under its own node (or the rigid body that owns it) with its geometry in that node's space, so it follows the node
// when it moves: animation, Lua, or LINK_MODE_NODE_NAME onto a parent unit's same-named node (weapon parts on the
// first-person rig). A scene that is just one unparented node keeps its geometry on the root.
int rigid_render_binding(const Scene& scene, std::size_t primitive_index, const std::vector<std::size_t>& primitive_skins) {
    const auto& primitive = scene.primitives[primitive_index];
    if (primitive_skins[primitive_index] != static_cast<std::size_t>(-1)) return -1;
    if (primitive.render_owner_node >= 0) return primitive.render_owner_node;
    const bool has_skinned_source = std::any_of(primitive_skins.begin(), primitive_skins.end(),
        [](std::size_t skin) { return skin != static_cast<std::size_t>(-1); });
    if (scene.source_features.node_count <= 1 && scene.source_features.parent_edge_count == 0 && !has_skinned_source &&
        scene.asset_definition.node_bodies.empty()) return -1;
    if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= scene.nodes.size()) return -1;
    // a zero-scaled node has no space to store the geometry in (it is collapsed in place anyway)
    const auto& m = scene.nodes[static_cast<std::size_t>(primitive.source_node)].world_stingray;
    const double det = static_cast<double>(m[0]) * (static_cast<double>(m[5]) * m[10] - static_cast<double>(m[9]) * m[6]) -
        static_cast<double>(m[4]) * (static_cast<double>(m[1]) * m[10] - static_cast<double>(m[9]) * m[2]) +
        static_cast<double>(m[8]) * (static_cast<double>(m[1]) * m[6] - static_cast<double>(m[5]) * m[2]);
    if (!std::isfinite(det) || det == 0.0) return -1;
    return primitive.source_node;
}

// Geometry moved from unit space into the space of a node whose world may rotate, scale or mirror: positions by
// the inverse world, tangents and binormals by the inverse 3x3, normals by the transposed 3x3, all renormalized;
// tangent handedness flips with a mirroring node; bounds recomputed.
bool localize_to_node(const Primitive& source, const Matrix4& world, Primitive& out, std::string& error) {
    const double a = world[0], b = world[4], c = world[8], d = world[1], e = world[5], f = world[9],
                 g = world[2], h = world[6], k = world[10];
    const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || det == 0.0) {
        error = "mesh node transform is not invertible (zero scale?)";
        return false;
    }
    // inverse 3x3 in row-major r[row][col]
    const double r[3][3] = {{(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det},
                            {(f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det},
                            {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
    const double tx = world[12], ty = world[13], tz = world[14];
    const auto normalize = [](double& x, double& y, double& z) {
        const double length = std::sqrt(x * x + y * y + z * z);
        if (length > 0.0) { x /= length; y /= length; z /= length; }
    };
    out = source;
    const VertexChannel* position = nullptr;
    for (auto& channel : out.channels) {
        const bool point = channel.semantic == VertexChannel::Semantic::Position;
        const bool normal = channel.semantic == VertexChannel::Semantic::Normal;
        const bool direction = channel.semantic == VertexChannel::Semantic::Tangent ||
            channel.semantic == VertexChannel::Semantic::Binormal;
        if (!point && !normal && !direction) continue;
        if (channel.components < 3 || channel.values.size() % channel.components != 0) {
            error = "mesh vector channel has fewer than 3 components";
            return false;
        }
        for (std::size_t i = 0; i < channel.values.size(); i += channel.components) {
            double x = channel.values[i], y = channel.values[i + 1], z = channel.values[i + 2];
            if (point) { x -= tx; y -= ty; z -= tz; }
            double lx, ly, lz;
            if (normal) { // transpose of the world 3x3
                lx = a * x + d * y + g * z; ly = b * x + e * y + h * z; lz = c * x + f * y + k * z;
            } else {
                lx = r[0][0] * x + r[0][1] * y + r[0][2] * z;
                ly = r[1][0] * x + r[1][1] * y + r[1][2] * z;
                lz = r[2][0] * x + r[2][1] * y + r[2][2] * z;
            }
            if (!point) normalize(lx, ly, lz);
            channel.values[i] = static_cast<float>(lx);
            channel.values[i + 1] = static_cast<float>(ly);
            channel.values[i + 2] = static_cast<float>(lz);
            if (channel.semantic == VertexChannel::Semantic::Tangent && channel.components > 3 && det < 0.0)
                channel.values[i + 3] = -channel.values[i + 3];
        }
        if (point) position = &channel;
    }
    if (!position || position->values.empty()) { error = "mesh has no POSITION channel"; return false; }
    out.bounds_min = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
    out.bounds_max = {-out.bounds_min[0], -out.bounds_min[1], -out.bounds_min[2]};
    for (std::size_t i = 0; i < position->values.size(); i += position->components)
        for (int axis = 0; axis < 3; ++axis) {
            out.bounds_min[axis] = std::min(out.bounds_min[axis], position->values[i + axis]);
            out.bounds_max[axis] = std::max(out.bounds_max[axis], position->values[i + axis]);
        }
    const std::array<float, 3> center{(out.bounds_min[0] + out.bounds_max[0]) * 0.5f,
                                      (out.bounds_min[1] + out.bounds_max[1]) * 0.5f,
                                      (out.bounds_min[2] + out.bounds_max[2]) * 0.5f};
    double radius2 = 0;
    for (std::size_t i = 0; i < position->values.size(); i += position->components) {
        const double dx = position->values[i] - center[0], dy = position->values[i + 1] - center[1],
                     dz = position->values[i + 2] - center[2];
        radius2 = std::max(radius2, dx * dx + dy * dy + dz * dz);
    }
    out.bounds_radius = static_cast<float>(std::sqrt(radius2));
    if (!std::isfinite(out.bounds_radius)) { error = "mesh bounds are non-finite"; return false; }
    return true;
}

bool build_scene_graph(const Scene& scene,
                       std::uint32_t root_name_hash,
                       const std::vector<std::uint32_t>& mesh_name_hashes,
                       const std::map<int, std::uint32_t>& joint_name_hashes,
                       const std::vector<std::size_t>& primitive_skins,
                       std::vector<SceneGraphNode>& nodes,
                       std::vector<std::uint32_t>& source_node_refs,
                       std::vector<std::uint32_t>& renderer_node_refs,
                       std::string& error) {
    const auto identity = identity_matrix();
    const std::array<float, 9> identity_rotation{1,0,0, 0,1,0, 0,0,1};
    const std::array<float, 3> zero{0,0,0};
    const std::array<float, 3> one{1,1,1};
    nodes.clear();
    nodes.push_back({identity_rotation, zero, one, identity, 0, 0, root_name_hash});
    source_node_refs.assign(scene.nodes.size(), 0);
    renderer_node_refs.assign(scene.primitives.size(), 0);

    std::set<std::uint32_t> hashes{root_name_hash};
    for (const auto mesh_name_hash : mesh_name_hashes) {
        if (!hashes.insert(mesh_name_hash).second) {
            error = "static renderer mesh name hash collides with another UNIT node";
            return false;
        }
    }

    const auto append_renderer_nodes = [&]() {
        // MeshObjects and their renderer nodes have a strict 1:1 identity pairing.
        // Put these nodes after the authored hierarchy and parent them to the source
        // node only when geometry is stored in that node's local space.
        if (nodes.size() + scene.primitives.size() > kMaxSceneNodes) {
            error = "UNIT scene graph exceeds the 16-bit node limit";
            return false;
        }
        for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
            const auto& primitive = scene.primitives[i];
            const bool skinned = primitive_skins[i] != static_cast<std::size_t>(-1);
            const bool world_baked_rigid_skin = skinned && primitive_skins[i] < scene.skins.size() &&
                scene.skins[primitive_skins[i]].name == "__rigid_animation_skin";
            // Rigid animation lowering stores rest-pose world vertices and uses
            // SkinDT's inverse-bind palette to apply node motion. Parenting its
            // renderer beneath the same source node would apply that motion twice.
            const int binding = world_baked_rigid_skin ? -1 :
                (skinned ? primitive.source_node : rigid_render_binding(scene, i, primitive_skins));
            std::uint16_t parent_index = 0;
            if (binding >= 0) {
                if (static_cast<std::size_t>(binding) >= source_node_refs.size() ||
                    source_node_refs[static_cast<std::size_t>(binding)] == 0) {
                    error = "renderer node binding is not present in the active authored SceneGraph";
                    return false;
                }
                const auto ref = source_node_refs[static_cast<std::size_t>(binding)];
                if (ref > 65535u) {
                    error = "renderer node parent exceeds the 16-bit node limit";
                    return false;
                }
                parent_index = static_cast<std::uint16_t>(ref);
            }
            nodes.push_back({identity_rotation, zero, one, nodes[parent_index].world, 1, parent_index, mesh_name_hashes[i]});
            renderer_node_refs[i] = static_cast<std::uint32_t>(nodes.size() - 1u);
        }
        return true;
    };

    const bool has_skinned_source = std::any_of(primitive_skins.begin(), primitive_skins.end(),
        [](std::size_t skin) { return skin != static_cast<std::size_t>(-1); });
    if (scene.source_features.node_count <= 1 && scene.source_features.parent_edge_count == 0 && !has_skinned_source && scene.asset_definition.node_bodies.empty()) return append_renderer_nodes();

    std::vector<int> order;
    std::vector<int> parent_source;
    if (!collect_active_node_order(scene, order, parent_source, error)) return false;
    const std::size_t reference_base = nodes.size();
    if (reference_base + order.size() + scene.primitives.size() > kMaxSceneNodes) {
        error = "authored UNIT scene graph exceeds the 16-bit node limit";
        return false;
    }

    std::vector<int> order_index(scene.nodes.size(), -1);
    for (std::size_t i = 0; i < order.size(); ++i) order_index[static_cast<std::size_t>(order[i])] = static_cast<int>(i);

    std::vector<std::string> authored_names;
    std::vector<std::size_t> stable_indices;
    authored_names.reserve(order.size());
    stable_indices.reserve(order.size());
    for (const int source_index : order) {
        if (!joint_name_hashes.count(source_index)) {
            authored_names.push_back(scene.nodes[static_cast<std::size_t>(source_index)].name);
            stable_indices.push_back(static_cast<std::size_t>(source_index));
        }
    }
    auto reserved_hashes = mesh_name_hashes;
    reserved_hashes.push_back(root_name_hash);
    for (const auto& [source_index, joint_hash] : joint_name_hashes) {
        (void)source_index;
        reserved_hashes.push_back(joint_hash);
    }
    const auto native_names = lower_unique_native_names(authored_names, stable_indices, "node", "node_", reserved_hashes);

    std::size_t authored_position = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int source_index = order[i];
        const auto& source = scene.nodes[static_cast<std::size_t>(source_index)];
        if (source.name.find('\0') != std::string::npos) {
            error = "authored UNIT scene node names must be NUL-free";
            return false;
        }
        const auto joint = joint_name_hashes.find(source_index);
        const std::uint32_t name_hash = joint != joint_name_hashes.end()
            ? joint->second : name_id32(native_names[authored_position++]);
        if (!hashes.insert(name_hash).second) {
            error = "lowered UNIT node hash collides with another scene node: " + source.name;
            return false;
        }

        SceneGraphNode node;
        if (!decompose_scene_matrix(source.local_stingray, "GLB node '" + source.name + "'",
                                    node.rotation, node.position, node.scale, error)) return false;
        const int parent = parent_source[static_cast<std::size_t>(source_index)];
        if (parent < 0) {
            node.parent_type = 1;
            node.parent_index = 0;
            node.world = source.local_stingray;
        } else {
            const int parent_order = order_index[static_cast<std::size_t>(parent)];
            if (parent_order < 0 || static_cast<std::size_t>(parent_order) >= i) {
                error = "authored UNIT node parent must precede its child";
                return false;
            }
            const std::size_t parent_index = reference_base + static_cast<std::size_t>(parent_order);
            if (parent_index > 65535u) {
                error = "authored UNIT node parent exceeds the 16-bit node limit";
                return false;
            }
            node.parent_type = 1;
            node.parent_index = static_cast<std::uint16_t>(parent_index);
            node.world = multiply_matrix(nodes[parent_index].world, source.local_stingray);
        }
        if (!std::all_of(node.world.begin(), node.world.end(), [](float value) { return std::isfinite(value); })) {
            error = "authored UNIT scene node world transform is non-finite: " + source.name;
            return false;
        }
        node.name_hash = name_hash;
        nodes.push_back(node);
        source_node_refs[static_cast<std::size_t>(source_index)] = static_cast<std::uint32_t>(nodes.size() - 1u);
    }

    if (!append_renderer_nodes()) return false;
    if (has_skinned_source) {
        for (std::size_t i = 0; i < primitive_skins.size(); ++i) {
            if (primitive_skins[i] == static_cast<std::size_t>(-1)) continue;
            const int source_node = scene.primitives[i].source_node;
            if (source_node < 0 || static_cast<std::size_t>(source_node) >= source_node_refs.size() ||
                source_node_refs[static_cast<std::size_t>(source_node)] == 0) {
                error = "skinned primitive source node is not present in the active authored SceneGraph";
                return false;
            }
        }
    }
    return true;
}

bool write_scene_graph(BinaryWriter& w,
                       const Scene& scene,
                       std::uint32_t root_name_hash,
                       const std::vector<std::uint32_t>& mesh_name_hashes,
                       const std::map<int, std::uint32_t>& joint_name_hashes,
                       const std::vector<std::size_t>& primitive_skins,
                       std::vector<std::uint32_t>& source_node_refs,
                       std::vector<std::uint32_t>& renderer_node_refs,
                       std::vector<std::uint32_t>& source_node_hashes,
                       std::string& error) {
    std::vector<SceneGraphNode> nodes;
    if (!build_scene_graph(scene, root_name_hash, mesh_name_hashes, joint_name_hashes, primitive_skins, nodes, source_node_refs, renderer_node_refs, error)) return false;
    source_node_hashes.assign(source_node_refs.size(), 0);
    for (std::size_t i = 0; i < source_node_refs.size(); ++i)
        if (source_node_refs[i]) source_node_hashes[i] = nodes[source_node_refs[i]].name_hash;
    w.u32(static_cast<std::uint32_t>(nodes.size()));
    for (const auto& node : nodes) {
        for (float value : node.rotation) w.f32(value);
        for (float value : node.position) w.f32(value);
        for (float value : node.scale) w.f32(value);
    }
    for (const auto& node : nodes) for (float value : node.world) w.f32(value);
    for (const auto& node : nodes) { w.u16(node.parent_type); w.u16(node.parent_index); }
    for (const auto& node : nodes) w.u32(node.name_hash);
    w.u32(0);
    return true;
}

void write_simple_mesh_object(BinaryWriter& w,
                              const Primitive& p,
                              std::uint32_t mesh_name_hash,
                              std::uint32_t node_ref,
                              std::uint32_t geometry_ref,
                              std::uint32_t render_flags,
                              std::uint32_t skin_ref = 0,
                              bool skinned = false) {
    w.u32(mesh_name_hash);
    w.u32(node_ref);
    w.u32(geometry_ref);
    w.u32(skin_ref);
    w.u32(render_flags);
    w.u32(kMeshObjectKind);
    w.u32(kMeshObjectEnabled);
    for (float value : bounds10(p, skinned)) w.f32(value);
    w.u32(0);
}

bool primitive_skin(const Scene& scene, const Primitive& p, std::size_t& skin_index) {
    if (p.source_node < 0 || static_cast<std::size_t>(p.source_node) >= scene.nodes.size()) return false;
    const int skin = scene.nodes[static_cast<std::size_t>(p.source_node)].skin;
    if (skin < 0 || static_cast<std::size_t>(skin) >= scene.skins.size()) return false;
    skin_index = static_cast<std::size_t>(skin);
    return true;
}

bool skin_node_indices(const Scene& scene, std::size_t skin_index, std::vector<std::uint32_t>& nodes, std::string& error) {
    std::vector<int> order, parents;
    if (!collect_active_node_order(scene, order, parents, error)) return false;
    const auto& skin = scene.skins[skin_index];
    nodes.clear();
    for (int joint : skin.joints) {
        auto it = std::find(order.begin(), order.end(), joint);
        if (it == order.end()) { error = "SkinDT joint is not present in the active scene graph"; return false; }
        nodes.push_back(static_cast<std::uint32_t>(1u + (it - order.begin())));
    }
    return true;
}

std::pair<std::uint32_t, std::uint64_t> resolve_material(const std::string& slot, const WriteOptions& options) {
    std::string resource = options.material_override;
    for (const auto& [binding_slot, binding_resource] : options.material_bindings) {
        if (binding_slot == slot) {
            resource = binding_resource;
            break;
        }
    }
    return {id32_from_id64(slot), resource.empty() ? 0u : resource_name_hash(resource)};
}

bool collect_material_bindings(const Scene& scene,
                               const WriteOptions& options,
                               const std::vector<std::string>& material_slots,
                               std::vector<std::pair<std::uint32_t, std::uint64_t>>& bindings,
                               std::string& error) {
    bindings.clear();
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        const auto resolved = resolve_material(material_slots[i], options);
        const auto existing = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.first == resolved.first;
        });
        if (existing == bindings.end()) {
            bindings.push_back(resolved);
        } else if (existing->second != resolved.second) {
            error = "one material slot resolves to multiple resource identities";
            return false;
        }
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return !bindings.empty();
}

// One UNIT light record per glTF punctual light, bound to the light's own
// SceneGraph node (its local +Y is glTF's -Z, the Stingray spot axis).
// Field order follows the engine serializer FUN_1402b3cd0; the trailing block
// is not exposed to Lua and repeats the constants every retail omni/spot light uses.
bool write_light_records(const Scene& scene, const std::vector<std::uint32_t>& source_node_refs,
                         BinaryWriter& w, std::uint32_t& count, std::string& error) {
    count = 0;
    for (std::size_t node_index = 0; node_index < scene.nodes.size(); ++node_index) {
        const auto& node = scene.nodes[node_index];
        if (node.light < 0) continue;
        if (static_cast<std::size_t>(node.light) >= scene.lights.size()) { error = "node references a missing light"; return false; }
        const auto& light = scene.lights[static_cast<std::size_t>(node.light)];
        if (light.type == LightInfo::Type::Directional) continue; // retail units never carry directional lights
        if (node_index >= source_node_refs.size() || source_node_refs[node_index] == 0) {
            error = "light node is not present in the UNIT SceneGraph: " + node.name;
            return false;
        }
        const bool spot = light.type == LightInfo::Type::Spot;
        const float values[] = {light.color[0], light.color[1], light.color[2], light.intensity,
                                light.range, light.inner_cone_angle, light.outer_cone_angle};
        if (!std::all_of(std::begin(values), std::end(values), [](float v) { return std::isfinite(v) && v >= 0.0f; })) {
            error = "light color, intensity, range and cone angles must be finite and nonnegative: " + node.name;
            return false;
        }
        const std::string& name = node.name.empty() ? light.name : node.name;
        w.u32(id32_from_id64(name));
        w.u32(source_node_refs[node_index]);
        for (float c : light.color) w.f32(c);
        w.f32(6500.0f);                                   // correlated color temperature (neutral)
        // Blender exports W * 683 / 4pi candela. Retail lamps are 15..600 (engine default 600),
        // far below physical lumens, so map Blender watts * 60: a new 10 W lamp -> 600.
        w.f32(light.intensity * (12.5663706f / 683.0f) * 60.0f);
        w.f32(1.0f);                                      // volumetric intensity
        w.f32(1.0f);                                      // particle intensity
        w.f32(light.range > 0.0f ? light.range : 8.0f);   // falloff end (engine default 8)
        w.f32(0.0f);                                      // falloff start
        w.f32(spot ? 2.0f * light.inner_cone_angle : 0.0f); // spot angle start (full cone)
        w.f32(spot ? 2.0f * light.outer_cone_angle : 3.14159274f); // spot angle end (full cone)
        w.f32(0.0f); w.f32(0.0f);                         // box max y/z
        // flags (Lua Light.*): 1 casts shadows, 2 disabled, 4 baked, 8 dynamic, 0x40 force static
        const auto& shadows = scene.asset_definition.shadow_lights;
        w.u32(std::find(shadows.begin(), shadows.end(), static_cast<int>(node_index)) != shadows.end() ? 1u : 0u);
        w.u32(spot ? 1u : 0u);                            // type: omni / spot
        w.u64(0);                                         // material
        w.u64(0);                                         // IES profile
        w.u32(128);                                       // max shadow resolution
        w.f32(0.8f);
        w.f32(0.2f); w.f32(0.2f); w.f32(0.2f);
        w.u32(144);
        w.f32(0.97f); w.f32(100.0f); w.f32(0.2f); w.f32(1.0f);
        w.u32(1);
        w.f32(10.0f); w.f32(10.0f); w.f32(10.0f); w.f32(-10.0f);
        w.u32(0);
        w.f32(-10.0f);
        w.u32(0); w.u32(0);
        ++count;
    }
    return true;
}

// UNIT visibility groups: IdString32 name + MeshObject indices. A group node
// claims every primitive owned by that node or one of its descendants.
void write_visibility_groups(const Scene& scene, BinaryWriter& w) {
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> groups;
    const auto under = [&](int node, int ancestor) {
        for (int guard = 0; node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size() && guard < 65536; ++guard) {
            if (node == ancestor) return true;
            node = scene.nodes[static_cast<std::size_t>(node)].parent;
        }
        return false;
    };
    for (const auto& member : scene.asset_definition.visibility_groups) {
        auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == member.group; });
        if (group == groups.end()) group = groups.insert(groups.end(), {member.group, {}});
        for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
            const auto& primitive = scene.primitives[i];
            if ((under(primitive.render_owner_node, member.source_node) || under(primitive.source_node, member.source_node)) &&
                std::find(group->second.begin(), group->second.end(), static_cast<std::uint32_t>(i)) == group->second.end())
                group->second.push_back(static_cast<std::uint32_t>(i));
        }
    }
    w.u32(static_cast<std::uint32_t>(groups.size()));
    for (const auto& [name, meshes] : groups) {
        w.u32(id32_from_id64(name));
        w.u32(static_cast<std::uint32_t>(meshes.size()));
        for (const auto mesh : meshes) w.u32(mesh);
    }
}

// MeshObject render flags as the game writes them (Stingray: 0x1 viewport visible, 0x2 shadow caster; Darktide
// adds 0xc0000 with visibility and 0x2000): 0xc2003 visible + shadow, 0xc2001 visible only, 0x2002 shadow only.
// The nearest render setting on the mesh's node or above it decides.
std::uint32_t mesh_render_flags_for(const Scene& scene, const Primitive& primitive) {
    const auto& settings = scene.asset_definition.render_settings;
    for (int node : {primitive.source_node, primitive.render_owner_node}) {
        for (int guard = 0; node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size() && guard < 65536; ++guard) {
            for (const auto& setting : settings)
                if (setting.source_node == node)
                    return setting.visible ? (setting.shadow ? 0x000c2003u : 0x000c2001u) : 0x00002002u;
            node = scene.nodes[static_cast<std::size_t>(node)].parent;
        }
    }
    return kSimpleStaticRenderFlags;
}

// LOD objects: each group's levels in level order, a level owning the meshes of its node and
// the nodes below it (the nearest level node wins).
struct LodObject {
    std::string group;
    std::vector<const LodLevel*> levels;
    std::vector<std::vector<std::uint32_t>> meshes;
};

bool plan_lod_objects(const Scene& scene, std::vector<LodObject>& objects, std::vector<bool>& in_lod, std::string& error) {
    const auto& levels = scene.asset_definition.lod_levels;
    in_lod.assign(scene.primitives.size(), false);
    if (levels.empty()) return true;
    const auto level_of = [&](int node) -> const LodLevel* {
        for (int guard = 0; node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size() && guard < 65536; ++guard) {
            for (const auto& level : levels) if (level.source_node == node) return &level;
            node = scene.nodes[static_cast<std::size_t>(node)].parent;
        }
        return nullptr;
    };
    for (const auto& level : levels) {
        auto object = std::find_if(objects.begin(), objects.end(), [&](const LodObject& o) { return o.group == level.group; });
        if (object == objects.end()) object = objects.insert(objects.end(), {level.group, {}, {}});
        for (const auto* other : object->levels)
            if (other->level == level.level) { error = "LOD group '" + level.group + "' has level " + std::to_string(level.level) + " twice"; return false; }
        object->levels.push_back(&level);
    }
    for (auto& object : objects) {
        std::sort(object.levels.begin(), object.levels.end(), [](const LodLevel* a, const LodLevel* b) { return a->level < b->level; });
        object.meshes.resize(object.levels.size());
        for (std::size_t k = 1; k < object.levels.size(); ++k)
            if (!(object.levels[k]->down_to < object.levels[k - 1]->down_to)) {
                error = "LOD group '" + object.group + "': each level must switch at a smaller screen height than the one before";
                return false;
            }
    }
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        const LodLevel* level = level_of(scene.primitives[i].source_node);
        if (!level) level = level_of(scene.primitives[i].render_owner_node);
        if (!level) continue;
        for (auto& object : objects)
            for (std::size_t k = 0; k < object.levels.size(); ++k)
                if (object.levels[k] == level) object.meshes[k].push_back(static_cast<std::uint32_t>(i));
        in_lod[i] = true;
    }
    for (const auto& object : objects)
        for (std::size_t k = 0; k < object.levels.size(); ++k)
            if (object.meshes[k].empty()) {
                error = "LOD group '" + object.group + "' level " + std::to_string(object.levels[k]->level) + " has no meshes";
                return false;
            }
    return true;
}

// UNIT LOD object records (non-streamed, like the game's units without a geometry stream):
// name, the unit's source file id, orientation node, steps {visible height range, meshes, stream
// offset 0, stream index 0}, bounding volume, flags, kLodObjectX, empty stream order, 0, not streamed.
// As in Stingray's compiler the bounding volume is the first mesh's of the top level, the object
// turns with that mesh's node, and the flags are its meshes' render flags without the LOD bit (0x8)
// and the streamed bit (0x10000).
void write_lod_objects(BinaryWriter& w, const std::vector<LodObject>& objects, const std::string& resource_name,
                       const std::vector<Primitive>& geometry, const std::vector<std::uint32_t>& renderer_node_refs,
                       const std::vector<std::uint32_t>& render_flags, const std::vector<bool>& skinned) {
    w.u32(static_cast<std::uint32_t>(objects.size()));
    for (const auto& object : objects) {
        const auto bounds_mesh = object.meshes.front().front();
        w.u32(id32_from_id64(object.group));
        w.u64(resource_name.empty() ? 0u : resource_name_hash(resource_name + ".unit"));
        w.u32(renderer_node_refs[bounds_mesh]);
        w.u32(static_cast<std::uint32_t>(object.levels.size()));
        std::uint32_t flags = 0;
        for (std::size_t k = 0; k < object.levels.size(); ++k) {
            w.f32(k == 0 ? std::numeric_limits<float>::max() : object.levels[k - 1]->down_to);
            w.f32(object.levels[k]->down_to);
            w.u32(static_cast<std::uint32_t>(object.meshes[k].size()));
            for (const auto mesh : object.meshes[k]) { w.u32(mesh); flags |= render_flags[mesh]; }
            w.u32(0);
            w.u32(0);
        }
        for (float value : bounds10(geometry[bounds_mesh], skinned[bounds_mesh])) w.f32(value);
        w.u32(flags & ~0x10008u);
        w.u32(kLodObjectX);
        w.u32(0);
        w.u32(0);
        w.u8(0);
    }
}

bool write_static_tail(BinaryWriter& w, const Scene& scene, const WriteOptions& options,
                       const std::vector<std::string>& material_slots,
                       const std::vector<std::vector<std::uint8_t>>& actor_records,
                       const std::vector<std::uint8_t>& physics_scene,
                       const std::vector<std::uint32_t>& source_node_refs,
                       const std::vector<std::uint8_t>& lod_objects,
                       const std::vector<std::uint8_t>& flow,
                       const std::vector<std::uint8_t>& flow_dynamic_data,
                       std::string& error) {
    w.u32(static_cast<std::uint32_t>(actor_records.size()));
    for (const auto& record : actor_records) w.bytes(record.data(), record.size());
    // actors_2, u32 list, cameras, then lights.
    for (int i = 0; i < 3; ++i) w.u32(0);
    BinaryWriter lights;
    std::uint32_t light_count = 0;
    if (!write_light_records(scene, source_node_refs, lights, light_count, error)) return false;
    w.u32(light_count);
    w.bytes(lights.data().data(), lights.data().size());
    // u64 list, LOD objects, terrains, unused, joints, movers, unused.
    w.u32(0);
    w.bytes(lod_objects.data(), lod_objects.size());
    for (int i = 0; i < 5; ++i) w.u32(0);
    // Unit resource +0x2b0: the engine only creates the animation blender and
    // instances the state machine named below when this is set (all retail
    // units that reference a state machine set it).
    w.u8(options.animation_state_machine_resource.empty() ? 0 : 1);
    w.u32(static_cast<std::uint32_t>(options.animation_state_machine_resource.size()));
    if (!options.animation_state_machine_resource.empty())
        w.bytes(options.animation_state_machine_resource.data(), options.animation_state_machine_resource.size());

    // script data (Unit.get_data); empty encodes as {0xffffffff, 0} like the game's units without data
    std::vector<std::uint8_t> dynamic_data;
    json::Value data = json::Value::object();
    if (!scene.asset_definition.script_data.empty() && !json::parse(scene.asset_definition.script_data, data, error)) {
        error = "unit data: " + error;
        return false;
    }
    if (!encode_script_data(data, dynamic_data, error)) return false;
    w.u32(static_cast<std::uint32_t>(dynamic_data.size()));
    w.bytes(dynamic_data.data(), dynamic_data.size());
    write_visibility_groups(scene, w);
    w.u32(static_cast<std::uint32_t>(flow.size()));
    w.bytes(flow.data(), flow.size());
    w.u32(static_cast<std::uint32_t>(flow_dynamic_data.size()));
    w.bytes(flow_dynamic_data.data(), flow_dynamic_data.size());

    static constexpr std::array<std::uint8_t, 4> pre_physics{0,0,0,0};
    w.u32(static_cast<std::uint32_t>(pre_physics.size()));
    w.bytes(pre_physics.data(), pre_physics.size());
    w.blob(physics_scene);
    w.u64(0);

    std::vector<std::pair<std::uint32_t, std::uint64_t>> bindings;
    if (!collect_material_bindings(scene, options, material_slots, bindings, error)) {
        if (error.empty()) error = "static UNIT requires at least one material binding";
        return false;
    }
    w.u32(static_cast<std::uint32_t>(bindings.size()));
    for (const auto& [slot, resource] : bindings) {
        w.u32(slot);
        w.u64(resource);
    }

    w.u64(0);
    w.u64(0);
    w.u64(options.skeleton_resource.empty() ? 0u : resource_name_hash(options.skeleton_resource));
    w.u32(0);
    return true;
}

bool save_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "cannot create UNIT output directory: " + ec.message(); return false; }
    std::ofstream file(path, std::ios::binary);
    if (!file) { error = "cannot open UNIT output"; return false; }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) { error = "cannot write UNIT output"; return false; }
    return true;
}

} // namespace

bool build_unit_v115(const Scene& scene, UnitResource& out, std::string& error, const WriteOptions& options) {
    out = {};
    if (scene.primitives.empty()) {
        error = "current static UNIT v115 profile requires at least one mesh primitive";
        return false;
    }
    for (const auto& primitive : scene.primitives) {
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) >= scene.materials.size()) {
            error = "UNIT primitive material index is out of range";
            return false;
        }
        if (vertex_count(primitive) == 0 || primitive.indices.empty()) {
            error = "current static UNIT v115 profile requires non-empty indexed geometry in every primitive";
            return false;
        }
    }

    std::vector<std::size_t> primitive_skins(scene.primitives.size(), static_cast<std::size_t>(-1));
    std::vector<std::size_t> used_skins;
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        std::size_t skin = 0;
        if (primitive_skin(scene, scene.primitives[i], skin)) {
            primitive_skins[i] = skin;
            if (std::find(used_skins.begin(), used_skins.end(), skin) == used_skins.end()) used_skins.push_back(skin);
        }
    }

    if (used_skins.size() > 1) {
        error = "UNIT skeleton identity mapping currently requires a single used skin";
        return false;
    }
    std::map<int, std::uint32_t> joint_name_hashes;
    std::map<std::uint32_t, int> hash_joint_nodes;
    for (const auto skin_index : used_skins) {
        const auto& skin = scene.skins[skin_index];
        const auto canonical_names = bones::canonical_bone_names(skin);
        if (canonical_names.size() != skin.joints.size()) {
            error = "SkinDT canonical bone identity count does not match joints";
            return false;
        }
        std::set<int> skin_joints;
        for (std::size_t i = 0; i < skin.joints.size(); ++i) {
            const int source_index = skin.joints[i];
            if (source_index < 0 || static_cast<std::size_t>(source_index) >= scene.nodes.size()) {
                error = "SkinDT joint index is out of range";
                return false;
            }
            if (!skin_joints.insert(source_index).second) {
                error = "SkinDT skin contains a duplicate joint";
                return false;
            }
            const std::uint32_t joint_hash = id32_from_id64(canonical_names[i]);
            const auto existing = joint_name_hashes.find(source_index);
            if (existing != joint_name_hashes.end() && existing->second != joint_hash) {
                error = "shared SkinDT joint has conflicting canonical identities";
                return false;
            }
            const auto hash_existing = hash_joint_nodes.find(joint_hash);
            if (hash_existing != hash_joint_nodes.end() && hash_existing->second != source_index) {
                error = "SkinDT canonical bone hashes collide across different joints";
                return false;
            }
            joint_name_hashes[source_index] = joint_hash;
            hash_joint_nodes[joint_hash] = source_index;
        }
    }

    std::vector<std::string> mesh_names;
    std::vector<std::uint32_t> mesh_name_hashes;
    mesh_names.reserve(scene.primitives.size());
    mesh_name_hashes.reserve(scene.primitives.size());
    std::vector<std::string> authored_mesh_names;
    std::vector<std::size_t> mesh_indices;
    authored_mesh_names.reserve(scene.primitives.size());
    mesh_indices.reserve(scene.primitives.size());
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        authored_mesh_names.push_back(scene.primitives[i].name);
        mesh_indices.push_back(i);
    }

    std::vector<std::string> material_slots = options.primitive_material_slots;
    if (material_slots.empty()) {
        std::vector<int> material_indices;
        material_indices.reserve(scene.primitives.size());
        for (const auto& primitive : scene.primitives) material_indices.push_back(primitive.material);
        material_slots = lower_material_slots([&] {
            std::vector<std::string> names;
            names.reserve(scene.materials.size());
            for (const auto& material : scene.materials) names.push_back(material.name);
            return names;
        }(), material_indices).primitive_slots;
    }
    if (material_slots.size() != scene.primitives.size()) {
        error = "UNIT primitive material slot count does not match primitive count";
        return false;
    }
    std::map<int, std::string> material_to_slot;
    std::map<std::uint32_t, int> slot_to_material;
    for (std::size_t i = 0; i < material_slots.size(); ++i) {
        const int material = scene.primitives[i].material < 0 ? -1 : scene.primitives[i].material;
        const auto& slot = material_slots[i];
        if (slot.empty()) { error = "UNIT material slot cannot be empty"; return false; }
        const auto owner = slot_to_material.emplace(id32_from_id64(slot), material);
        const auto binding = material_to_slot.emplace(material, slot);
        if ((!owner.second && owner.first->second != material) ||
            (!binding.second && binding.first->second != slot)) {
            error = "UNIT material slots must preserve distinct source material identities";
            return false;
        }
    }
    std::map<std::string, std::string> explicit_bindings;
    for (const auto& binding : options.material_bindings) {
        if (std::find(material_slots.begin(), material_slots.end(), binding.first) == material_slots.end()) {
            error = "UNIT material binding must name a lowered active slot";
            return false;
        }
        const auto inserted = explicit_bindings.emplace(binding.first, binding.second);
        if (!inserted.second && inserted.first->second != binding.second) {
            error = "UNIT material slot has conflicting explicit bindings";
            return false;
        }
    }
    std::vector<std::uint32_t> reserved_joint_hashes;
    reserved_joint_hashes.reserve(joint_name_hashes.size());
    for (const auto& [source_index, joint_hash] : joint_name_hashes) {
        (void)source_index;
        reserved_joint_hashes.push_back(joint_hash);
    }
    mesh_names = lower_unique_native_names(authored_mesh_names, mesh_indices, "mesh", "mesh_", reserved_joint_hashes);
    for (const auto& name : mesh_names) mesh_name_hashes.push_back(name_id32(name));

    std::set<std::uint32_t> mesh_hash_set(mesh_name_hashes.begin(), mesh_name_hashes.end());
    mesh_hash_set.insert(reserved_joint_hashes.begin(), reserved_joint_hashes.end());
    // Retail units root at root_point; gear linking and Unit.node lookups expect it.
    // (used to be __glb_static_root__, worked but looked nothing like the real thing)
    std::string root_text = "root_point";
    while (mesh_hash_set.count(id32_from_id64(root_text))) root_text += "_node";
    const std::uint32_t root_name_hash = id32_from_id64(root_text);

    std::vector<std::uint8_t> primary_actor;
    if (options.fitted_physics) {
        physics::PhysicsActor actor;
        if (!physics::build_actor(scene.primitives, "__glb_physics_actor__",
                                                   root_text, root_name_hash,
                                                   *options.fitted_physics, actor, error) ||
            !physics::serialize_actor(actor, primary_actor, error)) return false;
    } else if (options.authored_physics && scene.asset_definition.body) {
        physics::PhysicsActor actor;
        if (!physics::build_authored_actor(scene, root_text, root_name_hash, actor, error) ||
            !physics::serialize_actor(actor, primary_actor, error)) return false;
    }

    BinaryWriter body;
    // Unskinned MeshObjects use the vertices and bounds of the node they render under (rigid_render_binding).
    // Keep the original world-space geometry intact for collider construction.
    std::vector<Primitive> owned_geometry = scene.primitives;
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        const int owner = scene.primitives[i].render_owner_node;
        if (owner >= 0 && primitive_skins[i] != static_cast<std::size_t>(-1)) {
            const auto skin_index = primitive_skins[i];
            // This skin already deforms rest-world vertices through its inverse-bind palette; the renderer has no
            // body parent.
            if (skin_index < scene.skins.size() && scene.skins[skin_index].name == "__rigid_animation_skin")
                continue;
            error = "body-owned render geometry requires a rigid source node";
            return false;
        }
        const int binding = rigid_render_binding(scene, i, primitive_skins);
        if (binding < 0) continue;
        if (static_cast<std::size_t>(binding) >= scene.nodes.size()) {
            error = "render geometry is bound to a missing node";
            return false;
        }
        if (!localize_to_node(scene.primitives[i], scene.nodes[binding].world_stingray, owned_geometry[i], error))
            return false;
    }
    const auto& render_geometry = owned_geometry.empty() ? scene.primitives : owned_geometry;
    body.u32(kUnitVersion);
    body.u32(static_cast<std::uint32_t>(scene.primitives.size()));
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        if (!write_mesh_geometry(body, render_geometry[i], material_slots[i], error, primitive_skins[i] != static_cast<std::size_t>(-1))) return false;
    }
    body.u32(static_cast<std::uint32_t>(used_skins.size()));
    for (const auto skin_index : used_skins) {
        const auto& skin = scene.skins[skin_index];
        const auto& matrices = skin.inverse_bind_matrices_stingray.empty() ? skin.inverse_bind_matrices_gltf : skin.inverse_bind_matrices_stingray;
        if (matrices.empty() || matrices.size() != skin.joints.size() || matrices.size() > 256u) { error = "SkinDT requires 1..256 inverse-bind matrices matching joints"; return false; }
        if (!std::all_of(matrices.begin(), matrices.end(), [](const Matrix4& matrix) { return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); }); })) { error = "SkinDT inverse-bind matrices must be finite"; return false; }
        std::vector<std::uint32_t> node_indices;
        if (!skin_node_indices(scene, skin_index, node_indices, error)) return false;
        body.u32(static_cast<std::uint32_t>(matrices.size()));
        for (const auto& matrix : matrices) for (float value : matrix) body.f32(value);
        body.u32(static_cast<std::uint32_t>(node_indices.size()));
        for (auto value : node_indices) body.u32(value);
        body.u32(1);
        body.u32(static_cast<std::uint32_t>(node_indices.size()));
        for (std::size_t i = 0; i < node_indices.size(); ++i) body.u32(static_cast<std::uint32_t>(i));
    }
    if (!used_skins.empty() && options.skeleton_resource.empty()) { error = "skinned UNIT requires skeleton_resource"; return false; }
    if (options.simple_animation.empty()) {
        body.u32(0);
        body.u32(0);
    } else {
        // Simple animation: an embedded ANIMATION body plus one unnamed group
        // (IdString32 0) binding track i to the SceneGraph node of joint i.
        // basically a normal .animation glued into the unit, ammo belts do the same
        const auto& blob = options.simple_animation;
        if (blob.size() < 20 || read_u32(blob, 16) != blob.size()) { error = "simple animation body has an invalid size header"; return false; }
        std::vector<std::uint32_t> group_nodes;
        if (!skin_node_indices(scene, options.simple_animation_skin, group_nodes, error)) return false;
        if (read_u32(blob, 8) != group_nodes.size()) { error = "simple animation track count must match the skin joint count"; return false; }
        body.u32(static_cast<std::uint32_t>(blob.size()));
        body.bytes(blob.data(), blob.size());
        body.u32(1);
        body.u32(0);
        body.u32(static_cast<std::uint32_t>(group_nodes.size()));
        for (auto node : group_nodes) body.u32(node);
    }
    std::vector<std::uint32_t> source_node_refs;
    std::vector<std::uint32_t> renderer_node_refs;
    std::vector<std::uint32_t> source_node_hashes;
    if (!write_scene_graph(body, scene, root_name_hash, mesh_name_hashes, joint_name_hashes, primitive_skins, source_node_refs, renderer_node_refs, source_node_hashes, error)) return false;
    std::vector<std::uint8_t> physics_scene;
    std::vector<std::vector<std::uint8_t>> actor_records;
    if (!primary_actor.empty()) actor_records.push_back(primary_actor);
    if (options.authored_physics && !options.fitted_physics && !scene.asset_definition.node_bodies.empty()) {
#ifdef DTGLB_HAS_PHYSICS_COLLECTIONS
        std::vector<std::vector<std::uint8_t>> body_records;
        if (!physics::build_authored_physics_scene(scene, source_node_hashes, physics_scene, body_records, error,
                                                   options.ragdoll_handoff)) return false;
        if (!options.ragdoll_handoff)
            for (auto& record : body_records) actor_records.push_back(std::move(record));
#else
        error = "node-bound authored physics requires the native PhysX collection backend";
        return false;
#endif
    }
    std::vector<LodObject> lods;
    std::vector<bool> in_lod;
    if (!plan_lod_objects(scene, lods, in_lod, error)) return false;
    std::vector<std::uint32_t> mesh_render_flags(scene.primitives.size());
    std::vector<bool> mesh_skinned(scene.primitives.size());
    body.u32(static_cast<std::uint32_t>(scene.primitives.size()));
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        const bool skinned = primitive_skins[i] != static_cast<std::size_t>(-1);
        const auto skin_position = skinned ? std::find(used_skins.begin(), used_skins.end(), primitive_skins[i]) : used_skins.end();
        const auto skin_ref = skinned ? static_cast<std::uint32_t>(scene.primitives.size() +
            static_cast<std::size_t>(skin_position - used_skins.begin()) + 1u) : 0u;
        if (renderer_node_refs[i] == 0) {
            error = "MeshObject has no matching renderer SceneGraph node";
            return false;
        }
        // 0x8: drawn only while a LOD object selects it
        const std::uint32_t render_flags = mesh_render_flags_for(scene, scene.primitives[i]) | (in_lod[i] ? 0x00000008u : 0u);
        mesh_render_flags[i] = render_flags;
        mesh_skinned[i] = skinned;
        write_simple_mesh_object(body, render_geometry[i], mesh_name_hashes[i],
                                 renderer_node_refs[i], static_cast<std::uint32_t>(i + 1),
                                 render_flags, skin_ref, skinned);
    }
    BinaryWriter lod_section;
    write_lod_objects(lod_section, lods, options.resource_name, render_geometry, renderer_node_refs, mesh_render_flags, mesh_skinned);
    std::vector<std::uint8_t> flow, flow_dynamic_data;
    if (!scene.asset_definition.flow.empty()) {
        json::Value authored, graph;
        if (!json::parse(scene.asset_definition.flow, authored, error)) { error = "unit flow: " + error; return false; }
        // an effect named without a path is one of this asset's own particle effects (<asset folder>/<name>)
        const auto slash = options.resource_name.find_last_of('/');
        const auto folder = slash == std::string::npos ? std::string() : options.resource_name.substr(0, slash + 1);
        const auto member = [](json::Value& object, const char* name) -> json::Value* {
            for (auto& [key, value] : object.members) if (key == name) return &value;
            return nullptr;
        };
        if (auto* nodes = member(authored, "nodes"); nodes && nodes->is_array())
            for (auto& node : nodes->items) {
                const auto* type = node.find("type");
                auto* inputs = type && type->is_string() && type->string == "particle_effect" ? member(node, "inputs") : nullptr;
                auto* effect = inputs ? member(*inputs, "effect") : nullptr;
                if (effect && effect->is_string() && effect->string.find('/') == std::string::npos)
                    effect->string = folder + effect->string;
                // a mesh named by its object: the object's first mesh (one per material, <object>_p<n>)
                auto* mesh_inputs = type && type->is_string() && type->string == "get_mesh" ? member(node, "inputs") : nullptr;
                auto* mesh = mesh_inputs ? member(*mesh_inputs, "name") : nullptr;
                if (mesh && mesh->is_string() && std::find(mesh_names.begin(), mesh_names.end(), mesh->string) == mesh_names.end())
                    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
                        const int source = scene.primitives[i].source_node;
                        if (source >= 0 && static_cast<std::size_t>(source) < scene.nodes.size() &&
                            scene.nodes[static_cast<std::size_t>(source)].name == mesh->string) {
                            mesh->string = mesh_names[i];
                            break;
                        }
                    }
                // the engine finds a mesh's material by its resource name: a material slot name becomes the
                // material resource bound to that slot
                auto* material_inputs = type && type->is_string() && type->string == "get_material" ? member(node, "inputs") : nullptr;
                auto* material = material_inputs ? member(*material_inputs, "name") : nullptr;
                if (material && material->is_string())
                    for (std::size_t i = 0; i < material_slots.size(); ++i) {
                        const auto& slot = material_slots[i];
                        const int source = scene.primitives[i].material;
                        const bool named = slot == material->string || (source >= 0 &&
                            static_cast<std::size_t>(source) < scene.materials.size() &&
                            scene.materials[static_cast<std::size_t>(source)].name == material->string);
                        if (named) {
                            const auto& bound = std::find_if(options.material_bindings.begin(), options.material_bindings.end(),
                                [&](const auto& binding) { return binding.first == slot; });
                            if (bound != options.material_bindings.end()) material->string = bound->second;
                            else if (!options.material_override.empty()) material->string = options.material_override;
                            break;
                        }
                    }
            }
        if (!flow::build_graph(authored, graph, error) || !flow::encode(graph, flow, flow_dynamic_data, error)) {
            error = "unit flow: " + error;
            return false;
        }
    }
    if (!write_static_tail(body, scene, options, material_slots, actor_records, physics_scene, source_node_refs,
                           lod_section.data(), flow, flow_dynamic_data, error)) return false;

    out.body = body.data();
    return true;
}

bool write_unit_v115(const Scene& scene, const std::filesystem::path& path, std::string& error, const WriteOptions& options) {
    UnitResource resource;
    if (!build_unit_v115(scene, resource, error, options)) return false;
    auto bytes = std::move(resource.body);
    const std::string resource_name = options.resource_name.empty() ? path.stem().string() : options.resource_name;
    if (options.cooked_envelope) {
        if (resource_name.empty()) {
            error = "cooked UNIT output requires a resource name";
            return false;
        }
        bytes = wrap_cooked_resource("unit", resource_name, bytes);
    }
    return save_bytes(path, bytes, error);
}

} // namespace dtglb::stingray::unit
