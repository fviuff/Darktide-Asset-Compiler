#include "processing/back_faces.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace dtglb::processing {
namespace {

using Semantic = VertexChannel::Semantic;

// Mirror every vertex in place: normals negated, tangent w negated.
void mirror_channels(std::vector<VertexChannel>& channels) {
    for (auto& channel : channels) {
        if (channel.components == 0) continue;
        for (std::size_t i = 0; i < channel.values.size(); ++i) {
            const std::size_t component = i % channel.components;
            if ((channel.semantic == Semantic::Normal && component < 3) ||
                (channel.semantic == Semantic::Tangent && component == 3))
                channel.values[i] = -channel.values[i];
        }
    }
}

} // namespace

std::size_t add_back_faces(Scene& scene, std::string& error) {
    std::vector<Primitive> back_faces;
    for (const auto& primitive : scene.primitives) {
        if (primitive.material < 0 || static_cast<std::size_t>(primitive.material) >= scene.materials.size() ||
            !scene.materials[static_cast<std::size_t>(primitive.material)].double_sided)
            continue;
        if (primitive.mode != 4 || primitive.indices.size() % 3 != 0) {
            error = "double-sided primitive '" + primitive.name + "' is not a triangle list";
            return 0;
        }
        // a mesh of its own beside the front faces, so neither grows past the per-mesh vertex limit
        Primitive back = primitive;
        back.name = primitive.name + "_back";
        mirror_channels(back.channels);
        mirror_channels(back.source_channels);
        for (auto& target : back.morph_targets) mirror_channels(target.channels);
        for (std::size_t k = 0; k + 2 < back.indices.size(); k += 3) std::swap(back.indices[k + 1], back.indices[k + 2]);
        back_faces.push_back(std::move(back));
    }
    const std::size_t added = back_faces.size();
    for (auto& back : back_faces) scene.primitives.push_back(std::move(back));
    return added;
}

} // namespace dtglb::processing
