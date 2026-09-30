#pragma once

#include "scene/scene.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::processing {

struct TopologyCanonicalizationDiagnostic {
    std::size_t primitive_index = 0;
    std::string code;
    std::string message;
};

struct TopologyCanonicalizationReport {
    bool changed = false;
    bool lossy = false;
    std::vector<TopologyCanonicalizationDiagnostic> diagnostics;
    std::vector<std::string> approximations;
};

bool canonicalize_non_triangle_topology(Scene& scene, TopologyCanonicalizationReport& report,
                                        std::string& error);

// Split indexed triangle primitives into bounded parts while preserving all
// vertex, custom-attribute, morph, and primitive provenance fields.
bool partition_triangle_primitive(const Primitive& input, std::size_t max_triangles,
                                  std::vector<Primitive>& output, std::string& error);

} // namespace dtglb::processing
