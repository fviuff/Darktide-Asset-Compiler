#include "stingray/material/mask_profile.h"

#include <cmath>

namespace dtglb::stingray::material {
namespace {
constexpr std::uint64_t kShaderProviderMaterial = 0xe03ea781c4162268ull;
constexpr std::uint64_t kParentMaterial = 0x345c8a27a643dc8eull;
constexpr std::uint32_t kBca = 0x2fadcc8cu;
constexpr std::uint32_t kNormal = 0xb45e95f3u;
constexpr std::uint32_t kOrm = 0x8655079cu;
constexpr std::uint32_t kAlphaCutoff = 0x53c84707u;
constexpr std::uint32_t kMaskValue = 0x2005c3e6u;
constexpr std::uint32_t kMaskScale = 0xed4bd4a3u;

void append_float(std::vector<std::uint8_t>& data, float value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(value));
}
} // namespace

std::uint64_t mask_shader_provider_material_hash() { return kShaderProviderMaterial; }
std::uint64_t mask_parent_material_hash() { return kParentMaterial; }

bool build_mask_profile(const MaskProfileSpec& spec, MaterialStream& out, std::string& error) {
    for (const auto hash : spec.texture_hashes) {
        if (!hash) {
            error = "MASK profile texture hashes must be nonzero";
            return false;
        }
    }
    if (!std::isfinite(spec.alpha_cutoff) || spec.alpha_cutoff < 0.0f || spec.alpha_cutoff > 1.0f) {
        error = "MASK alpha cutoff must be finite and within [0,1]";
        return false;
    }

    InheritedMaterialSpec material;
    material.shader_provider_material_hash = mask_shader_provider_material_hash();
    material.parent_material_hash = mask_parent_material_hash();
    material.surface_material = spec.surface_material;
    material.textures = {
        {kBca, spec.texture_hashes[0]},
        {kNormal, spec.texture_hashes[1]},
        {kOrm, spec.texture_hashes[2]},
    };
    material.variables = {
        {0, 0, kAlphaCutoff, 0, 0},
        {0, 0, kMaskValue, 4, 0},
        {0, 0, kMaskScale, 8, 0},
    };
    material.variable_data.reserve(12);
    append_float(material.variable_data, spec.alpha_cutoff);
    append_float(material.variable_data, 1.0f);
    append_float(material.variable_data, 1.0f);
    return build_inherited_material(material, out, error);
}
} // namespace dtglb::stingray::material
