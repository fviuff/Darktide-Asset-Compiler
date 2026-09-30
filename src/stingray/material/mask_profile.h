#pragma once

#include "stingray/material/material_v61.h"

#include <array>
#include <cstdint>
#include <string>

namespace dtglb::stingray::material {

struct MaskProfileSpec {
    std::array<std::uint64_t, 3> texture_hashes{}; // BCA, normal, ORM
    float alpha_cutoff = 0.5f;
    std::string surface_material = "default";
};

std::uint64_t mask_shader_provider_material_hash();
std::uint64_t mask_parent_material_hash();
bool build_mask_profile(const MaskProfileSpec& spec, MaterialStream& out, std::string& error);

} // namespace dtglb::stingray::material
