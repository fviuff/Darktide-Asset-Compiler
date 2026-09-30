#include "processing/topology_canonicalizer.h"
#include "processing/tangent_generator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_map>

namespace dtglb::processing {
namespace {
using Vec3 = std::array<float, 3>;

Vec3 add(Vec3 a, Vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(Vec3 a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(Vec3 a, Vec3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float size = length(v);
    return size > 1e-12f && std::isfinite(size) ? mul(v, 1.0f / size) : fallback;
}

const VertexChannel* position_channel(const std::vector<VertexChannel>& channels) {
    for (const auto& channel : channels)
        if (channel.semantic == VertexChannel::Semantic::Position && channel.set == 0) return &channel;
    return nullptr;
}

Vec3 row_position(const VertexChannel& position, std::uint32_t index) {
    const auto base = static_cast<std::size_t>(index) * position.components;
    return {position.values[base], position.values[base + 1], position.values[base + 2]};
}

struct Pattern {
    std::vector<std::uint32_t> source_vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Vec3> offsets;
};

bool valid_finite(const std::vector<float>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

bool validate_rows(const std::vector<VertexChannel>& channels, std::size_t vertices, const char* label,
                  std::string& error) {
    for (const auto& channel : channels) {
        if (channel.components == 0 || channel.components > 4 ||
            vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
            channel.values.size() != vertices * channel.components || !valid_finite(channel.values)) {
            error = std::string(label) + " channel has malformed or non-finite vertex rows";
            return false;
        }
    }
    return true;
}

void update_bounds(const std::vector<VertexChannel>& channels,
                   std::array<float, 3>& minimum, std::array<float, 3>& maximum,
                   float& radius) {
    const auto* position = position_channel(channels);
    if (!position || position->components < 3 || position->values.empty()) return;
    minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
               std::numeric_limits<float>::infinity()};
    maximum = {-minimum[0], -minimum[1], -minimum[2]};
    float radius_squared = 0.0f;
    for (std::size_t row = 0; row + 2 < position->values.size(); row += position->components) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], position->values[row + axis]);
            maximum[axis] = std::max(maximum[axis], position->values[row + axis]);
        }
        radius_squared = std::max(radius_squared,
            position->values[row] * position->values[row] +
            position->values[row + 1] * position->values[row + 1] +
            position->values[row + 2] * position->values[row + 2]);
    }
    radius = std::sqrt(radius_squared);
}

void update_bounds(Primitive& primitive) {
    update_bounds(primitive.source_channels, primitive.source_bounds_min,
                  primitive.source_bounds_max, primitive.source_bounds_radius);
    update_bounds(primitive.channels, primitive.bounds_min,
                  primitive.bounds_max, primitive.bounds_radius);
}

Pattern points_pattern(const std::vector<std::uint32_t>& raw, const std::vector<VertexChannel>& channels,
                       std::size_t vertex_count) {
    const auto* position = position_channel(channels);
    Vec3 min = row_position(*position, 0), max = min;
    for (std::size_t i = 1; i < vertex_count; ++i) {
        const auto value = row_position(*position, static_cast<std::uint32_t>(i));
        for (int axis = 0; axis < 3; ++axis) { min[axis] = std::min(min[axis], value[axis]); max[axis] = std::max(max[axis], value[axis]); }
    }
    const float extent = std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});
    const float radius = std::isfinite(extent) && extent > 1e-12f ? extent * 0.01f : 0.01f;
    Pattern result;
    for (const auto source : raw) {
        const std::uint32_t base = static_cast<std::uint32_t>(result.source_vertices.size());
        result.source_vertices.insert(result.source_vertices.end(), {source, source, source, source});
        result.offsets.insert(result.offsets.end(), {
            {radius, radius, radius}, {radius, -radius, -radius},
            {-radius, radius, -radius}, {-radius, -radius, radius}});
        result.indices.insert(result.indices.end(), {
            base, base + 2, base + 1,
            base, base + 1, base + 3,
            base, base + 3, base + 2,
            base + 1, base + 2, base + 3});
    }
    return result;
}

