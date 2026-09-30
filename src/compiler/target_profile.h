#pragma once

#include <cstdint>
#include <array>
#include <string_view>

namespace dtglb::compiler {
enum class TargetEvidenceState { RetailControl };
enum class TargetSidecarStrategy { None, HashedTextureStream, HashedMaterialStream };

struct ResourceStorageContract {
    std::string_view resource_type;
    std::uint32_t version = 0;
    std::string_view primary_artifact;
    TargetSidecarStrategy sidecar = TargetSidecarStrategy::None;
    TargetEvidenceState evidence = TargetEvidenceState::RetailControl;
};

struct TargetProfile {
    std::string_view id;
    std::string_view name;
    std::uint32_t unit_version = 0;
    std::uint32_t material_version = 0;
    std::uint32_t package_version = 0;
    TargetEvidenceState unit_evidence = TargetEvidenceState::RetailControl;
    TargetEvidenceState material_evidence = TargetEvidenceState::RetailControl;
    TargetEvidenceState package_evidence = TargetEvidenceState::RetailControl;
    std::array<ResourceStorageContract, 7> storage_contracts{};

    const ResourceStorageContract* storage_contract(std::string_view resource_type) const noexcept;
};
const TargetProfile& darktide_target_profile() noexcept;
std::string_view target_evidence_state_name(TargetEvidenceState state) noexcept;
}
