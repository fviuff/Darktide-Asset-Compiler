#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::processing {

struct MaterialCanonicalizationDiagnostic {
    std::size_t material_index = 0;
    std::string code;
    std::string message;
    bool lossy = true;
};

struct MaterialCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<MaterialCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

// Converts alternate glTF workflows and optional lobes into the core
// metallic-roughness/alpha domain before texture and vertex-color baking.
bool canonicalize_material_workflows(Scene& scene, MaterialCanonicalizationReport& report,
                                     std::string& error);

// Selects the final core rendering semantics after texture/vertex-color baking.
bool finalize_material_profiles(Scene& scene, MaterialCanonicalizationReport& report,
                                std::string& error);

} // namespace dtglb::processing
