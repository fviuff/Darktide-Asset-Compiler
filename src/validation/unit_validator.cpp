#include "validation/unit_validator.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"
#include "stingray/flow/flow_resource.h"
#include "stingray/unit/script_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace dtglb::validation {
namespace {

struct Reader {
    const std::vector<std::uint8_t>* bytes = nullptr;
    std::size_t p = 0;
    std::string err;
    bool need(std::size_t n) {
        if (!bytes || p > bytes->size() || n > bytes->size() - p) {
            err = "truncated at byte " + std::to_string(p);
            return false;
        }
        return true;
    }
    bool u8(std::uint8_t& v) { if (!need(1)) return false; v = (*bytes)[p++]; return true; }
    bool u16(std::uint16_t& v) { if (!need(2)) return false; std::memcpy(&v, bytes->data() + p, 2); p += 2; return true; }
    bool u32(std::uint32_t& v) { if (!need(4)) return false; std::memcpy(&v, bytes->data() + p, 4); p += 4; return true; }
    bool u64(std::uint64_t& v) { if (!need(8)) return false; std::memcpy(&v, bytes->data() + p, 8); p += 8; return true; }
    bool skip(std::size_t n) { if (!need(n)) return false; p += n; return true; }
};

struct GeometrySummary {
    std::uint32_t vertices = 0;
    std::uint32_t triangles = 0;
    std::uint32_t slot_hash = 0;
    bool skinned = false;
};

bool zero_u32(Reader& r, const char* label, std::string& report) {
    std::uint32_t value = 0;
    if (!r.u32(value)) { report = "invalid: " + r.err; return false; }
    if (value != 0) { report = std::string("invalid: non-empty ") + label; return false; }
    return true;
}

bool finite_f32(Reader& r, float& value) {
    std::uint32_t bits = 0;
    if (!r.u32(bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

// Bounds-check the native scene container without feeding untrusted bytes to
// PhysX. The serializer's round-trip tests verify the SEBD object contents.
bool validate_physics_scene(Reader& r, const std::set<std::uint32_t>& nodes,
                            std::uint32_t& actor_count, std::string& report) {
    actor_count = 0;
    std::uint32_t size = 0;
    if (!r.u32(size) || !r.need(size)) { report = "invalid: truncated physics scene"; return false; }
    if (!size) return true;
    auto fail = [&]() { report = "invalid: malformed native physics scene or node binding"; return false; };
    if (size < 128u) return fail();
    const auto* data = r.bytes->data() + r.p;
    const std::size_t header = size - 128u;
    auto word = [&](std::size_t offset) { std::uint32_t v; std::memcpy(&v, data + offset, 4); return v; };
    auto h = [&](std::size_t offset) { return word(header + offset); };
    auto range = [&](std::uint32_t offset, std::uint32_t count, std::uint32_t stride) {
        return offset <= header && std::uint64_t(count) * stride <= header - offset;
    };
    if (h(0) != 41200u || h(4) != 0 || h(8) != 0 || h(80) % 128u != 0 ||
        !range(h(8), h(12), 1) || !range(h(80), h(84), 1) ||
        h(12) < 52u || h(84) < 52u || h(12) > h(80) ||
        std::uint64_t(h(80)) + h(84) > h(88) || h(92) == 0) return fail();
    if (std::memcmp(data, "SEBD", 4) || std::memcmp(data + h(80), "SEBD", 4)) return fail();
    for (std::size_t i = 0; i < 16; ++i) {
        float value; const auto bits = h(16 + i * 4); std::memcpy(&value, &bits, 4);
        if (value != (i % 5 == 0 ? 1.0f : 0.0f)) return fail();
    }
    if (!range(h(88), h(92), 72) || !range(h(96), h(100), 4) || h(100) != 0 ||
        !range(h(104), h(108), 16) || !range(h(112), h(116), 8) ||
        !range(h(120), h(124), 8) || h(108) != h(116)) return fail();
    const auto dependency_count = word(48), object_count = word(h(80) + 48);
    std::set<std::uint32_t> actors, actor_names, actor_nodes;
    for (std::uint32_t i = 0; i < h(92); ++i) {
        const auto p = std::size_t(h(88)) + i * 72u;
        if (word(p) >= object_count || !actors.insert(word(p)).second ||
            !actor_names.insert(word(p + 4)).second || !actor_nodes.insert(word(p + 8)).second ||
            !nodes.count(word(p + 8)) || word(p + 12) > 1u || word(p + 44) != 0) return fail();
    }
    for (std::uint32_t i = 0; i < h(108); ++i) {
        const auto p = std::size_t(h(104)) + i * 16u;
        if (word(p) >= h(92) || word(p + 8) >= object_count || word(p + 12) != 6u) return fail();
    }
    for (std::uint32_t i = 0; i < h(116); ++i)
        if (word(std::size_t(h(112)) + i * 8u) >= object_count) return fail();
    for (std::uint32_t i = 0; i < h(124); ++i)
        if (word(std::size_t(h(120)) + i * 8u) >= dependency_count) return fail();
    r.p += size;
    actor_count = h(92);
    return true;
}

bool validate_primary_actor(Reader& r, const std::set<std::uint32_t>& scene_nodes,
                            std::string& report) {
    std::uint32_t name = 0, actor_template = 0, node = 0, shape_count = 0;
    float mass = 0.0f;
    if (!r.u32(name) || !r.u32(actor_template) || !r.u32(node) || !finite_f32(r, mass) ||
        !r.u32(shape_count)) {
        report = "invalid: physics actor header is truncated or non-finite";
        return false;
    }
    if (name == 0 || actor_template == 0 || !scene_nodes.count(node) ||
        mass < 0.0f || shape_count == 0 || !r.bytes ||
        shape_count > (r.bytes->size() - r.p) / 88u) {
        report = "invalid: primary physics actor header is outside the supported family";
        return false;
    }
    for (std::uint32_t i = 0; i < shape_count; ++i) {
        std::uint32_t type = 0, material = 0, shape_template = 0;
        if (!r.u32(type) || !r.u32(material) || !r.u32(shape_template) ||
            (type != 0u && type != 1u && type != 2u && type != 3u && type != 4u) ||
            material == 0 || shape_template == 0) {
            report = "invalid: primary actor contains an unsupported physics shape";
            return false;
        }
        for (int component = 0; component < 16; ++component) {
            float value = 0.0f;
            if (!finite_f32(r, value)) {
                report = "invalid: primitive shape transform is truncated or non-finite";
                return false;
            }
        }
        std::uint32_t cooked_size = 0;
        if (!r.u32(cooked_size)) { report = "invalid: cooked shape size is truncated"; return false; }
        if (type == 3u || type == 4u) {
            if (cooked_size < 12u || !r.need(cooked_size)) {
                report = "invalid: cooked geometry payload is missing or truncated";
                return false;
            }
            const auto& bytes = *r.bytes;
            const char* family = type == 3u ? "MESH" : "CVXM";
            if (bytes[r.p] != 'N' || bytes[r.p + 1] != 'X' || bytes[r.p + 2] != 'S' ||
                bytes[r.p + 3] != 1u ||
                !std::equal(family, family + 4, bytes.begin() + static_cast<std::ptrdiff_t>(r.p + 4))) {
                report = "invalid: cooked geometry payload is not the expected PhysX stream family";
                return false;
            }
            if (!r.skip(cooked_size)) { report = "invalid: " + r.err; return false; }
        } else if (cooked_size != 0u) {
            report = "invalid: primitive shape unexpectedly contains cooked data";
            return false;
        }
        std::uint32_t reserved_node = 0, shape_node = 0;
        if (!r.u32(reserved_node) || !r.u32(shape_node) || reserved_node != 0 ||
            !scene_nodes.count(shape_node)) {
            report = "invalid: physics shape node binding changed";
            return false;
        }
        if (type == 3u && actor_template == stingray::id32_from_id64("dynamic")) {
            report = "invalid: PhysX triangle-mesh collision cannot be dynamic";
            return false;
        }
        const int dimensions = type == 0u ? 1 : (type == 1u ? 3 : (type == 2u ? 2 : 0));
        for (int component = 0; component < dimensions; ++component) {
            float value = 0.0f;
            if (!finite_f32(r, value) || (type == 2u && component == 1 ? value < 0.0f : value <= 0.0f)) {
                report = "invalid: primitive physics dimensions must be finite and positive";
                return false;
            }
        }
    }
    for (int i = 0; i < 6; ++i) {
        std::uint32_t sentinel = 0;
        if (!r.u32(sentinel) || sentinel != 0xffffffffu) {
            report = "invalid: primitive actor trailer sentinels changed";
            return false;
        }
    }
    std::uint8_t enabled = 0, reserved = 0;
    if (!r.u8(enabled) || enabled > 1u) {
        report = "invalid: primitive actor enabled flag is not boolean";
        return false;
    }
    for (int i = 0; i < 3; ++i) if (!r.u8(reserved) || reserved != 0u) {
        report = "invalid: primitive actor trailer padding changed";
        return false;
    }
    return true;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::string& report) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { report = "invalid: cannot open " + path.string(); return false; }
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    f.seekg(0);
    if (n < 0) { report = "invalid: cannot size input"; return false; }
    bytes.resize(static_cast<std::size_t>(n));
    if (!bytes.empty()) f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) { report = "invalid: cannot read input"; return false; }
    return true;
}

bool finite_half_region(const std::vector<std::uint8_t>& body, std::size_t offset, std::size_t size) {
    if (size % 2 != 0 || offset > body.size() || size > body.size() - offset) return false;
    for (std::size_t i = 0; i < size; i += 2) {
        const std::uint16_t bits = static_cast<std::uint16_t>(body[offset + i]) |
                                   static_cast<std::uint16_t>(body[offset + i + 1] << 8);
        if ((bits & 0x7c00u) == 0x7c00u) return false;
    }
    return true;
}

bool finite_float_region(const std::vector<std::uint8_t>& body, std::size_t offset, std::size_t count) {
    if (offset > body.size() || count > (body.size() - offset) / sizeof(float)) return false;
    for (std::size_t i = 0; i < count; ++i) {
        float value = 0.0f;
        std::memcpy(&value, body.data() + offset + i * sizeof(float), sizeof(float));
        if (!std::isfinite(value)) return false;
    }
    return true;
}

bool validate_geometry(Reader& r,
                       const std::vector<std::uint8_t>& body,
                       GeometrySummary& summary,
                       std::string& report) {
    std::uint32_t geometry_version = 0, stream_count = 0;
    if (!r.u32(geometry_version) || geometry_version != 1) {
        report = "invalid: MeshGeometry version is not 1";
        return false;
    }
    if (!r.u32(stream_count) || stream_count < 4 || stream_count > 10) {
        report = "invalid: packed geometry requires 4..10 vertex streams";
        return false;
    }

    std::uint32_t vertex_count = 0;
    std::vector<std::uint32_t> data_sizes(stream_count);
    std::vector<std::uint32_t> strides(stream_count);
    std::vector<std::size_t> data_starts(stream_count);
    for (std::uint32_t i = 0; i < stream_count; ++i) {
        std::uint32_t data_size = 0;
        if (!r.u32(data_size)) { report = "invalid: " + r.err; return false; }
        const std::size_t data_start = r.p;
        if (!r.skip(data_size)) { report = "invalid: " + r.err; return false; }
        std::uint32_t validity = 0, stream_type = 0, count = 0, stride = 0;
        if (!r.u32(validity) || !r.u32(stream_type) || !r.u32(count) || !r.u32(stride)) {
            report = "invalid: " + r.err;
            return false;
        }
        if (validity != 0 || stream_type != 0) {
            report = "invalid: packed vertex stream metadata changed";
            return false;
        }
        if (i == 0) vertex_count = count;
        if (vertex_count == 0 || vertex_count > 65536u || count != vertex_count ||
            data_size != static_cast<std::uint64_t>(count) * stride) {
            report = "invalid: packed vertex stream size/count mismatch";
            return false;
        }
        data_sizes[i] = data_size;
        data_starts[i] = data_start;
        strides[i] = stride;
    }

    const bool skinned = stream_count >= 6u && strides[stream_count - 2u] == 8u && strides.back() == 4u;
    const std::uint32_t uv_count = skinned ? stream_count - 5u : stream_count - 3u;
    if ((skinned && (uv_count < 1u || uv_count > 2u)) || (!skinned && (uv_count < 1u || uv_count > 7u))) {
        report = "invalid: packed geometry stream profile is unsupported";
        return false;
    }
    for (std::uint32_t i = 0; i < stream_count; ++i) {
        std::uint32_t expected_stride = i < 2u ? 8u : 4u;
        if (skinned && i == 3u + uv_count) expected_stride = 8u;
        if (strides[i] != expected_stride) {
            report = "invalid: packed vertex stream stride changed";
            return false;
        }
        // Every stream is binary16 except the final UBYTE4 joint-index stream.
        if (!(skinned && i + 1u == stream_count) &&
            !finite_half_region(body, data_starts[i], data_sizes[i])) {
            report = "invalid: packed vertex stream contains non-finite binary16 data";
            return false;
        }
    }

    std::uint32_t channel_count = 0;
    if (!r.u32(channel_count) || channel_count != stream_count) {
        report = "invalid: channel/stream count mismatch";
        return false;
    }
    for (std::uint32_t i = 0; i < channel_count; ++i) {
        std::uint32_t component = 0, type = 0, set = 0, stream_index = 0;
        std::uint8_t is_instance = 0;
        if (!r.u32(component) || !r.u32(type) || !r.u32(set) || !r.u32(stream_index) || !r.u8(is_instance)) {
            report = "invalid: " + r.err;
            return false;
        }
        std::uint32_t expected_component = 5u, expected_type = 15u, expected_set = i - 3u;
        if (i == 0) { expected_component = 4u; expected_type = 17u; expected_set = 0u; }
        else if (i == 1) { expected_component = 0u; expected_type = 17u; expected_set = 0u; }
        else if (i == 2) { expected_component = 1u; expected_type = 15u; expected_set = 0u; }
        else if (skinned && i == 3u + uv_count) { expected_component = 8u; expected_type = 17u; expected_set = 0u; }
        else if (skinned && i == 4u + uv_count) { expected_component = 7u; expected_type = 19u; expected_set = 0u; }
        if (component != expected_component || type != expected_type || set != expected_set ||
            stream_index != i || is_instance != 0) {
            report = "invalid: packed vertex declaration changed";
            return false;
        }
    }

    std::uint32_t index_validity = 0, index_stream_type = 0, index_format = 0, index_count = 0;
    if (!r.u32(index_validity) || !r.u32(index_stream_type) || !r.u32(index_format) || !r.u32(index_count)) {
        report = "invalid: " + r.err;
        return false;
    }
    if (index_validity != 0 || index_stream_type != 0 || index_format != 0 || index_count == 0 || index_count % 3) {
        report = "invalid: current packed static geometry requires uint16 triangle indices";
        return false;
    }
    std::uint32_t index_data_size = 0;
    if (!r.u32(index_data_size) || index_data_size != static_cast<std::uint64_t>(index_count) * 2u || !r.need(index_data_size)) {
        report = "invalid: packed static uint16 index payload size mismatch";
        return false;
    }
    for (std::uint32_t i = 0; i < index_count; ++i) {
        const std::size_t offset = r.p + static_cast<std::size_t>(i) * 2u;
        const std::uint16_t index = static_cast<std::uint16_t>(body[offset]) |
                                    static_cast<std::uint16_t>(body[offset + 1] << 8);
        if (index >= vertex_count) {
            report = "invalid: packed static index exceeds vertex count";
            return false;
        }
    }
    r.p += index_data_size;

    std::uint32_t batch_count = 0;
    if (!r.u32(batch_count) || batch_count != 1) {
        report = "invalid: each current static MeshGeometry requires one mesh batch";
        return false;
    }
    std::uint32_t material_index = 0, first_triangle = 0, triangle_count = 0, bone_set = 0;
    if (!r.u32(material_index) || !r.u32(first_triangle) || !r.u32(triangle_count) || !r.u32(bone_set)) {
        report = "invalid: " + r.err;
        return false;
    }
    if (material_index != 0 || first_triangle != 0 || triangle_count != index_count / 3 || bone_set != 0) {
        report = "invalid: static mesh batch is inconsistent";
        return false;
    }
    if (!r.skip(40)) { report = "invalid: " + r.err; return false; }
    std::uint32_t slot_count = 0, slot_hash = 0, geometry_trailing = 0;
    if (!r.u32(slot_count) || slot_count != 1 || !r.u32(slot_hash) || !r.u32(geometry_trailing) || geometry_trailing != 0) {
        report = "invalid: static material-slot geometry footer changed";
        return false;
    }

    summary.vertices = vertex_count;
    summary.triangles = index_count / 3;
    summary.slot_hash = slot_hash;
    summary.skinned = skinned;
    return true;
}

} // namespace

bool validate_unit_v115(const std::filesystem::path& path, std::string& report,
                        const std::string& expected_state_machine_resource) {
    std::vector<std::uint8_t> file_bytes;
    if (!read_file(path, file_bytes, report)) return false;

    std::vector<std::uint8_t> body;
    std::string stream;
    std::uint64_t unit_name_hash = 0;
    if (file_bytes.size() >= 4) {
        std::uint32_t first = 0;
        std::memcpy(&first, file_bytes.data(), 4);
        if (first == 115) {
            body = file_bytes;
        } else {
            std::string envelope_error;
            if (!stingray::parse_cooked_resource_envelope(file_bytes, "unit", body, stream, envelope_error)) {
                report = "invalid: " + envelope_error;
                return false;
            }
            if (!stream.empty()) {
                report = "invalid: current static UNIT has external stream data";
                return false;
            }
            if (file_bytes.size() < 16u) { report = "invalid: UNIT envelope is truncated"; return false; }
            std::memcpy(&unit_name_hash, file_bytes.data() + 8, sizeof(unit_name_hash));
        }
    } else {
        report = "invalid: UNIT is truncated";
        return false;
    }

    Reader r{&body};
    std::uint32_t version = 0, geometry_count = 0;
    if (!r.u32(version) || version != 115) {
        report = "invalid: UNIT version is not 115";
        return false;
    }
    if (!r.u32(geometry_count) || geometry_count == 0 || geometry_count >= 65535u) {
        report = "invalid: current static UNIT requires 1..65534 MeshGeometry records";
        return false;
    }

    std::vector<GeometrySummary> geometries(geometry_count);
    std::set<std::uint32_t> geometry_slots;
    std::uint64_t total_vertices = 0;
    std::uint64_t total_triangles = 0;
    for (std::uint32_t i = 0; i < geometry_count; ++i) {
        if (!validate_geometry(r, body, geometries[i], report)) return false;
        geometry_slots.insert(geometries[i].slot_hash);
        total_vertices += geometries[i].vertices;
        total_triangles += geometries[i].triangles;
    }

    std::uint32_t skin_count = 0;
    if (!r.u32(skin_count) || skin_count > 256u) { report = "invalid: SkinDT count is outside supported range"; return false; }
    const auto skinned_geometry_count = static_cast<std::size_t>(std::count_if(
        geometries.begin(), geometries.end(), [](const GeometrySummary& geometry) { return geometry.skinned; }));
    if ((skinned_geometry_count == 0u) != (skin_count == 0u)) {
        report = "invalid: packed skinned geometry and SkinDT presence disagree";
        return false;
    }
    for (std::uint32_t s = 0; s < skin_count; ++s) {
        std::uint32_t matrix_count = 0;
        if (!r.u32(matrix_count) || matrix_count == 0 || matrix_count > 256u || !finite_float_region(body, r.p, static_cast<std::size_t>(matrix_count) * 16u) || !r.skip(static_cast<std::size_t>(matrix_count) * 64u)) { report = "invalid: SkinDT matrices"; return false; }
        std::uint32_t node_indices = 0;
        if (!r.u32(node_indices) || node_indices != matrix_count || !r.skip(static_cast<std::size_t>(node_indices) * 4u)) { report = "invalid: SkinDT node mapping"; return false; }
        std::uint32_t sets = 0;
        if (!r.u32(sets) || sets == 0) { report = "invalid: SkinDT matrix-index sets"; return false; }
        for (std::uint32_t set = 0; set < sets; ++set) { std::uint32_t count = 0; if (!r.u32(count) || count == 0 || count > matrix_count || !r.skip(static_cast<std::size_t>(count) * 4u)) { report = "invalid: SkinDT matrix-index set"; return false; } }
    }
    // Simple animation: ANIMATION body + groups of SceneGraph node indices, one per track.
    std::uint32_t simple_size = 0;
    std::uint32_t simple_tracks = 0;
    if (!r.u32(simple_size)) { report = "invalid: simple animation size"; return false; }
    if (simple_size != 0) {
        std::uint32_t header_tracks = 0, header_size = 0;
        const std::size_t blob_start = r.p;
        if (simple_size < 20 || !r.skip(simple_size)) { report = "invalid: simple animation body"; return false; }
        std::memcpy(&header_tracks, body.data() + blob_start + 8, 4);
        std::memcpy(&header_size, body.data() + blob_start + 16, 4);
        if (header_size != simple_size || header_tracks == 0) { report = "invalid: simple animation header"; return false; }
        simple_tracks = header_tracks;
    }
    std::uint32_t simple_group_count = 0;
    if (!r.u32(simple_group_count) || (simple_size == 0) != (simple_group_count == 0)) {
        report = "invalid: simple animation groups must accompany a simple animation";
        return false;
    }
    std::vector<std::uint32_t> simple_group_nodes;
    for (std::uint32_t g = 0; g < simple_group_count; ++g) {
        std::uint32_t name = 0, count = 0;
        if (!r.u32(name) || !r.u32(count) || count != simple_tracks) { report = "invalid: simple animation group must bind every track"; return false; }
        for (std::uint32_t n = 0; n < count; ++n) {
            std::uint32_t node = 0;
            if (!r.u32(node)) { report = "invalid: simple animation group nodes"; return false; }
            simple_group_nodes.push_back(node);
        }
    }

    std::uint32_t node_count = 0;
    if (!r.u32(node_count) || node_count < geometry_count + 1u || node_count > 65535u) {
        report = "invalid: SceneGraph must contain a root and one renderer node per geometry";
        return false;
    }
    for (const auto node : simple_group_nodes) {
        if (node != 0xffffffffu && node >= node_count) { report = "invalid: simple animation group references a missing SceneGraph node"; return false; }
    }
    const std::uint32_t renderer_start = node_count - geometry_count;
    const std::size_t local_start = r.p;
    const std::size_t local_float_count = static_cast<std::size_t>(node_count) * 15u;
    if (!finite_float_region(body, local_start, local_float_count) || !r.skip(local_float_count * sizeof(float))) {
        report = "invalid: SceneGraph contains invalid local transform data";
        return false;
    }
    const std::size_t world_start = r.p;
    const std::size_t world_float_count = static_cast<std::size_t>(node_count) * 16u;
    if (!finite_float_region(body, world_start, world_float_count) || !r.skip(world_float_count * sizeof(float))) {
        report = "invalid: SceneGraph contains invalid world transform data";
        return false;
    }

    for (std::uint32_t index = 0; index < node_count; ++index) {
        std::uint16_t parent_type = 0, parent_index = 0;
        if (!r.u16(parent_type) || !r.u16(parent_index)) {
            report = "invalid: " + r.err;
            return false;
        }
        if (index == 0) {
            if (parent_type != 0 || parent_index != 0) {
                report = "invalid: synthetic UNIT root SceneGraph linkage changed";
                return false;
            }
        } else if (parent_type != 1 || parent_index >= index || parent_index >= renderer_start) {
            report = index < renderer_start
                ? "invalid: authored SceneGraph node parent must be an earlier authored node"
                : "invalid: renderer SceneGraph node parent must be the root or an authored node";
            return false;
        }
    }

    std::vector<std::uint32_t> scene_names(node_count);
    std::set<std::uint32_t> unique_scene_names;
    for (std::uint32_t index = 0; index < node_count; ++index) {
        if (!r.u32(scene_names[index])) { report = "invalid: " + r.err; return false; }
        if (!unique_scene_names.insert(scene_names[index]).second) {
            report = "invalid: SceneGraph contains duplicate node-name hashes";
            return false;
        }
    }
    if (!zero_u32(r, "SceneGraph trailing vector", report)) return false;

    std::vector<std::uint32_t> mesh_flags;
    std::uint32_t mesh_count = 0;
    if (!r.u32(mesh_count) || mesh_count != geometry_count) {
        report = "invalid: static UNIT MeshObject count must match MeshGeometry count";
        return false;
    }
    for (std::uint32_t i = 0; i < mesh_count; ++i) {
        std::uint32_t mesh_name = 0, node_ref = 0, geometry_ref = 0, skin_ref = 0;
        std::uint32_t render_flags = 0, kind = 0, enabled = 0;
        if (!r.u32(mesh_name) || !r.u32(node_ref) || !r.u32(geometry_ref) || !r.u32(skin_ref) ||
            !r.u32(render_flags) || !r.u32(kind) || !r.u32(enabled)) {
            report = "invalid: " + r.err;
            return false;
        }
        const std::uint32_t expected_ref = i + 1u;
        const std::uint32_t renderer_ref = renderer_start + i;
        const bool skinned = skin_ref != 0;
        const bool valid_skin_ref = !skinned || (skin_ref > geometry_count && skin_ref <= geometry_count + skin_count);
        const bool valid_node_ref = node_ref == renderer_ref && mesh_name == scene_names[renderer_ref];
        if (!valid_node_ref || geometry_ref != expected_ref ||
            skinned != geometries[i].skinned || !valid_skin_ref ||
            ((render_flags & ~0x8u) != 0x000c2003u && (render_flags & ~0x8u) != 0x000c2001u &&
             (render_flags & ~0x8u) != 0x00002002u) ||
            kind != 3 || enabled != 1) {
            report = "invalid: MeshObject is not a packed static or skinned mesh this compiler writes";
            return false;
        }
        float bounds[10]{};
        for (float& value : bounds) {
            std::uint32_t bits = 0;
            if (!r.u32(bits)) { report = "invalid: " + r.err; return false; }
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) { report = "invalid: MeshObject bounds are non-finite"; return false; }
        }
        std::uint32_t mesh_trailing = 0;
        if (!r.u32(mesh_trailing) || mesh_trailing != 0) {
            report = "invalid: simple-static MeshObject trailing field changed";
            return false;
        }
        mesh_flags.push_back(render_flags);
    }

