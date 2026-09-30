#include "processing/scene_scaler.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dtglb::processing {
namespace {

void scale_position_channels(std::vector<VertexChannel>& channels, float scale) {
    for (auto& channel : channels) {
        if (channel.semantic != VertexChannel::Semantic::Position || channel.components < 3) continue;
        for (std::size_t row = 0; row + 2 < channel.values.size(); row += channel.components) {
            channel.values[row] *= scale;
            channel.values[row + 1] *= scale;
            channel.values[row + 2] *= scale;
        }
    }
}

void recompute_bounds(const std::vector<VertexChannel>& channels,
                      std::array<float, 3>& minimum, std::array<float, 3>& maximum,
                      float& radius) {
    const VertexChannel* position = nullptr;
    for (const auto& channel : channels) {
        if (channel.semantic == VertexChannel::Semantic::Position && channel.components >= 3) {
            position = &channel;
            break;
        }
    }
    if (!position || position->values.size() < 3) return;

    minimum = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
               std::numeric_limits<float>::infinity()};
    maximum = {-minimum[0], -minimum[1], -minimum[2]};
    float radius_squared = 0.0f;
    for (std::size_t row = 0; row + 2 < position->values.size(); row += position->components) {
        for (std::size_t component = 0; component < 3; ++component) {
            minimum[component] = std::min(minimum[component], position->values[row + component]);
            maximum[component] = std::max(maximum[component], position->values[row + component]);
        }
        const float x = position->values[row];
        const float y = position->values[row + 1];
        const float z = position->values[row + 2];
        radius_squared = std::max(radius_squared, x * x + y * y + z * z);
    }
    radius = std::sqrt(radius_squared);
}

void scale_matrix_translation(Matrix4& matrix, float scale) {
    matrix[12] *= scale;
    matrix[13] *= scale;
    matrix[14] *= scale;
}

} // namespace

bool scale_scene(Scene& scene, float scale, std::string& error) {
    if (!std::isfinite(scale) || scale <= 0.0f) {
        error = "scene scale must be finite and positive";
        return false;
    }
    error.clear();

    for (auto* primitives : {&scene.primitives, &scene.collider_primitives})
    for (auto& primitive : *primitives) {
        scale_position_channels(primitive.source_channels, scale);
        scale_position_channels(primitive.channels, scale);
        for (auto& target : primitive.morph_targets) scale_position_channels(target.channels, scale);
        recompute_bounds(primitive.source_channels, primitive.source_bounds_min,
                         primitive.source_bounds_max, primitive.source_bounds_radius);
        recompute_bounds(primitive.channels, primitive.bounds_min, primitive.bounds_max,
                         primitive.bounds_radius);
    }

    for (auto& node : scene.nodes) {
        scale_matrix_translation(node.local_gltf, scale);
        scale_matrix_translation(node.world_gltf, scale);
        scale_matrix_translation(node.local_stingray, scale);
        scale_matrix_translation(node.world_stingray, scale);
        for (float& value : node.local_translation_stingray) value *= scale;
    }

    for (auto& skin : scene.skins) {
        for (auto& matrix : skin.inverse_bind_matrices_gltf) scale_matrix_translation(matrix, scale);
        for (auto& matrix : skin.inverse_bind_matrices_stingray) scale_matrix_translation(matrix, scale);
    }

    for (auto& animation : scene.animations) {
        for (auto& track : animation.tracks) {
            if (track.path != AnimationPath::Translation) continue;
            for (float& value : track.values) value *= scale;
        }
    }

    for (auto& camera : scene.cameras) {
        camera.znear *= scale;
        camera.zfar *= scale;
        camera.xmag *= scale;
        camera.ymag *= scale;
    }
    for (auto& light : scene.lights) light.range *= scale;
    return true;
}

} // namespace dtglb::processing
