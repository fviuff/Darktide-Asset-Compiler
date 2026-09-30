#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::processing {

struct NativeProfileCanonicalizationDiagnostic {
    std::size_t index = 0;
    std::string code;
    std::string message;
};

struct NativeProfileCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<NativeProfileCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

// Applies the bounded geometry and identity rules of the current native profile.
bool canonicalize_native_profile(Scene& scene, NativeProfileCanonicalizationReport& report,
                                 std::string& error);

} // namespace dtglb::processing