    // Actor records: one primary actor, and/or one record per PhysX-collection body.
    std::uint32_t primary_actor_count = 0;
    if (!r.u32(primary_actor_count) || primary_actor_count > 4096u) {
        report = "invalid: UNIT actor record count";
        return false;
    }
    const std::set<std::uint32_t> scene_node_set(scene_names.begin(), scene_names.end());
    for (std::uint32_t a = 0; a < primary_actor_count; ++a)
        if (!validate_primary_actor(r, scene_node_set, report)) return false;
    for (int i = 0; i < 3; ++i) if (!zero_u32(r, "late static object family", report)) return false;
    std::uint32_t light_count = 0, shadow_lights = 0;
    if (!r.u32(light_count) || light_count > 4096u) { report = "invalid: UNIT light count"; return false; }
    for (std::uint32_t i = 0; i < light_count; ++i) {
        std::uint32_t name = 0, node = 0, flags = 0, type = 0;
        if (!r.u32(name) || !r.u32(node)) { report = "invalid: " + r.err; return false; }
        if (node == 0 || node >= node_count) { report = "invalid: light is bound to a missing SceneGraph node"; return false; }
        float value = 0.0f;
        for (int f = 0; f < 13; ++f) {
            if (!finite_f32(r, value)) { report = "invalid: light color, intensity or shape parameter is non-finite"; return false; }
        }
        if (!r.u32(flags) || !r.u32(type)) { report = "invalid: " + r.err; return false; }
        if (type > 1u) { report = "invalid: only omni and spot UNIT lights are emitted"; return false; }
        if (flags & 2u) { report = "invalid: UNIT light is disabled"; return false; }
        if (flags & 1u) ++shadow_lights;
        if (!r.need(16 + 19 * 4)) { report = "invalid: truncated UNIT light record"; return false; }
        r.p += 16 + 19 * 4;
    }
    if (!zero_u32(r, "late static object family", report)) return false;
    // LOD objects (non-streamed): every LOD mesh carries render flag 0x8 and belongs to one step;
    // steps cover contiguous, falling screen-height ranges from FLT_MAX.
    std::uint32_t lod_count = 0;
    if (!r.u32(lod_count) || lod_count > 256u) { report = "invalid: UNIT LOD object count"; return false; }
    std::vector<int> lod_owner(mesh_count, 0);
    for (std::uint32_t i = 0; i < lod_count; ++i) {
        std::uint32_t name = 0, node = 0, steps = 0, flags = 0, x = 0, order = 0, zero = 0;
        std::uint64_t id = 0;
        std::uint8_t streamed = 0;
        if (!r.u32(name) || !r.u64(id) || !r.u32(node) || !r.u32(steps) || steps == 0 || steps > 32u) { report = "invalid: UNIT LOD object header"; return false; }
        if (node == 0 || node >= node_count) { report = "invalid: LOD object orientation node is missing"; return false; }
        float previous = 0.0f;
        std::uint32_t expected_flags = 0;
        for (std::uint32_t s = 0; s < steps; ++s) {
            float from = 0.0f, to = 0.0f;
            std::uint32_t count = 0, mesh = 0, offset = 0, index = 0;
            if (!finite_f32(r, from) || !finite_f32(r, to) || !r.u32(count) || count == 0 || count > mesh_count) { report = "invalid: UNIT LOD step"; return false; }
            if ((s == 0 ? from != std::numeric_limits<float>::max() : from != previous) || !(to < from) || to < 0.0f) { report = "invalid: LOD step ranges must fall from FLT_MAX without gaps"; return false; }
            previous = to;
            for (std::uint32_t m = 0; m < count; ++m) {
                if (!r.u32(mesh) || mesh >= mesh_count) { report = "invalid: LOD step references a missing mesh"; return false; }
                if (lod_owner[mesh]++ != 0) { report = "invalid: a mesh belongs to more than one LOD step"; return false; }
                expected_flags |= mesh_flags[mesh];
            }
            if (!r.u32(offset) || !r.u32(index) || offset != 0 || index != 0) { report = "invalid: non-streamed LOD step has stream data"; return false; }
        }
        float bounds = 0.0f;
        for (int f = 0; f < 10; ++f) if (!finite_f32(r, bounds)) { report = "invalid: LOD bounding volume is non-finite"; return false; }
        if (!r.u32(flags) || flags != (expected_flags & ~0x10008u)) { report = "invalid: LOD flags differ from its meshes' render flags"; return false; }
        if (!r.u32(x) || x > 1u || !r.u32(order) || order != 0 || !r.u32(zero) || zero != 0 || !r.u8(streamed) || streamed != 0) {
            report = "invalid: UNIT LOD object tail";
            return false;
        }
        (void)name; (void)id;
    }
    for (std::uint32_t m = 0; m < mesh_count; ++m)
        if ((lod_owner[m] != 0) != ((mesh_flags[m] & 0x8u) != 0)) { report = "invalid: LOD render flag 0x8 and LOD step membership disagree"; return false; }
    for (int i = 0; i < 3; ++i) if (!zero_u32(r, "late static object family", report)) return false;
    std::uint32_t mover_count = 0;
    if (!r.u32(mover_count) || mover_count > 16u || !r.need(mover_count * 28u)) {
        report = "invalid: UNIT mover records are truncated";
        return false;
    }
    for (std::uint32_t i = 0; i < mover_count; ++i) {
        std::uint32_t name = 0, filter = 0, on_actor = 0, on_mover = 0;
        float height = 0.0f, radius = 0.0f, slope = 0.0f;
        if (!r.u32(name) || !finite_f32(r, height) || !finite_f32(r, radius) || !r.u32(filter) ||
            !finite_f32(r, slope) || !r.u32(on_actor) || !r.u32(on_mover) || name == 0 || filter == 0 ||
            height <= 0.0f || radius <= 0.0f) {
            report = "invalid: UNIT mover record";
            return false;
        }
    }
    if (!zero_u32(r, "late static object family", report)) return false;
    std::uint8_t animated = 0;
    if (!r.u8(animated) || animated > 1) {
        report = "invalid: UNIT animation-blender flag is truncated or not boolean";
        return false;
    }
    std::uint32_t state_machine_size = 0;
    if (!r.u32(state_machine_size) || state_machine_size > 4096u || !r.need(state_machine_size)) {
        report = "invalid: animation state machine path is truncated or too long";
        return false;
    }
    std::string state_machine_resource;
    state_machine_resource.reserve(state_machine_size);
    for (std::uint32_t i = 0; i < state_machine_size; ++i) {
        const auto c = body[r.p + i];
        if (c == 0 || c < 0x20 || c >= 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            report = "invalid: animation state machine path contains an invalid byte";
            return false;
        }
        state_machine_resource.push_back(static_cast<char>(c));
    }
    r.p += state_machine_size;
    if ((animated != 0) != !state_machine_resource.empty()) {
        report = animated ? "invalid: UNIT animation-blender flag is set without a state machine"
                          : "invalid: UNIT references a state machine but the animation-blender flag is clear "
                            "(the engine never instances it)";
        return false;
    }
    if (!state_machine_resource.empty() &&
        (!unit_name_hash || stingray::resource_name_hash(state_machine_resource) != unit_name_hash)) {
        report = "invalid: animation state machine path does not share the UNIT resource identity";
        return false;
    }
    if (!expected_state_machine_resource.empty() && state_machine_resource != expected_state_machine_resource) {
        report = "invalid: UNIT animation state machine path differs from its graph dependency";
        return false;
    }

