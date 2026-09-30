#include "stingray/material/transparent_profile.h"

namespace dtglb::stingray::material {
namespace {
constexpr std::uint64_t kShaderProviderMaterial = 0x1a7adfef3dbd8e95ull;
constexpr std::uint64_t kParentMaterial = 0x40bc90ba5e9b0852ull;
constexpr std::uint32_t kBca = 0x2fadcc8cu;
constexpr std::uint32_t kNormal = 0xb45e95f3u;
constexpr std::uint32_t kOrm = 0x8655079cu;
}

std::uint64_t transparent_shader_provider_material_hash() { return kShaderProviderMaterial; }
std::uint64_t transparent_parent_material_hash() { return kParentMaterial; }

bool build_transparent_profile(const TransparentProfileSpec& spec, MaterialStream& out, std::string& error) {
    for (const auto hash : spec.texture_hashes) {
        if (!hash) {
            error = "transparent profile texture hashes must be nonzero";
            return false;
        }
    }
    InheritedMaterialSpec material;
    material.shader_provider_material_hash = transparent_shader_provider_material_hash();
    material.parent_material_hash = transparent_parent_material_hash();
    material.surface_material = spec.surface_material;
    material.textures = {
        {kBca, spec.texture_hashes[0]},
        {kNormal, spec.texture_hashes[1]},
        {kOrm, spec.texture_hashes[2]},
    };
    // Keep the retail parent family's opacity and auxiliary controls at their
    // inherited defaults. The complete requested opacity is baked into BCA.a.
    return build_inherited_material(material, out, error);
}

} // namespace dtglb::stingray::material
