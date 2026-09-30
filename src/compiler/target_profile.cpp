#include "compiler/target_profile.h"
namespace dtglb::compiler {
const TargetProfile& darktide_target_profile() noexcept {
    static const TargetProfile profile{
        "darktide", "Warhammer 40,000: Darktide", 115, 61, 43,
        TargetEvidenceState::RetailControl, TargetEvidenceState::RetailControl, TargetEvidenceState::RetailControl,
        {{
            {"unit", 115, "unit", TargetSidecarStrategy::None, TargetEvidenceState::RetailControl},
            {"bones", 0, "bones", TargetSidecarStrategy::None, TargetEvidenceState::RetailControl},
            {"animation", 0, "animation", TargetSidecarStrategy::None, TargetEvidenceState::RetailControl},
            {"material", 61, "material", TargetSidecarStrategy::HashedMaterialStream, TargetEvidenceState::RetailControl},
            {"texture", 0, "texture", TargetSidecarStrategy::HashedTextureStream, TargetEvidenceState::RetailControl},
            {"package", 43, "package", TargetSidecarStrategy::None, TargetEvidenceState::RetailControl},
            {"state_machine", 0, "state_machine", TargetSidecarStrategy::None, TargetEvidenceState::RetailControl},
        }}
    };
    return profile;
}
const ResourceStorageContract* TargetProfile::storage_contract(std::string_view resource_type) const noexcept {
    for (const auto& contract : storage_contracts)
        if (contract.resource_type == resource_type) return &contract;
    return nullptr;
}
std::string_view target_evidence_state_name(TargetEvidenceState state) noexcept {
    return state == TargetEvidenceState::RetailControl ? "retail_control" : "unknown";
}
}
