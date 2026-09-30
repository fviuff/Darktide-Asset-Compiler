#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::processing {

struct AttributeCanonicalizationDiagnostic {
    std::size_t primitive_index = 0;
    std::string code;
    std::string message;
};

struct AttributeCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<AttributeCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

// Removes decoded attributes which have no consumer in the finalized core profile.
bool canonicalize_unused_attributes(Scene& scene, AttributeCanonicalizationReport& report,
                                    std::string& error);

} // namespace dtglb::processing