Pattern lines_pattern(const std::vector<std::uint32_t>& raw, int mode,
                      const std::vector<VertexChannel>& channels, std::size_t vertex_count) {
    const auto* position = position_channel(channels);
    Vec3 min = row_position(*position, 0), max = min;
    for (std::size_t i = 1; i < vertex_count; ++i) {
        const auto value = row_position(*position, static_cast<std::uint32_t>(i));
        for (int axis = 0; axis < 3; ++axis) { min[axis] = std::min(min[axis], value[axis]); max[axis] = std::max(max[axis], value[axis]); }
    }
    const float extent = std::max({max[0] - min[0], max[1] - min[1], max[2] - min[2]});
    const float radius = std::isfinite(extent) && extent > 1e-12f ? extent * 0.005f : 0.005f;
    Pattern result;
    const std::size_t step = mode == 1 ? 2u : 1u;
    const std::size_t segment_count = mode == 1 ? raw.size() / 2u :
        (mode == 2 ? raw.size() : (raw.size() > 1 ? raw.size() - 1 : 0));
    for (std::size_t segment = 0; segment < segment_count; ++segment) {
        const auto a = raw[segment * step];
        const auto b = mode == 2 ? raw[(segment + 1) % raw.size()] : raw[segment + 1];
        const auto pa = row_position(*position, a), pb = row_position(*position, b);
        const auto direction = normalized(sub(pb, pa), {1, 0, 0});
        const Vec3 reference = std::fabs(direction[0]) < 0.8f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const auto side = normalized(cross(direction, reference), {0, 0, 1});
        const auto up = normalized(cross(direction, side), {0, 1, 0});
        const std::uint32_t base = static_cast<std::uint32_t>(result.source_vertices.size());
        result.source_vertices.insert(result.source_vertices.end(), {a, a, a, a, b, b, b, b});
        for (int end = 0; end < 2; ++end) for (int corner = 0; corner < 4; ++corner) {
            const float s = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
            const float u = (corner < 2) ? -1.0f : 1.0f;
            result.offsets.push_back(add(mul(side, s * radius), mul(up, u * radius)));
        }
        for (int corner = 0; corner < 4; ++corner) {
            const auto n = (corner + 1) % 4;
            result.indices.insert(result.indices.end(), {base + static_cast<std::uint32_t>(corner), base + static_cast<std::uint32_t>(n), base + 4u + static_cast<std::uint32_t>(n), base + static_cast<std::uint32_t>(corner), base + 4u + static_cast<std::uint32_t>(n), base + 4u + static_cast<std::uint32_t>(corner)});
        }
        result.indices.insert(result.indices.end(), {
            base, base + 2u, base + 1u, base, base + 3u, base + 2u,
            base + 4u, base + 5u, base + 6u, base + 4u, base + 6u, base + 7u});
    }
    return result;
}

void append_channel_rows(std::vector<VertexChannel>& output, const std::vector<VertexChannel>& input,
                         const Pattern& pattern, const std::vector<Vec3>& offsets, bool normals,
                         const std::vector<Vec3>& generated_normals, std::size_t vertices) {
    for (const auto& channel : input) {
        if (channel.semantic == VertexChannel::Semantic::Normal || channel.semantic == VertexChannel::Semantic::Tangent) continue;
        VertexChannel copy = channel;
        copy.values.clear(); copy.values.reserve(pattern.source_vertices.size() * channel.components);
        for (std::size_t i = 0; i < pattern.source_vertices.size(); ++i) {
            const auto source = static_cast<std::size_t>(pattern.source_vertices[i]);
            copy.values.insert(copy.values.end(), channel.values.begin() + static_cast<std::ptrdiff_t>(source * channel.components), channel.values.begin() + static_cast<std::ptrdiff_t>((source + 1) * channel.components));
            if (channel.semantic == VertexChannel::Semantic::Position && channel.components >= 3) {
                const auto base = i * channel.components;
                copy.values[base] += offsets[i][0]; copy.values[base + 1] += offsets[i][1]; copy.values[base + 2] += offsets[i][2];
            }
        }
        output.push_back(std::move(copy));
    }
    VertexChannel normal;
    normal.semantic = VertexChannel::Semantic::Normal; normal.components = 3;
    normal.values.reserve(pattern.source_vertices.size() * 3u);
    for (const auto& value : generated_normals) normal.values.insert(normal.values.end(), value.begin(), value.end());
    output.push_back(std::move(normal));
    (void)normals; (void)vertices;
}

