#include "stingray/unit/unit_identity.h"

#include "stingray/murmur_hash.h"

#include <map>
#include <set>

namespace dtglb::stingray::unit {

std::vector<std::string> lower_unique_native_names(
    const std::vector<std::string>& authored_names,
    const std::vector<std::size_t>& stable_indices,
    const std::string& kind,
    const std::string& empty_prefix,
    const std::vector<std::uint32_t>& reserved_hashes,
    NativeNameHashDomain hash_domain) {
    const auto native_hash = [hash_domain](const std::string& value) {
        return hash_domain == NativeNameHashDomain::Id64High32 ? id32_from_id64(value) : id32(value);
    };
    std::vector<std::string> names;
    names.reserve(authored_names.size());
    std::map<std::string, std::size_t> counts;
    std::map<std::uint32_t, std::size_t> hash_counts;
    for (std::size_t i = 0; i < authored_names.size(); ++i) {
        const std::size_t stable_index = i < stable_indices.size() ? stable_indices[i] : i;
        const std::string name = authored_names[i].empty()
            ? empty_prefix + std::to_string(stable_index) : authored_names[i];
        names.push_back(name);
        ++counts[name];
        ++hash_counts[native_hash(name)];
    }

    std::set<std::uint32_t> used_hashes(reserved_hashes.begin(), reserved_hashes.end());
    std::set<std::uint32_t> preserved_hashes;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (counts[names[i]] == 1 && hash_counts[native_hash(names[i])] == 1 &&
            !used_hashes.count(native_hash(names[i]))) {
            preserved_hashes.insert(native_hash(names[i]));
            used_hashes.insert(native_hash(names[i]));
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::size_t stable_index = i < stable_indices.size() ? stable_indices[i] : i;
        if (counts[names[i]] == 1 && hash_counts[native_hash(names[i])] == 1 &&
            preserved_hashes.count(native_hash(names[i]))) {
            continue;
        }
        const std::string base = names[i] + "__" + kind + "_" + std::to_string(stable_index);
        std::string candidate = base;
        std::size_t attempt = 0;
        while (!used_hashes.insert(native_hash(candidate)).second) {
            candidate = base + "_" + std::to_string(++attempt);
        }
        names[i] = std::move(candidate);
    }
    return names;
}

MaterialSlotLowering lower_material_slots(const std::vector<std::string>& material_names,
                                           const std::vector<int>& primitive_material_indices) {
    MaterialSlotLowering result;
    result.material_slots.resize(material_names.size());
    std::set<std::size_t> used_indices;
    bool has_missing = false;
    for (const int index : primitive_material_indices) {
        if (index >= 0 && static_cast<std::size_t>(index) < material_names.size()) used_indices.insert(static_cast<std::size_t>(index));
        else has_missing = true;
    }
    std::vector<std::string> authored;
    std::vector<std::size_t> stable;
    std::vector<std::size_t> owners;
    for (const auto index : used_indices) {
        authored.push_back(material_names[index]); stable.push_back(index); owners.push_back(index);
    }
    const auto lowered = lower_unique_native_names(authored, stable, "material", "material_",
        has_missing ? std::vector<std::uint32_t>{id32_from_id64("__no_material__")} : std::vector<std::uint32_t>{},
        NativeNameHashDomain::Id64High32);
    for (std::size_t i = 0; i < lowered.size(); ++i) result.material_slots[owners[i]] = lowered[i];
    if (has_missing) result.missing_slot = "__no_material__";
    result.primitive_slots.reserve(primitive_material_indices.size());
    for (const int index : primitive_material_indices)
        result.primitive_slots.push_back(index >= 0 && static_cast<std::size_t>(index) < result.material_slots.size()
            ? result.material_slots[static_cast<std::size_t>(index)] : result.missing_slot);
    return result;
}

}
