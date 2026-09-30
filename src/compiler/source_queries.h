#pragma once
#include "scene/scene.h"
#include <set>

namespace dtglb::compiler {
std::string safe_name(std::string s);
std::vector<std::string> canonical_bone_names(const SkinInfo& skin);
std::vector<std::size_t> used_skin_indices(const Scene& s);
std::vector<std::size_t> used_material_indices(const Scene& scene);
std::set<int> active_node_indices(const Scene& scene);
std::vector<std::size_t> active_node_skin_indices(const Scene& scene);
}
