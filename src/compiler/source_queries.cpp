#include "compiler/source_queries.h"
#include "stingray/bones/bone_identity.h"
#include <algorithm>
#include <cctype>

namespace dtglb::compiler {
std::string safe_name(std::string s) {
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    }
    if (s.empty()) s = "asset";
    return s;
}

std::vector<std::string> canonical_bone_names(const SkinInfo& skin) {
    return stingray::bones::canonical_bone_names(skin);
}

std::vector<std::size_t> used_skin_indices(const Scene& s) {
    std::vector<std::size_t> used;
    for (const auto& primitive : s.primitives) {
        if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= s.nodes.size()) continue;
        const int skin = s.nodes[static_cast<std::size_t>(primitive.source_node)].skin;
        if (skin < 0 || static_cast<std::size_t>(skin) >= s.skins.size()) continue;
        const auto index = static_cast<std::size_t>(skin);
        if (std::find(used.begin(), used.end(), index) == used.end()) used.push_back(index);
    }
    return used;
}

std::vector<std::size_t> used_material_indices(const Scene& scene) {
    std::vector<std::size_t> used;
    for (const auto& primitive : scene.primitives) {
        if (primitive.material < 0 || static_cast<std::size_t>(primitive.material) >= scene.materials.size()) continue;
        const auto index = static_cast<std::size_t>(primitive.material);
        if (std::find(used.begin(), used.end(), index) == used.end()) used.push_back(index);
    }
    return used;
}

std::set<int> active_node_indices(const Scene& scene) {
    std::set<int> active;
    std::vector<int> pending(scene.scene_roots.begin(), scene.scene_roots.end());
    while (!pending.empty()) {
        const int index = pending.back();
        pending.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size() || !active.insert(index).second) continue;
        const auto& children = scene.nodes[static_cast<std::size_t>(index)].children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return active;
}

std::vector<std::size_t> active_node_skin_indices(const Scene& scene) {
    std::vector<std::size_t> skins;
    for (const int node_index : active_node_indices(scene)) {
        const int skin = scene.nodes[static_cast<std::size_t>(node_index)].skin;
        if (skin < 0 || static_cast<std::size_t>(skin) >= scene.skins.size()) continue;
        const auto index = static_cast<std::size_t>(skin);
        if (std::find(skins.begin(), skins.end(), index) == skins.end()) skins.push_back(index);
    }
    return skins;
}

}
