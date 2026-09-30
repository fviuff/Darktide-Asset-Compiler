#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::unit {

enum class NativeNameHashDomain { LegacyId32, Id64High32 };

struct MaterialSlotLowering {
    std::vector<std::string> primitive_slots;
    std::vector<std::string> material_slots;
    std::string missing_slot;
};

// Lower authored names into the native UNIT name domain. The authored names
// remain owned by Scene; only names written into the UNIT are changed.
std::vector<std::string> lower_unique_native_names(
    const std::vector<std::string>& authored_names,
    const std::vector<std::size_t>& stable_indices,
    const std::string& kind,
    const std::string& empty_prefix,
    const std::vector<std::uint32_t>& reserved_hashes = {},
    NativeNameHashDomain hash_domain = NativeNameHashDomain::LegacyId32);

MaterialSlotLowering lower_material_slots(
    const std::vector<std::string>& material_names,
    const std::vector<int>& primitive_material_indices);

}
