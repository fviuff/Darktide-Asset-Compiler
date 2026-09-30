#pragma once
#include <cstdint>
#include <vector>

namespace dtglb::stingray::unit {
// Prepared v115 native sections. Keeping the packed body here preserves the
// established producer while decoupling publication from the GLB frontend.
struct UnitResource { std::vector<std::uint8_t> body; };
}
