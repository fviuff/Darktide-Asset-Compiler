#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::processing {

struct AnimationCanonicalizationDiagnostic {
    std::size_t index = 0;
    std::string code;
    std::string message;
};

struct AnimationCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<AnimationCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

bool canonicalize_animations(Scene& scene, AnimationCanonicalizationReport& report,
                             std::string& error);

} // namespace dtglb::processing
