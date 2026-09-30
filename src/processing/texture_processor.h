#pragma once
#include "scene/scene.h"
#include "stingray/texture/texture_writer.h"
#include <array>
#include <string>
#include <optional>

namespace dtglb::processing {
enum class ConstantColorBakeStatus { Applied, NotApplicable };
enum class AtlasBakeStatus { Applied, NotApplicable };
struct VertexColorCanonicalizationReport {
    bool changed = false;
    bool constant_applied = false;
    bool atlas_applied = false;
    bool direct_specgloss_applied = false;
    bool explicit_drop = false;
    bool higher_set_drop = false;
    bool sampler_filter_fallback = false;
    bool atlas_capacity_fallback = false;
    std::vector<std::string> approximations;
    std::vector<std::string> diagnostics;
};

struct CanonicalAtlasResult {
    AtlasBakeStatus status = AtlasBakeStatus::NotApplicable;
    bool sampler_filter_fallback = false;
    bool missing_texcoord_fallback = false;
    bool uv_overflow_clamped = false;
    bool color_range_clamped = false;
    Primitive primitive;
    MaterialInfo material;
    stingray::texture::ImageRGBA image;
    std::optional<stingray::texture::ImageRGBA> metallic_roughness;
    std::optional<stingray::texture::ImageRGBA> occlusion;
    std::optional<stingray::texture::ImageRGBA> normal;
    std::optional<stingray::texture::ImageRGBA> emission;
};
bool fold_constant_color0(const Primitive&, const std::array<float, 4>&, std::array<float, 4>&, ConstantColorBakeStatus&, std::string&);
bool remove_consumed_color0(const Primitive&, Primitive&, std::string&);

// Canonicalize only finite constant COLOR_0 values. The caller must invoke
// this only for automatic material generation without an explicit override.
bool canonicalize_constant_color0(Scene&, bool& applied, std::string& error);
bool canonicalize_varying_color0(const Scene&, std::size_t primitive_index,
                                 CanonicalAtlasResult&, std::string& error);
bool canonicalize_vertex_colors(Scene&, bool automatic_materials,
                                VertexColorCanonicalizationReport&, std::string& error);
} // namespace dtglb::processing
