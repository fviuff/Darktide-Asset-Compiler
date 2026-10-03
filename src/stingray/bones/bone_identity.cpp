#include "stingray/bones/bone_identity.h"
#include "stingray/unit/unit_identity.h"
#include "scene/scene.h"
#include <cctype>
#include <set>

namespace dtglb::stingray::bones {
std::vector<std::string> canonical_bone_names(const SkinInfo& skin) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    names.reserve(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        std::string name = i < skin.joint_names.size() ? skin.joint_names[i] : "joint_" + std::to_string(i);
        // Preserve the existing BONES spelling policy and collision-free bytes.
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
        if (name.empty()) name = "asset";
        const std::string base = name;
        std::size_t suffix = 1;
        while (!seen.insert(name).second) name = base + "_" + std::to_string(suffix++);
        names.push_back(std::move(name));
    }
    return unit::lower_unique_native_names(names, {}, "bone", "joint_");
}
}
