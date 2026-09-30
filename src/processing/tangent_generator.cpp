#include "processing/tangent_generator.h"

#include "mikktspace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dtglb::processing {
namespace {
struct MeshData {
    const VertexChannel* position = nullptr;
    const VertexChannel* normal = nullptr;
    const VertexChannel* uv = nullptr;
    const std::vector<std::uint32_t>* indices = nullptr;
    std::size_t vertices = 0;
    std::vector<std::array<float, 3>> corner_tangent;
    std::vector<float> corner_sign;
};

bool finite_values(const VertexChannel& channel) {
    return std::all_of(channel.values.begin(), channel.values.end(), [](float value) { return std::isfinite(value); });
}

const VertexChannel* find(const std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    for (const auto& channel : channels)
        if (channel.semantic == semantic && channel.set == set) return &channel;
    return nullptr;
}

bool validate_channel(const VertexChannel& channel, std::size_t vertices, const char* label, std::string& error) {
    if (channel.components == 0 || vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
        channel.values.size() != vertices * channel.components || !finite_values(channel)) {
        error = std::string("atlas tangent ") + label + " channel has invalid rows or non-finite values";
        return false;
    }
    return true;
}

std::size_t corner_index(int face, int vertex) {
    return static_cast<std::size_t>(face) * 3u + static_cast<std::size_t>(vertex);
}

std::size_t vertex_index(const MeshData& mesh, int face, int vertex) {
    return static_cast<std::size_t>((*mesh.indices)[corner_index(face, vertex)]);
}

std::array<float, 3> normal_for(const MeshData& mesh, std::size_t index) {
    std::array<float, 3> n{mesh.normal->values[index * mesh.normal->components], mesh.normal->values[index * mesh.normal->components + 1], mesh.normal->values[index * mesh.normal->components + 2]};
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (length > 1e-20f) for (float& value : n) value /= length;
    else n = {0.0f, 0.0f, 1.0f};
    return n;
}

void get_position(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const auto& mesh = *static_cast<const MeshData*>(context->m_pUserData);
    const auto index = vertex_index(mesh, face, vertex);
    for (std::uint32_t c = 0; c != 3; ++c) out[c] = mesh.position->values[index * mesh.position->components + c];
}
void get_normal(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const auto& mesh = *static_cast<const MeshData*>(context->m_pUserData);
    const auto n = normal_for(mesh, vertex_index(mesh, face, vertex));
    std::copy(n.begin(), n.end(), out);
}
void get_uv(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const auto& mesh = *static_cast<const MeshData*>(context->m_pUserData);
    const auto index = vertex_index(mesh, face, vertex);
    out[0] = mesh.uv->values[index * mesh.uv->components];
    out[1] = mesh.uv->values[index * mesh.uv->components + 1];
}
void set_tangent(const SMikkTSpaceContext* context, const float tangent[], const float sign, int face, int vertex) {
    auto& mesh = *static_cast<MeshData*>(context->m_pUserData);
    const auto index = corner_index(face, vertex);
    for (int c = 0; c != 3; ++c) mesh.corner_tangent[index][c] = tangent[c];
    mesh.corner_sign[index] = sign;
}

bool regenerate_channels(std::vector<VertexChannel>& channels, const std::vector<std::uint32_t>& indices, std::string& error) {
    const auto* position = find(channels, VertexChannel::Semantic::Position);
    const auto* normal = find(channels, VertexChannel::Semantic::Normal);
    const auto* uv = find(channels, VertexChannel::Semantic::Texcoord, 0);
    if (!position || !normal || !uv) return true;
    if (position->components < 3 || normal->components < 3 || uv->components < 2 ||
        position->values.size() % position->components != 0) {
        error = "atlas tangent source channels have invalid component counts"; return false;
    }
    const auto vertices = position->values.size() / position->components;
    if (vertices == 0 || indices.empty() || indices.size() % 3 != 0) {
        error = "tangent regeneration requires indexed triangles"; return false;
    }
    if (!validate_channel(*position, vertices, "position", error) ||
        !validate_channel(*normal, vertices, "normal", error) || !validate_channel(*uv, vertices, "UV0", error)) return false;
    for (const auto index : indices)
        if (index >= vertices) { error = "tangent regeneration index exceeds vertex count"; return false; }

    MeshData mesh{position, normal, uv, &indices, vertices,
                  std::vector<std::array<float, 3>>(indices.size()),
                  std::vector<float>(indices.size(), 1.0f)};
    SMikkTSpaceInterface interface{};
    if (indices.size() / 3u > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "tangent regeneration triangle count exceeds MikkTSpace range";
        return false;
    }
    interface.m_getNumFaces = [](const SMikkTSpaceContext* context) {
        return static_cast<int>(static_cast<const MeshData*>(context->m_pUserData)->indices->size() / 3u);
    };
    interface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    interface.m_getPosition = get_position;
    interface.m_getNormal = get_normal;
    interface.m_getTexCoord = get_uv;
    interface.m_setTSpaceBasic = set_tangent;
    interface.m_setTSpace = nullptr;
    SMikkTSpaceContext context{&interface, &mesh};
    if (!genTangSpaceDefault(&context)) {
        // Mikk may decline a wholly degenerate UV island. Keep the operation
        // deterministic and finite; the orthogonal fallback below resolves it
        // against each vertex normal.
        for (auto& value : mesh.corner_tangent) value = {1.0f, 0.0f, 0.0f};
        std::fill(mesh.corner_sign.begin(), mesh.corner_sign.end(), 1.0f);
    }

    VertexChannel output;
    output.semantic = VertexChannel::Semantic::Tangent;
    output.set = 0;
    output.components = 4;
    output.values.resize(vertices * 4);
    std::vector<std::array<float, 3>> accumulated(vertices);
    std::vector<float> accumulated_sign(vertices);
    for (std::size_t corner = 0; corner < indices.size(); ++corner) {
        const auto vertex = static_cast<std::size_t>(indices[corner]);
        for (std::size_t c = 0; c < 3; ++c) accumulated[vertex][c] += mesh.corner_tangent[corner][c];
        accumulated_sign[vertex] += mesh.corner_sign[corner];
    }
    for (std::size_t i = 0; i != vertices; ++i) {
        const auto n = normal_for(mesh, i);
        auto t = accumulated[i];
        float ndt = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
        for (int c = 0; c != 3; ++c) t[c] -= n[c] * ndt;
        float length = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (!std::isfinite(length) || length <= 1e-20f) {
            const std::array<float, 3> axis = std::fabs(n[2]) < 0.9f ? std::array<float, 3>{0, 0, 1} : std::array<float, 3>{1, 0, 0};
            t = {n[1] * axis[2] - n[2] * axis[1], n[2] * axis[0] - n[0] * axis[2], n[0] * axis[1] - n[1] * axis[0]};
            length = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        }
        for (float& value : t) value /= length;
        for (int c = 0; c != 3; ++c) output.values[i * 4 + c] = t[c];
        output.values[i * 4 + 3] = std::isfinite(accumulated_sign[i]) && accumulated_sign[i] < 0.0f ? -1.0f : 1.0f;
    }
    channels.erase(std::remove_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
        return channel.semantic == VertexChannel::Semantic::Tangent;
    }), channels.end());
    channels.push_back(std::move(output));
    return true;
}
}

TangentRegenerationStatus regenerate_atlas_tangents(Primitive& primitive, std::string& error) {
    return regenerate_indexed_tangents(primitive, error);
}

TangentRegenerationStatus regenerate_indexed_tangents(Primitive& primitive, std::string& error) {
    const bool source_applicable = find(primitive.source_channels, VertexChannel::Semantic::Position) &&
        find(primitive.source_channels, VertexChannel::Semantic::Normal) && find(primitive.source_channels, VertexChannel::Semantic::Texcoord, 0);
    const bool current_applicable = find(primitive.channels, VertexChannel::Semantic::Position) &&
        find(primitive.channels, VertexChannel::Semantic::Normal) && find(primitive.channels, VertexChannel::Semantic::Texcoord, 0);
    if (!source_applicable && !current_applicable) return TangentRegenerationStatus::NotApplicable;
    if (source_applicable && !regenerate_channels(primitive.source_channels, primitive.indices, error)) return TangentRegenerationStatus::Invalid;
    if (current_applicable && !regenerate_channels(primitive.channels, primitive.indices, error)) return TangentRegenerationStatus::Invalid;
    return TangentRegenerationStatus::Regenerated;
}
}
