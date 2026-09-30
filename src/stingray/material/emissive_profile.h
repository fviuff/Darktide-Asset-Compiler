#pragma once

#include "stingray/material/material_v61.h"

namespace dtglb::stingray::material {

// Bounded native child profile evidenced by the fluorescent material sample.
struct EmissiveProfileSpec {
    float intensity = 1.0f;
    float multiplier = 1.0f;
    std::array<float, 3> color{1.0f, 0.4f, 0.05f};
    std::string surface_material = "plastic";
};

std::uint64_t emissive_shader_provider_material_hash();
bool build_emissive_profile(const EmissiveProfileSpec& spec, MaterialStream& out, std::string& error);

} // namespace dtglb::stingray::material
