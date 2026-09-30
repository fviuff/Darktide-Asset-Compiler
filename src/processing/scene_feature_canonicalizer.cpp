#include "processing/scene_feature_canonicalizer.h"

#include <set>

namespace dtglb::processing {
namespace {

std::vector<int> active_nodes(const Scene& scene) {
    std::vector<int> result;
    std::set<int> visited;
    std::vector<int> pending(scene.scene_roots.begin(), scene.scene_roots.end());
    while (!pending.empty()) {
        const int index = pending.back();
        pending.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size() || !visited.insert(index).second) continue;
        result.push_back(index);
        for (const int child : scene.nodes[static_cast<std::size_t>(index)].children) pending.push_back(child);
    }
    return result;
}

void note(Scene& scene, SceneFeatureCanonicalizationReport& report, std::size_t index,
          const char* code, const std::string& message) {
    report.changed = true;
    report.lossy = true;
    report.approximations.emplace_back(code);
    report.diagnostics.push_back({index, code, message});
    scene.notes.push_back(message + " [" + code + "]");
}

} // namespace

bool canonicalize_scene_features(Scene& scene, SceneFeatureCanonicalizationReport& report,
                                 std::string& error) {
    report = {};
    error.clear();

    for (const int node_index : active_nodes(scene)) {
        auto& node = scene.nodes[static_cast<std::size_t>(node_index)];
        if (node.camera >= 0) {
            node.camera = -1;
            note(scene, report, static_cast<std::size_t>(node_index), "camera_ignored",
                 "node " + std::to_string(node_index) + ": ignored active camera attachment");
        }
        if (node.light >= 0) {
            node.light = -1;
            note(scene, report, static_cast<std::size_t>(node_index), "light_ignored",
                 "node " + std::to_string(node_index) + ": ignored active punctual light attachment");
        }
        if (node.skin >= 0 && !node.instance_attributes.empty()) {
            for (auto& primitive : scene.primitives) {
                if (primitive.source_node == node_index) primitive.source_node = -1;
            }
            node.skin = -1;
            node.instance_attributes.clear();
            note(scene, report, static_cast<std::size_t>(node_index), "skinned_instancing_static_bake",
                 "node " + std::to_string(node_index) + ": detached skinned GPU instances and retained world-baked geometry");
        }
    }

    if (scene.source_features.active_material_variant_mapping_count != 0 &&
        !scene.source_features.material_variant_selected) {
        const auto count = scene.source_features.active_material_variant_mapping_count;
        note(scene, report, 0, "material_variants_base_material",
             "ignored " + std::to_string(count) + " active material variant mapping(s); retained each primitive's base material");
    }
    return true;
}

} // namespace dtglb::processing
