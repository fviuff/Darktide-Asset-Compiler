#include "compiler/target_profile.h"
namespace dtglb::compiler {
const TargetProfile& darktide_target_profile() noexcept {
    static const TargetProfile profile{
        "darktide", "Warhammer 40,000: Darktide", 115, 61, 43,
        {{
            {"unit", 115, "unit", TargetSidecarStrategy::None},
            {"bones", 0, "bones", TargetSidecarStrategy::None},
            {"animation", 0, "animation", TargetSidecarStrategy::None},
            {"material", 61, "material", TargetSidecarStrategy::HashedMaterialStream},
            {"texture", 0, "texture", TargetSidecarStrategy::HashedTextureStream},
            {"package", 43, "package", TargetSidecarStrategy::None},
            {"state_machine", 0, "state_machine", TargetSidecarStrategy::None},
            {"particles", 0, "particles", TargetSidecarStrategy::None},
        }}
    };
    return profile;
}
const ResourceStorageContract* TargetProfile::storage_contract(std::string_view resource_type) const noexcept {
    for (const auto& contract : storage_contracts)
        if (contract.resource_type == resource_type) return &contract;
    return nullptr;
}
}
