#pragma once

#include "scene/scene.h"

#include <string>
#include <string_view>
#include <vector>

namespace dtglb::compiler {

enum class CapabilityStatus {
    Supported,
    SupportedWithLoss,
    TargetSpecific,
    KnownFormatNotImplemented,
    UnknownFormat,
    UnsupportedByTarget,
    InvalidSource,
};

struct CapabilityRecord {
    std::string_view feature;
    CapabilityStatus status = CapabilityStatus::UnknownFormat;
};

struct CapabilityDecision {
    std::string feature;
    CapabilityStatus status = CapabilityStatus::UnknownFormat;
    bool required = false;
};

const CapabilityRecord* gltf_extension_capability(std::string_view extension);
std::vector<CapabilityDecision> analyze_gltf_extension_capabilities(const Scene& scene);

} // namespace dtglb::compiler
