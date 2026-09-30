#include "processing/uv1_material_canonicalizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dtglb::processing {
namespace {

int effective_texcoord(const TextureBinding& binding) {
    return binding.transform.texcoord >= 0 ? binding.transform.texcoord : binding.texcoord;
}

bool valid_uv1(const std::vector<VertexChannel>& channels) {
    const auto position = std::find_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
        return channel.semantic == VertexChannel::Semantic::Position;
    });
    const auto uv = std::find_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
        return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 1;
    });
    if (position == channels.end() || position->components < 3 || position->values.size() % position->components != 0 ||
        uv == channels.end() || uv->components != 2 || uv->values.size() != (position->values.size() / position->components) * 2)
        return false;
    return std::all_of(uv->values.begin(), uv->values.end(), [](float value) { return std::isfinite(value); });
}

void copy_uv1_to_uv0(std::vector<VertexChannel>& channels) {
    const auto uv1 = std::find_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
        return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 1;
    });
    auto uv0 = std::find_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
        return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 0;
    });
    VertexChannel replacement = *uv1;
    replacement.set = 0;
    if (uv0 == channels.end()) channels.push_back(std::move(replacement));
    else *uv0 = std::move(replacement);
}

void lower_binding(TextureBinding& binding) {
    if (binding.texture < 0 || effective_texcoord(binding) != 1) return;
    binding.texcoord = 0;
    if (binding.transform.texcoord >= 0) binding.transform.texcoord = 0;
}

} // namespace

void canonicalize_generated_uv1_materials(Scene& scene) {
    for (auto& material : scene.materials) {
        if (material.intent != MaterialInfo::Intent::Generated) continue;

        const std::vector<TextureBinding*> bindings{
            &material.base_color_texture,
            &material.metallic_roughness_texture,
            &material.normal_texture,
            &material.occlusion_texture,
            &material.emissive_texture,
            &material.diffuse_texture,
            &material.specular_glossiness_texture,
            &material.transmission_texture,
        };
        bool has_texture = false;
        bool uv1_only = true;
        for (const auto* binding : bindings) {
            if (binding->texture < 0) continue;
            has_texture = true;
            if (effective_texcoord(*binding) != 1) uv1_only = false;
        }
        if (!has_texture || !uv1_only) continue;

        std::vector<Primitive*> uses;
        bool coherent_geometry = true;
        for (auto& primitive : scene.primitives) {
            if (primitive.material < 0 || static_cast<std::size_t>(primitive.material) >= scene.materials.size() ||
                &scene.materials[static_cast<std::size_t>(primitive.material)] != &material)
                continue;
            if (!valid_uv1(primitive.channels) || !valid_uv1(primitive.source_channels)) {
                coherent_geometry = false;
                break;
            }
            uses.push_back(&primitive);
        }
        if (!coherent_geometry || uses.empty()) continue;

        for (auto* primitive : uses) {
            copy_uv1_to_uv0(primitive->channels);
            copy_uv1_to_uv0(primitive->source_channels);
            if (material.normal_texture.texture >= 0) {
                auto clear_tangents = [](std::vector<VertexChannel>& channels) {
                    channels.erase(std::remove_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                        return channel.semantic == VertexChannel::Semantic::Tangent;
                    }), channels.end());
                };
                clear_tangents(primitive->channels);
                clear_tangents(primitive->source_channels);
            }
        }
        for (auto* binding : bindings) lower_binding(*binding);
        scene.notes.push_back("material " + material.name +
            ": copied TEXCOORD_1 to TEXCOORD_0 and retargeted its generated texture bindings; unused TEXCOORD_1 may be removed [uv1_to_uv0]");
    }
}

} // namespace dtglb::processing