    std::uint32_t dynamic_size = 0;
    if (!r.u32(dynamic_size) || dynamic_size < 8 || !r.need(dynamic_size)) {
        report = "invalid: UNIT script data is missing or truncated";
        return false;
    }
    std::size_t data_entries = 0;
    {
        const std::vector<std::uint8_t> data(body.begin() + static_cast<std::ptrdiff_t>(r.p),
                                             body.begin() + static_cast<std::ptrdiff_t>(r.p + dynamic_size));
        json::Value decoded;
        std::string error;
        if (!stingray::unit::decode_script_data(data, decoded, error)) { report = "invalid: UNIT script data: " + error; return false; }
        data_entries = decoded.members.size();
    }
    r.p += dynamic_size;
    std::uint32_t visibility_group_count = 0;
    if (!r.u32(visibility_group_count) || visibility_group_count > 4096u) { report = "invalid: UNIT visibility group count"; return false; }
    std::set<std::uint32_t> visibility_names;
    for (std::uint32_t i = 0; i < visibility_group_count; ++i) {
        std::uint32_t name = 0, count = 0, mesh = 0;
        if (!r.u32(name) || !r.u32(count) || count > geometry_count) { report = "invalid: UNIT visibility group header"; return false; }
        if (!visibility_names.insert(name).second) { report = "invalid: duplicate UNIT visibility group name"; return false; }
        for (std::uint32_t m = 0; m < count; ++m)
            if (!r.u32(mesh) || mesh >= geometry_count) { report = "invalid: visibility group references a missing mesh"; return false; }
    }
    // unit flow: the graph must decode (and re-encode identically) as the game's graphs do
    std::size_t flow_nodes = 0;
    {
        std::array<std::vector<std::uint8_t>, 2> blobs;
        for (auto& blob : blobs) {
            std::uint32_t size = 0;
            if (!r.u32(size) || !r.need(size)) { report = "invalid: UNIT flow data is truncated"; return false; }
            blob.assign(body.begin() + static_cast<std::ptrdiff_t>(r.p), body.begin() + static_cast<std::ptrdiff_t>(r.p + size));
            r.p += size;
        }
        if (blobs[0].empty() != blobs[1].empty()) { report = "invalid: UNIT flow and flow dynamic data must come together"; return false; }
        if (!blobs[0].empty()) {
            json::Value graph;
            std::vector<std::uint8_t> flow_again, dynamic_again;
            std::string error;
            if (!stingray::flow::decode(blobs[0], blobs[1], graph, error) ||
                !stingray::flow::encode(graph, flow_again, dynamic_again, error) ||
                flow_again != blobs[0] || dynamic_again != blobs[1]) {
                report = "invalid: UNIT flow graph does not decode: " + error;
                return false;
            }
            flow_nodes = graph.find("nodes")->items.size();
        }
    }

