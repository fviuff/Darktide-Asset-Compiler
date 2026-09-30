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
    ImplementedNotValidated,
    KnownFormatNotImplemented,
    UnknownFormat,
    UnsupportedByTarget,
    InvalidSource,
};

enum class EvidenceLevel {
    SourceDecoded,
    FormatUnderstood,
    SerializerImplemented,
    RoundTripValidated,
    RetailCorpusMatched,
    EngineLoads,
    RuntimeVerified,
};

struct CapabilityRecord {
    std::string_view feature;
    CapabilityStatus status = CapabilityStatus::UnknownFormat;
    EvidenceLevel evidence = EvidenceLevel::SourceDecoded;
};

struct CapabilityDecision {
    std::string feature;
    CapabilityStatus status = CapabilityStatus::UnknownFormat;
    EvidenceLevel evidence = EvidenceLevel::SourceDecoded;
    bool required = false;
};

const CapabilityRecord* gltf_extension_capability(std::string_view extension);
std::vector<CapabilityDecision> analyze_gltf_extension_capabilities(const Scene& scene);

} // namespace dtglb::compiler
