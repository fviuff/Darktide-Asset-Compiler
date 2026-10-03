#pragma once

#include <cstdint>
#include <array>
#include <string_view>

namespace dtglb::compiler {
enum class TargetSidecarStrategy { None, HashedTextureStream, HashedMaterialStream };

struct ResourceStorageContract {
    std::string_view resource_type;
    std::uint32_t version = 0;
    std::string_view primary_artifact;
    TargetSidecarStrategy sidecar = TargetSidecarStrategy::None;
};

struct TargetProfile {
    std::string_view id;
    std::string_view name;
    std::uint32_t unit_version = 0;
    std::uint32_t material_version = 0;
    std::uint32_t package_version = 0;
    std::array<ResourceStorageContract, 8> storage_contracts{};

    const ResourceStorageContract* storage_contract(std::string_view resource_type) const noexcept;
};
const TargetProfile& darktide_target_profile() noexcept;
}
