#include "stingray/material/pbr_emissive_profile.h"

#include <cmath>

namespace dtglb::stingray::material {
namespace {
constexpr std::uint64_t kShaderProviderMaterial = 0xeba5d203d733a235ull;
constexpr std::uint64_t kParentMaterial = 0xe04dba68a38d1429ull;
constexpr std::uint32_t kBaseColor = 0x8e604079u;
constexpr std::uint32_t kEmissive = 0x5f9dfbfbu;
constexpr std::uint32_t kNormal = 0xb45e95f3u;
constexpr std::uint32_t kOrm = 0x8655079cu;
constexpr std::uint32_t kIntensity = 0x32f447e5u;
constexpr std::uint32_t kEmissiveColor = 0xc985395au;
constexpr std::uint32_t kMultiplier = 0x519c1733u;

void append_float(std::vector<std::uint8_t>& data, float value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(value));
}
}

std::uint64_t pbr_emissive_shader_provider_material_hash() { return kShaderProviderMaterial; }
std::uint64_t pbr_emissive_parent_material_hash() { return kParentMaterial; }

bool build_pbr_emissive_profile(const PbrEmissiveProfileSpec& spec, MaterialStream& out, std::string& error) {
    for (const auto hash : spec.texture_hashes) {
        if (!hash) {
            error = "PBR emissive profile texture hashes must be nonzero";
            return false;
        }
    }
    if (!std::isfinite(spec.multiplier) || spec.multiplier < 0.0f || !std::isfinite(spec.intensity) || spec.intensity < 0.0f) {
        error = "PBR emissive multiplier and intensity must be finite and nonnegative";
        return false;
    }
    for (const float channel : spec.color) {
        if (!std::isfinite(channel) || channel < 0.0f) {
            error = "PBR emissive color channels must be finite and nonnegative";
            return false;
        }
    }

    InheritedMaterialSpec material;
    material.shader_provider_material_hash = pbr_emissive_shader_provider_material_hash();
    material.parent_material_hash = pbr_emissive_parent_material_hash();
    material.surface_material = spec.surface_material;
    material.textures = {
        {kBaseColor, spec.texture_hashes[0]},
        {kEmissive, spec.texture_hashes[1]},
        {kNormal, spec.texture_hashes[2]},
        {kOrm, spec.texture_hashes[3]},
    };
    material.variables = {
        {0, 0, kIntensity, 32, 0},
        {2, 0, kEmissiveColor, 20, 0},
        {0, 0, kMultiplier, 16, 0},
    };
    material.variable_data.reserve(36);
    append_float(material.variable_data, 0.5f);
    append_float(material.variable_data, 0.0f);
    append_float(material.variable_data, 0.0f);
    append_float(material.variable_data, 0.0f);
    append_float(material.variable_data, spec.multiplier);
    append_float(material.variable_data, spec.color[0]);
    append_float(material.variable_data, spec.color[1]);
    append_float(material.variable_data, spec.color[2]);
    append_float(material.variable_data, spec.intensity);
    return build_inherited_material(material, out, error);
}
} // namespace dtglb::stingray::material
