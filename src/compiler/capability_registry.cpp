#include "compiler/capability_registry.h"

#include <algorithm>
#include <array>
#include <set>

namespace dtglb::compiler {
namespace {
constexpr std::array kGltfExtensions{
    CapabilityRecord{"KHR_mesh_quantization", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"KHR_node_visibility", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_animation_pointer", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SourceDecoded},
    CapabilityRecord{"EXT_meshopt_compression", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"KHR_meshopt_compression", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"KHR_draco_mesh_compression", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"EXT_mesh_gpu_instancing", CapabilityStatus::TargetSpecific, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_texture_basisu", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"EXT_texture_webp", CapabilityStatus::Supported, EvidenceLevel::RoundTripValidated},
    CapabilityRecord{"MSFT_texture_dds", CapabilityStatus::Supported, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_texture_transform", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_lights_punctual", CapabilityStatus::KnownFormatNotImplemented, EvidenceLevel::FormatUnderstood},
    CapabilityRecord{"KHR_materials_variants", CapabilityStatus::Supported, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"MSFT_lod", CapabilityStatus::KnownFormatNotImplemented, EvidenceLevel::FormatUnderstood},
    CapabilityRecord{"KHR_materials_unlit", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_pbrSpecularGlossiness", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_clearcoat", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_transmission", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_volume", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_ior", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_specular", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_sheen", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_emissive_strength", CapabilityStatus::TargetSpecific, EvidenceLevel::RetailCorpusMatched},
    CapabilityRecord{"KHR_materials_iridescence", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_anisotropy", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_diffuse_transmission", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
    CapabilityRecord{"KHR_materials_dispersion", CapabilityStatus::SupportedWithLoss, EvidenceLevel::SerializerImplemented},
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
            decision.evidence = record->evidence;
        }
        decisions.push_back(std::move(decision));
    }
    return decisions;
}

} // namespace dtglb::compiler