bool expand_primitive(const Primitive& input, Primitive& output, std::string& error) {
    const auto* source_position = position_channel(input.source_channels);
    const auto* current_position = position_channel(input.channels);
    if (!source_position || !current_position || source_position->components < 3 || current_position->components < 3) { error = "non-triangle primitive is missing POSITION"; return false; }
    const std::size_t source_vertices = source_position->values.size() / source_position->components;
    const std::size_t current_vertices = current_position->values.size() / current_position->components;
    if (source_vertices == 0 || source_vertices != current_vertices || !validate_rows(input.source_channels, source_vertices, "source", error) || !validate_rows(input.channels, current_vertices, "current", error)) return false;
    for (const auto& custom : input.custom_attributes) if (custom.components == 0 || custom.components > 4 ||
        source_vertices > std::numeric_limits<std::size_t>::max() / custom.components ||
        custom.values.size() != source_vertices * custom.components || !valid_finite(custom.values)) { error = "non-triangle primitive has malformed custom attribute rows"; return false; }
    for (const auto index : input.indices) if (index >= source_vertices) { error = "non-triangle primitive index exceeds vertex count"; return false; }
    if (input.indices.empty()) { error = "non-triangle primitive has no vertices"; return false; }
    std::vector<std::uint32_t> lowered_indices = input.indices;
    if (input.mode == 1) {
        if (lowered_indices.size() % 2 != 0) lowered_indices.pop_back();
        if (lowered_indices.empty()) lowered_indices = {input.indices.front(), input.indices.front()};
    } else if ((input.mode == 2 || input.mode == 3) && lowered_indices.size() < 2) {
        lowered_indices = {lowered_indices.front(), lowered_indices.front()};
    }
    const auto source_pattern = input.mode == 0 ? points_pattern(lowered_indices, input.source_channels, source_vertices) : lines_pattern(lowered_indices, input.mode, input.source_channels, source_vertices);
    const auto current_pattern = input.mode == 0 ? points_pattern(lowered_indices, input.channels, current_vertices) : lines_pattern(lowered_indices, input.mode, input.channels, current_vertices);
    if (source_pattern.source_vertices != current_pattern.source_vertices || source_pattern.indices != current_pattern.indices) { error = "non-triangle topology expansion was nondeterministic"; return false; }
    output = input; output.mode = 4; output.indices = source_pattern.indices; output.source_channels.clear(); output.channels.clear(); output.custom_attributes.clear();
    auto build_normals = [](const std::vector<VertexChannel>& channels, const Pattern& pattern) {
        std::vector<Vec3> normals(pattern.source_vertices.size(), {0, 0, 0});
        const auto* position = position_channel(channels);
        for (std::size_t i = 0; i + 2 < pattern.indices.size(); i += 3) {
            const auto a = pattern.indices[i], b = pattern.indices[i + 1], c = pattern.indices[i + 2];
            const auto pa = add(row_position(*position, pattern.source_vertices[a]), pattern.offsets[a]);
            const auto pb = add(row_position(*position, pattern.source_vertices[b]), pattern.offsets[b]);
            const auto pc = add(row_position(*position, pattern.source_vertices[c]), pattern.offsets[c]);
            const auto face = cross(sub(pb, pa), sub(pc, pa));
            normals[a] = add(normals[a], face); normals[b] = add(normals[b], face); normals[c] = add(normals[c], face);
        }
        for (auto& normal : normals) normal = normalized(normal, {0, 0, 1});
        return normals;
    };
    append_channel_rows(output.source_channels, input.source_channels, source_pattern, source_pattern.offsets, true, build_normals(input.source_channels, source_pattern), source_vertices);
    append_channel_rows(output.channels, input.channels, current_pattern, current_pattern.offsets, true, build_normals(input.channels, current_pattern), current_vertices);
    for (const auto& attribute : input.custom_attributes) {
        auto copy = attribute; copy.values.clear(); copy.values.reserve(source_pattern.source_vertices.size() * attribute.components);
        for (const auto source : source_pattern.source_vertices) copy.values.insert(copy.values.end(), attribute.values.begin() + static_cast<std::ptrdiff_t>(source * attribute.components), attribute.values.begin() + static_cast<std::ptrdiff_t>((source + 1) * attribute.components));
        output.custom_attributes.push_back(std::move(copy));
    }
    output.morph_targets.clear();
    for (const auto& target : input.morph_targets) {
        MorphTarget copy; copy.name = target.name;
        for (const auto& channel : target.channels) {
            if (channel.semantic == VertexChannel::Semantic::Tangent) continue;
            if (channel.components == 0 || channel.components > 4 ||
                source_vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
                channel.values.size() != source_vertices * channel.components || !valid_finite(channel.values)) { error = "non-triangle morph target has malformed rows"; return false; }
            auto row = channel; row.values.clear();
            for (const auto source : source_pattern.source_vertices) row.values.insert(row.values.end(), channel.values.begin() + static_cast<std::ptrdiff_t>(source * channel.components), channel.values.begin() + static_cast<std::ptrdiff_t>((source + 1) * channel.components));
            copy.channels.push_back(std::move(row));
        }
        output.morph_targets.push_back(std::move(copy));
    }
    update_bounds(output);
    return true;
}

