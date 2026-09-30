#include "stingray/material/emissive_profile.h"

#include <cmath>

namespace dtglb::stingray::material {
namespace {
constexpr std::uint64_t kEmissiveShaderProviderMaterial = 0xebd773be991fab5cull;
constexpr std::uint32_t kEmissiveIntensityLumen = 0xf4da0976u;
constexpr std::uint32_t kMultiplier = 0x519c1733u;
constexpr std::uint32_t kColor = 0xc985395au;

void append_float(std::vector<std::uint8_t>& data, float value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(value));
}
}

std::uint64_t emissive_shader_provider_material_hash() { return kEmissiveShaderProviderMaterial; }

bool build_emissive_profile(const EmissiveProfileSpec& spec, MaterialStream& out, std::string& error) {
    if (!std::isfinite(spec.intensity) || spec.intensity < 0.0f) {
        error = "emissive intensity must be finite and nonnegative";
        return false;
    }
    if (!std::isfinite(spec.multiplier) || spec.multiplier < 0.0f) {
        error = "emissive multiplier must be finite and nonnegative";
        return false;
    }
    for (const float channel : spec.color) {
        if (!std::isfinite(channel) || channel < 0.0f) {
            error = "emissive color channels must be finite and nonnegative";
            return false;
        }
    }

    InheritedMaterialSpec material;
    material.shader_provider_material_hash = emissive_shader_provider_material_hash();
    material.parent_material_hash = 0;
    material.surface_material = spec.surface_material;
    material.variables = {
        {0, 0, kEmissiveIntensityLumen, 0, 0},
        {0, 0, kMultiplier, 4, 0},
        {2, 0, kColor, 8, 0},
    };
    material.variable_data.reserve(20);
    append_float(material.variable_data, spec.intensity);
    append_float(material.variable_data, spec.multiplier);
    append_float(material.variable_data, spec.color[0]);
    append_float(material.variable_data, spec.color[1]);
    append_float(material.variable_data, spec.color[2]);
    return build_inherited_material(material, out, error);
}
} // namespace dtglb::stingray::material
