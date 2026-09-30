#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::processing {

struct SceneFeatureCanonicalizationDiagnostic {
    std::size_t index = 0;
    std::string code;
    std::string message;
};

struct SceneFeatureCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<SceneFeatureCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

bool canonicalize_scene_features(Scene& scene, SceneFeatureCanonicalizationReport& report,
                                 std::string& error);

} // namespace dtglb::processing