std::vector<float> remap_rows(const std::vector<float>& values, std::uint32_t components,
                              const std::vector<std::uint32_t>& vertices) {
    std::vector<float> result;
    result.reserve(vertices.size() * components);
    for (const auto vertex : vertices) {
        const auto begin = static_cast<std::size_t>(vertex) * components;
        result.insert(result.end(), values.begin() + static_cast<std::ptrdiff_t>(begin),
                      values.begin() + static_cast<std::ptrdiff_t>(begin + components));
    }
    return result;
}

bool partition_primitive(const Primitive& input, std::size_t max_triangles,
                         std::vector<Primitive>& output, std::string& error) {
    const auto& source_channels = input.source_channels.empty() ? input.channels : input.source_channels;
    const auto* position = position_channel(source_channels);
    if (!position || position->components == 0 || position->values.size() % position->components != 0) {
        error = "triangle primitive is missing a valid POSITION channel";
        return false;
    }
    if (input.mode != 4 || input.indices.empty() || input.indices.size() % 3 != 0) {
        error = "triangle primitive topology is malformed";
        return false;
    }
    const auto vertices = position->values.size() / position->components;
    if (max_triangles == 0) { error = "triangle partition budget is zero"; return false; }
    const auto validate_channels = [&](const std::vector<VertexChannel>& channels, const char* label) {
        for (const auto& channel : channels) {
            if (channel.components == 0 || channel.components > 4 ||
                vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
                channel.values.size() != vertices * channel.components) {
                error = std::string(label) + " channel rows are malformed for triangle partitioning";
                return false;
            }
        }
        return true;
    };
    if (!validate_channels(input.source_channels, "source") || !validate_channels(input.channels, "current")) return false;
    for (const auto& attribute : input.custom_attributes) {
        if (attribute.components == 0 || attribute.components > 4 ||
            vertices > std::numeric_limits<std::size_t>::max() / attribute.components ||
            attribute.values.size() != vertices * attribute.components) {
            error = "custom attribute rows are malformed for triangle partitioning";
            return false;
        }
    }
    for (const auto& target : input.morph_targets) {
        for (const auto& channel : target.channels) {
            if (channel.components == 0 || channel.components > 4 ||
                vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
                channel.values.size() != vertices * channel.components) {
                error = "morph target rows are malformed for triangle partitioning";
                return false;
            }
        }
    }
    for (const auto index : input.indices) {
        if (index >= vertices) { error = "triangle index exceeds POSITION rows during partitioning"; return false; }
    }
    if (vertices <= 65535u && input.indices.size() / 3 <= max_triangles) { output.push_back(input); return true; }
    struct Chunk { std::vector<std::uint32_t> vertices; std::vector<std::uint32_t> indices; };
    Chunk chunk;
    std::unordered_map<std::uint32_t, std::uint32_t> local;
    auto flush = [&]() {
        if (chunk.indices.empty()) return;
        Primitive part = input;
        part.name += "_part" + std::to_string(output.size());
        part.indices = chunk.indices;
        part.source_channels.clear(); part.channels.clear(); part.custom_attributes.clear(); part.morph_targets.clear();
        for (const auto& channel : input.source_channels) { auto copy = channel; copy.values = remap_rows(channel.values, channel.components, chunk.vertices); part.source_channels.push_back(std::move(copy)); }
        for (const auto& channel : input.channels) { auto copy = channel; copy.values = remap_rows(channel.values, channel.components, chunk.vertices); part.channels.push_back(std::move(copy)); }
        for (const auto& attribute : input.custom_attributes) { auto copy = attribute; copy.values = remap_rows(attribute.values, attribute.components, chunk.vertices); part.custom_attributes.push_back(std::move(copy)); }
        for (const auto& target : input.morph_targets) { MorphTarget copy; copy.name = target.name; for (const auto& channel : target.channels) { auto row = channel; row.values = remap_rows(channel.values, channel.components, chunk.vertices); copy.channels.push_back(std::move(row)); } part.morph_targets.push_back(std::move(copy)); }
        update_bounds(part);
        output.push_back(std::move(part));
        chunk = {}; local.clear();
    };
    for (std::size_t i = 0; i < input.indices.size(); i += 3) {
        std::size_t needed = 0;
        for (std::size_t corner = 0; corner < 3; ++corner) if (!local.count(input.indices[i + corner])) ++needed;
        if (!chunk.indices.empty() &&
            (local.size() + needed > 65535u || chunk.indices.size() / 3u >= max_triangles)) flush();
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto source = input.indices[i + corner];
            auto [it, inserted] = local.emplace(source, static_cast<std::uint32_t>(local.size()));
            if (inserted) chunk.vertices.push_back(source);
            chunk.indices.push_back(it->second);
        }
    }
    flush();
    return true;
}

} // namespace