    std::uint32_t pre_physics_size = 0;
    if (!r.u32(pre_physics_size) || pre_physics_size != 4 || !r.need(4)) {
        report = "invalid: current static pre-physics blob is missing";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (body[r.p + static_cast<std::size_t>(i)] != 0) {
            report = "invalid: current static pre-physics blob changed";
            return false;
        }
    }
    r.p += 4;
    std::uint32_t collection_actor_count = 0;
    // Collection bodies are unit actors of their own (created at spawn when enabled, or later
    // by a ragdoll state), so they need no UNIT actor record.
    if (!validate_physics_scene(r, scene_node_set, collection_actor_count, report)) return false;

    std::uint64_t default_material = 0;
    if (!r.u64(default_material) || default_material != 0) {
        report = "invalid: current static UNIT has a default material resource";
        return false;
    }
    std::uint32_t material_count = 0;
    if (!r.u32(material_count) || material_count == 0 || material_count != geometry_slots.size()) {
        report = "invalid: static UNIT material map must cover each unique MeshGeometry slot exactly once";
        return false;
    }
    std::set<std::uint32_t> material_slots;
    for (std::uint32_t i = 0; i < material_count; ++i) {
        std::uint32_t material_slot = 0;
        std::uint64_t material_resource = 0;
        if (!r.u32(material_slot) || !r.u64(material_resource)) {
            report = "invalid: " + r.err;
            return false;
        }
        (void)material_resource;
        if (!material_slots.insert(material_slot).second) {
            report = "invalid: static UNIT material map contains a duplicate slot";
            return false;
        }
    }
    if (material_slots != geometry_slots) {
        report = "invalid: MeshGeometry slots and UNIT material map disagree";
        return false;
    }

