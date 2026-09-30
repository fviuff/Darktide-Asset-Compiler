#include "processing/attribute_canonicalizer.h"

#include <algorithm>
#include <set>

namespace dtglb::processing {
namespace {

int binding_texcoord(const TextureBinding& binding) {
    return binding.transform.texcoord >= 0 ? binding.transform.texcoord : binding.texcoord;
}

void add_binding_set(std::set<std::uint32_t>& sets, const TextureBinding& binding) {
    if (binding.texture >= 0 && binding_texcoord(binding) >= 0)
        sets.insert(static_cast<std::uint32_t>(binding_texcoord(binding)));
}

template <typename ChannelList>
std::size_t remove_unused_texcoords(ChannelList& channels, const std::set<std::uint32_t>& used_sets,
                                     bool preserve_set_zero) {
    const auto before = channels.size();
    channels.erase(std::remove_if(channels.begin(), channels.end(), [&](const VertexChannel& channel) {
        if (channel.semantic != VertexChannel::Semantic::Texcoord) return false;
        // The observed UNIT MeshGeometry profile has seven addressable UV sets.
        // Higher source sets cannot survive even when a stale material binding
        // still names them (automatic material lowering has already atlased such
        // bindings to set zero; explicit overrides do not consume source UVs).
        if (channel.set > 6u) return true;
        if (preserve_set_zero && channel.set == 0) return false;
        return !used_sets.count(channel.set);
    }), channels.end());
    return before - channels.size();
}

} // namespace

bool canonicalize_unused_attributes(Scene& scene, AttributeCanonicalizationReport& report,
                                    std::string& error) {
    report = {};
    error.clear();
    for (std::size_t primitive_index = 0; primitive_index < scene.primitives.size(); ++primitive_index) {
        auto& primitive = scene.primitives[primitive_index];
        std::set<std::uint32_t> used_texcoord_sets;
        bool external_material = false;
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size()) {
            const auto& material = scene.materials[static_cast<std::size_t>(primitive.material)];
            external_material = material.intent == MaterialInfo::Intent::External;
            add_binding_set(used_texcoord_sets, material.base_color_texture);
            add_binding_set(used_texcoord_sets, material.metallic_roughness_texture);
            add_binding_set(used_texcoord_sets, material.normal_texture);
            add_binding_set(used_texcoord_sets, material.occlusion_texture);
            add_binding_set(used_texcoord_sets, material.emissive_texture);
        }
        if (external_material) {
            if (!primitive.custom_attributes.empty()) {
                error = "primitive " + std::to_string(primitive_index) +
                    ": external material cannot preserve custom vertex attributes in the current UNIT mesh profile";
                return false;
            }
            const auto retain_native_uvs = [&](const std::vector<VertexChannel>& channels) {
                for (const auto& channel : channels) {
                    if (channel.semantic != VertexChannel::Semantic::Texcoord) continue;
                    if (channel.set > 6u) {
                        error = "primitive " + std::to_string(primitive_index) +
                            ": external material uses TEXCOORD set above the native 0..6 mesh profile";
                        return false;
                    }
                    used_texcoord_sets.insert(channel.set);
                }
                return true;
            };
            if (!retain_native_uvs(primitive.source_channels) || !retain_native_uvs(primitive.channels)) return false;
        }
        if (!primitive.custom_attributes.empty()) {
            const auto count = primitive.custom_attributes.size();
            primitive.custom_attributes.clear();
            report.changed = true;
            report.lossy = true;
            report.approximations.push_back("unused_custom_attribute");
            report.diagnostics.push_back({primitive_index, "unused_custom_attribute",
                "dropped " + std::to_string(count) + " custom attribute(s) with no current consumer"});
            scene.notes.push_back("primitive " + std::to_string(primitive_index) +
                ": dropped " + std::to_string(count) + " unused custom attribute(s) [unused_custom_attribute]");
        }

        const bool has_set_zero = [](const std::vector<VertexChannel>& channels) {
            return std::any_of(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 0;
            });
        }(primitive.source_channels) || [](const std::vector<VertexChannel>& channels) {
            return std::any_of(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 0;
            });
        }(primitive.channels);
        const bool had_out_of_profile_texcoord = [](const std::vector<VertexChannel>& channels) {
            return std::any_of(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set > 6u;
            });
        }(primitive.source_channels) || [](const std::vector<VertexChannel>& channels) {
            return std::any_of(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set > 6u;
            });
        }(primitive.channels);
        const auto removed_source = remove_unused_texcoords(primitive.source_channels, used_texcoord_sets, has_set_zero);
        const auto removed_current = remove_unused_texcoords(primitive.channels, used_texcoord_sets, has_set_zero);
        if (removed_source || removed_current) {
            report.changed = true;
            report.lossy = true;
            // The channels have already been removed; report the deterministic count at primitive scope.
            const char* code = had_out_of_profile_texcoord
                ? "out_of_profile_texcoord_set_dropped" : "unused_texcoord_set";
            const char* message = had_out_of_profile_texcoord
                ? "dropped TEXCOORD channel set(s) above the native 0..6 profile"
                : "dropped unreferenced TEXCOORD channel set(s)";
            report.approximations.push_back(code);
            report.diagnostics.push_back({primitive_index, code, message});
            scene.notes.push_back("primitive " + std::to_string(primitive_index) +
                ": " + message + " [" + code + "]");
        }
    }
    for (std::size_t node_index = 0; node_index < scene.nodes.size(); ++node_index) {
        auto& attributes = scene.nodes[node_index].instance_attributes;
        const auto before = attributes.size();
        attributes.erase(std::remove_if(attributes.begin(), attributes.end(), [](const CustomAttribute& attribute) {
            return !attribute.name.empty() && attribute.name.front() == '_';
        }), attributes.end());
        const auto removed = before - attributes.size();
        if (!removed) continue;
        report.changed = true;
        report.lossy = true;
        report.approximations.push_back("unused_instance_attribute");
        report.diagnostics.push_back({node_index, "unused_instance_attribute",
            "dropped " + std::to_string(removed) + " custom instance attribute(s) with no current consumer"});
        scene.notes.push_back("node " + std::to_string(node_index) + ": dropped " +
            std::to_string(removed) + " unused custom instance attribute(s) [unused_instance_attribute]");
    }
    return true;
}

} // namespace dtglb::processing
