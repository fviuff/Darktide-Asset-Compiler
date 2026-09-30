#pragma once
#include <string>
#include <vector>

namespace dtglb { struct SkinInfo; }

namespace dtglb::stingray::bones {
// Ordered identities shared by BONES and the corresponding UNIT joint nodes.
// Source joint indices and names are never modified.
std::vector<std::string> canonical_bone_names(const SkinInfo& skin);
}