    std::uint64_t reserved = 0;
    for (int i = 0; i < 2; ++i) {
        if (!r.u64(reserved) || reserved != 0) {
            report = "invalid: final reserved region changed";
            return false;
        }
    }
    std::uint64_t skeleton_resource = 0;
    if (!r.u64(skeleton_resource) || ((skin_count != 0u) != (skeleton_resource != 0u))) {
        report = "invalid: skeleton resource reference and SkinDT presence disagree";
        return false;
    }
    if (!zero_u32(r, "final UNIT field", report)) return false;
    if (r.p != body.size()) {
        report = "invalid: " + std::to_string(body.size() - r.p) + " trailing byte(s) after UNIT contract";
        return false;
    }

    std::ostringstream ss;
    ss << "valid UNIT v115: " << geometry_count << " packed mesh primitive(s), "
       << skinned_geometry_count << " skinned, "
       << node_count << " SceneGraph node(s), " << total_vertices << " vertices, "
       << total_triangles << " triangles, " << material_count << " material slot(s), "
       << primary_actor_count << " physics actor(s)";
    if (collection_actor_count) ss << " (" << collection_actor_count << " in PhysX collection)";
    ss << ", ";
    if (light_count != 0) ss << light_count << " light(s)" << (shadow_lights ? " (" + std::to_string(shadow_lights) + " casting shadows)" : std::string()) << ", ";
    if (visibility_group_count != 0) ss << visibility_group_count << " visibility group(s), ";
    if (lod_count != 0) ss << lod_count << " LOD object(s), ";
    if (mover_count != 0) ss << mover_count << " mover(s), ";
    if (flow_nodes != 0) ss << "flow " << flow_nodes << " node(s), ";
    if (data_entries != 0) ss << data_entries << " data value(s), ";
    if (simple_tracks != 0) ss << "simple animation " << simple_tracks << " track(s), ";
    ss << file_bytes.size() << " bytes";
    report = ss.str();
    return true;
}

} // namespace dtglb::validation
