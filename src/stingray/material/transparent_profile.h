#pragma once

#include "stingray/material/material_v61.h"

#include <array>
#include <cstdint>
#include <string>

namespace dtglb::stingray::material {

struct TransparentProfileSpec {
    std::array<std::uint64_t, 3> texture_hashes{}; // BCA, normal, ORM
    std::string surface_material = "default";
};

std::uint64_t transparent_shader_provider_material_hash();
std::uint64_t transparent_parent_material_hash();
bool build_transparent_profile(const TransparentProfileSpec& spec, MaterialStream& out, std::string& error);

} // namespace dtglb::stingray::material
