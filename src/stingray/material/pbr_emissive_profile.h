#pragma once

#include "stingray/material/material_v61.h"

#include <array>
#include <cstdint>
#include <string>

namespace dtglb::stingray::material {

// Native inherited profile evidenced by the current PBR emissive material
// corpus. This describes the observed stream layout; it does not select a
// profile automatically for glTF materials.
struct PbrEmissiveProfileSpec {
    std::array<std::uint64_t, 4> texture_hashes{}; // base color, emissive, normal, ORM
    float multiplier = 1.0f;
    std::array<float, 3> color{1.0f, 0.03f, 0.0f};
    float intensity = 1.0f;
    std::string surface_material = "metal_sheet";
};

std::uint64_t pbr_emissive_shader_provider_material_hash();
std::uint64_t pbr_emissive_parent_material_hash();
bool build_pbr_emissive_profile(const PbrEmissiveProfileSpec& spec, MaterialStream& out, std::string& error);

} // namespace dtglb::stingray::material
