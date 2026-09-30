#include "stingray/resource_name.h"
#include "compiler/resource_graph.h"
#include "stingray/murmur_hash.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace dtglb::compiler {
namespace {
std::string label(const ResourceKey& key) { return key.type + ":" + key.name; }
std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
}

bool portable_payload_path(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') return false;
    for (const unsigned char c : path)
        if (c < 32 || c >= 127 || std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) return false;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        const auto stem = lower(part.substr(0, part.find('.')));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

std::string resource_stream_name(const OwnedResourceNode& node) {
    return resource_stream_name(node, darktide_target_profile());
}

std::string resource_stream_name(const OwnedResourceNode& node, const TargetProfile& profile) {
    const auto* contract = profile.storage_contract(node.key.type);
    if (!contract) return {};
    switch (contract->sidecar) {
    case TargetSidecarStrategy::HashedTextureStream:
        return stingray::texture::make_texture_stream_name(node.key.name);
    case TargetSidecarStrategy::HashedMaterialStream: {
        std::ostringstream stream;
        stream << "data/am/" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
               << stingray::id64("stream:" + node.key.name);
        return stream.str();
    }
    case TargetSidecarStrategy::None:
        return {};
    }
    return {};
}

bool validate_graph(const ResourceGraph& graph, ResourceClosure& closure, std::string& error,
                    const std::vector<std::string>& reserved_payloads) {
    return validate_graph(graph, closure, error, darktide_target_profile(), reserved_payloads);
}

bool validate_graph(const ResourceGraph& graph, ResourceClosure& closure, std::string& error,
                    const TargetProfile& profile, const std::vector<std::string>& reserved_payloads) {
    closure = {};
    std::map<ResourceKey, std::size_t> owned;
    std::map<ResourceKey, bool> external;
    std::map<std::pair<std::uint64_t, std::uint64_t>, ResourceKey> hashes;
    std::set<std::string> files{"build.json", "compile_manifest.json", "compile_diagnostic.json"};
    auto check_key = [&](const ResourceKey& key) {
        if (!key.valid()) { error = "invalid resource key: " + label(key); return false; }
        const auto native = std::make_pair(stingray::id64(key.type), stingray::resource_name_hash(key.name));
        const auto [it, added] = hashes.emplace(native, key);
        if (!added && it->second != key) { error = "native resource hash collision: " + label(key); return false; }
        return true;
    };
    auto check_file = [&](const std::string& file) {
        if (!portable_payload_path(file)) { error = "invalid resource payload path: " + file; return false; }
        const auto normalized = lower(file);
        if (!files.insert(normalized).second) { error = "duplicate resource payload path: " + file; return false; }
        return true;
    };
    for (const auto& file : reserved_payloads) if (!check_file(file)) return false;
    constexpr const char* types[] = {"unit", "bones", "animation", "material", "material", "texture", "state_machine"};
    for (std::size_t i = 0; i < graph.owned.size(); ++i) {
        const auto& node = graph.owned[i];
        if (!check_key(node.key)) return false;
        if (!owned.emplace(node.key, i).second) { error = "duplicate owned resource: " + label(node.key); return false; }
        if (node.resource.valueless_by_exception() || node.resource.index() >= sizeof(types) / sizeof(types[0]) ||
            node.key.type != types[node.resource.index()]) {
            error = "resource key does not match native resource type: " + label(node.key); return false;
        }
        if (!check_file(node.file)) return false;
        const auto stream = resource_stream_name(node, profile);
        if (!stream.empty() && !check_file(stream)) return false;
        if (const auto* bones = std::get_if<stingray::bones::BonesResource>(&node.resource);
            bones && bones->resource_name != node.key.name) {
            error = "BONES identity differs from graph key"; return false;
        }
        if (const auto* animation = std::get_if<stingray::animation::BuiltAnimation>(&node.resource);
            animation && animation->resource_name != node.key.name) {
            error = "ANIMATION identity differs from graph key"; return false;
        }
        if (std::holds_alternative<std::vector<std::uint8_t>>(node.resource) && node.key.type != "state_machine") {
            error = "STATE_MACHINE bytes have a mismatched graph key"; return false;
        }
    }
    // File-prefix aliases also conflict (a payload cannot be another payload's directory).
    for (const auto& file : files)
        for (auto slash = file.find('/'); slash != std::string::npos; slash = file.find('/', slash + 1))
            if (files.count(file.substr(0, slash))) { error = "payload file/directory collision: " + file; return false; }
    for (const auto& ref : graph.external) {
        if (!check_key(ref.key)) return false;
        if (owned.count(ref.key)) { error = "resource is both owned and external: " + label(ref.key); return false; }
        const auto [it, added] = external.emplace(ref.key, ref.package_member);
        if (!added && it->second != ref.package_member) { error = "conflicting external package policy: " + label(ref.key); return false; }
    }
    const auto exists = [&](const ResourceKey& key) { return owned.count(key) || external.count(key); };
    for (const auto& node : graph.owned) {
        for (const auto& dependency : node.dependencies)
            if (!exists(dependency)) { error = "unresolved dependency " + label(dependency) + " from " + label(node.key); return false; }
        if (const auto* material = std::get_if<stingray::material::MaterialStream>(&node.resource)) {
            const auto has_dependency = [&](const char* type, std::uint64_t hash) {
                return !hash || std::any_of(node.dependencies.begin(), node.dependencies.end(), [&](const auto& key) {
                    return key.type == type && stingray::resource_name_hash(key.name) == hash;
                });
            };
            if (!has_dependency("material", material->shader_provider_material_hash)) {
                error = "MATERIAL shader provider is absent from graph dependencies"; return false;
            }
            if (!has_dependency("material", material->parent_material_hash)) {
                error = "MATERIAL parent is absent from graph dependencies"; return false;
            }
            for (const auto& texture : material->textures)
                if (!has_dependency("texture", texture.resource_hash)) { error = "MATERIAL texture is absent from graph dependencies"; return false; }
        } else if (const auto* preserved = std::get_if<stingray::material::PreservedMaterialStream>(&node.resource)) {
            stingray::material::MaterialDependencyHashes dependencies;
            if (!stingray::material::validate_preserved_material_v61(preserved->bytes, dependencies, error)) return false;
            const auto has_dependency = [&](const char* type, std::uint64_t hash) {
                return !hash || std::any_of(node.dependencies.begin(), node.dependencies.end(), [&](const auto& key) {
                    return key.type == type && stingray::resource_name_hash(key.name) == hash;
                });
            };
            if (!has_dependency("material", dependencies.shader_provider_material_hash)) {
                error = "MATERIAL shader provider is absent from graph dependencies"; return false;
            }
            if (!has_dependency("material", dependencies.parent_material_hash)) {
                error = "MATERIAL parent is absent from graph dependencies"; return false;
            }
            for (const auto hash : dependencies.texture_resource_hashes)
                if (!has_dependency("texture", hash)) { error = "MATERIAL texture is absent from graph dependencies"; return false; }
        } else if (std::holds_alternative<std::vector<std::uint8_t>>(node.resource)) {
            if (node.dependencies.empty() || std::any_of(node.dependencies.begin(), node.dependencies.end(),
                    [](const ResourceKey& dependency) { return dependency.type != "animation"; })) {
                error = "STATE_MACHINE must depend on one or more ANIMATION resources"; return false;
            }
        } else if (std::holds_alternative<stingray::unit::UnitResource>(node.resource)) {
            for (const auto& dependency : node.dependencies) {
                if (dependency.type == "state_machine" && dependency.name != node.key.name) {
                    error = "UNIT and STATE_MACHINE must share one resource identity"; return false;
                }
            }
        }
    }
    if (graph.roots.empty()) { error = "resource graph has no requested roots"; return false; }
    std::set<ResourceKey> reached;
    std::vector<ResourceKey> pending = graph.roots;
    while (!pending.empty()) {
        auto key = std::move(pending.back()); pending.pop_back();
        if (!exists(key)) { error = "unresolved graph root: " + label(key); return false; }
        if (!reached.insert(key).second) continue;
        if (const auto it = owned.find(key); it != owned.end()) {
            const auto& dependencies = graph.owned[it->second].dependencies;
            pending.insert(pending.end(), dependencies.begin(), dependencies.end());
        }
    }
    std::map<ResourceKey, std::size_t> remaining;
    std::map<ResourceKey, std::vector<ResourceKey>> dependents;
    std::set<ResourceKey> ready, members;
    for (const auto& key : reached) {
        if (const auto it = owned.find(key); it != owned.end()) {
            auto& count = remaining[key];
            const auto& deps = graph.owned[it->second].dependencies;
            for (const auto& dep : std::set<ResourceKey>(deps.begin(), deps.end()))
                if (owned.count(dep)) { ++count; dependents[dep].push_back(key); }
            if (!count) ready.insert(key);
            members.insert(key);
        } else {
            closure.external.push_back({key, external.at(key)});
            if (external.at(key)) members.insert(key);
        }
    }
    while (!ready.empty()) {
        const auto key = *ready.begin(); ready.erase(ready.begin());
        closure.owned.push_back(owned.at(key));
        for (const auto& next : dependents[key]) if (--remaining[next] == 0) ready.insert(next);
    }
    if (closure.owned.size() != remaining.size()) { error = "cycle in requested resource dependencies"; closure = {}; return false; }
    closure.package_members.assign(members.begin(), members.end());
    return true;
}
}