bool canonicalize_non_triangle_topology(Scene& scene, TopologyCanonicalizationReport& report, std::string& error) {
    report = {};
    error.clear();
    try {
        std::vector<Primitive> expanded;
        expanded.reserve(scene.primitives.size());
        for (std::size_t index = 0; index < scene.primitives.size(); ++index) {
            const auto& primitive = scene.primitives[index];
            if (primitive.mode == 4) { expanded.push_back(primitive); continue; }
            if (primitive.mode < 0 || primitive.mode > 3) {
                error = "primitive contains an invalid residual topology mode";
                return false;
            }
            Primitive replacement;
            const bool incomplete_lines = primitive.mode == 1 && (primitive.indices.size() < 2 || (primitive.indices.size() % 2) != 0);
            const bool short_line_sequence = (primitive.mode == 2 || primitive.mode == 3) && primitive.indices.size() < 2;
            if (!expand_primitive(primitive, replacement, error)) return false;
            if (regenerate_indexed_tangents(replacement, error) == TangentRegenerationStatus::Invalid)
                return false;
            report.changed = true; report.lossy = true;
            report.approximations.push_back("topology_mesh_expansion");
            if (incomplete_lines || short_line_sequence) {
                report.approximations.push_back("incomplete_topology_tail");
                report.diagnostics.push_back({index, "incomplete_topology_tail", "discarded incomplete line tail or lowered short sequence to a degenerate placeholder"});
                scene.notes.push_back("primitive " + std::to_string(index) + ": discarded incomplete line tail or lowered short sequence to a degenerate placeholder [incomplete_topology_tail]");
            }
            report.diagnostics.push_back({index, "topology_mesh_expansion", "expanded non-triangle topology into deterministic triangle geometry"});
            scene.notes.push_back("primitive " + std::to_string(index) + ": expanded non-triangle topology into triangle mesh [topology_mesh_expansion]");
            if (!partition_primitive(replacement, std::numeric_limits<std::size_t>::max(), expanded, error)) return false;
        }
        scene.primitives = std::move(expanded);
    } catch (const std::bad_alloc&) {
        error = "topology canonicalization allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "topology canonicalization exceeds the container limit";
        return false;
    }
    return true;
}

bool partition_triangle_primitive(const Primitive& input, std::size_t max_triangles,
                                  std::vector<Primitive>& output, std::string& error) {
    output.clear();
    try {
        return partition_primitive(input, max_triangles, output, error);
    } catch (const std::bad_alloc&) {
        output.clear();
        error = "triangle partition allocation failed";
        return false;
    } catch (const std::length_error&) {
        output.clear();
        error = "triangle partition exceeds the container limit";
        return false;
    }
}

} // namespace dtglb::processing
