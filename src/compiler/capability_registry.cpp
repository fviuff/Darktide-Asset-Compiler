#include "compiler/capability_registry.h"

#include <algorithm>
#include <array>
#include <set>

namespace dtglb::compiler {
namespace {
constexpr std::array kGltfExtensions{
    CapabilityRecord{"KHR_mesh_quantization", CapabilityStatus::Supported},
    CapabilityRecord{"KHR_node_visibility", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_animation_pointer", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"EXT_meshopt_compression", CapabilityStatus::Supported},
    CapabilityRecord{"KHR_meshopt_compression", CapabilityStatus::Supported},
    CapabilityRecord{"KHR_draco_mesh_compression", CapabilityStatus::Supported},
    CapabilityRecord{"EXT_mesh_gpu_instancing", CapabilityStatus::TargetSpecific},
    CapabilityRecord{"KHR_texture_basisu", CapabilityStatus::Supported},
    CapabilityRecord{"EXT_texture_webp", CapabilityStatus::Supported},
    CapabilityRecord{"MSFT_texture_dds", CapabilityStatus::Supported},
    CapabilityRecord{"KHR_texture_transform", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_lights_punctual", CapabilityStatus::Supported},
    CapabilityRecord{"KHR_materials_variants", CapabilityStatus::Supported},
    CapabilityRecord{"MSFT_lod", CapabilityStatus::KnownFormatNotImplemented},
    CapabilityRecord{"KHR_materials_unlit", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_pbrSpecularGlossiness", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_clearcoat", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_transmission", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_volume", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_ior", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_specular", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_sheen", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_emissive_strength", CapabilityStatus::TargetSpecific},
    CapabilityRecord{"KHR_materials_iridescence", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_anisotropy", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_diffuse_transmission", CapabilityStatus::SupportedWithLoss},
    CapabilityRecord{"KHR_materials_dispersion", CapabilityStatus::SupportedWithLoss},
};
}

const CapabilityRecord* gltf_extension_capability(std::string_view extension) {
    const auto found = std::find_if(kGltfExtensions.begin(), kGltfExtensions.end(),
        [extension](const CapabilityRecord& record) { return record.feature == extension; });
    return found == kGltfExtensions.end() ? nullptr : &*found;
}

std::vector<CapabilityDecision> analyze_gltf_extension_capabilities(const Scene& scene) {
    const std::set<std::string> required(scene.extensions_required.begin(), scene.extensions_required.end());
    std::set<std::string> declared(scene.active_extensions_used.begin(), scene.active_extensions_used.end());
    declared.insert(required.begin(), required.end());
    std::vector<CapabilityDecision> decisions;
    decisions.reserve(declared.size());
    for (const auto& extension : declared) {
        CapabilityDecision decision;
        decision.feature = extension;
        decision.required = required.count(extension) != 0;
        if (const auto* record = gltf_extension_capability(extension)) {
            decision.status = record->status;
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

} // namespace dtglb::compiler
