#include "processing/material_canonicalizer.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace dtglb::processing {
namespace {
constexpr float kDielectricSpecular = 0.04f;

float saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

float brightness(const std::array<float, 3>& color) {
    return std::sqrt(0.299f * color[0] * color[0] +
                     0.587f * color[1] * color[1] +
                     0.114f * color[2] * color[2]);
}

bool finite(const std::array<float, 3>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

bool finite(const std::array<float, 4>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

std::set<std::size_t> used_materials(const Scene& scene) {
    std::set<std::size_t> result;
    for (const auto& primitive : scene.primitives)
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size())
            result.insert(static_cast<std::size_t>(primitive.material));
    return result;
}

void record(Scene& scene, MaterialCanonicalizationReport& report, std::size_t index,
            std::string code, std::string message, bool lossy = true) {
    report.changed = true;
    report.lossy = report.lossy || lossy;
    report.approximations.push_back("material:" + std::to_string(index) + ":" + code);
    scene.notes.push_back("material " + scene.materials[index].name + ": " + message + " [" + code + "]");
    report.diagnostics.push_back({index, std::move(code), std::move(message), lossy});
}

float solve_metallic(float diffuse_brightness, float specular_brightness,
                     float one_minus_specular_strength) {
    if (specular_brightness < kDielectricSpecular) return 0.0f;
    const float a = kDielectricSpecular;
    const float b = diffuse_brightness * one_minus_specular_strength /
        (1.0f - kDielectricSpecular) + specular_brightness - 2.0f * kDielectricSpecular;
    const float c = kDielectricSpecular - specular_brightness;
    const float discriminant = std::max(0.0f, b * b - 4.0f * a * c);
    return saturate((-b + std::sqrt(discriminant)) / (2.0f * a));
}

bool convert_specular_glossiness(MaterialInfo& material, std::string& error) {
    if (!finite(material.diffuse_factor) || !finite(material.specular_factor) ||
        !std::isfinite(material.glossiness_factor)) {
        error = "KHR_materials_pbrSpecularGlossiness contains non-finite factors";
        return false;
    }
    std::array<float, 3> diffuse{}, specular{};
    for (std::size_t i = 0; i < 3; ++i) {
        diffuse[i] = saturate(material.diffuse_factor[i]);
        specular[i] = saturate(material.specular_factor[i]);
    }
    const float one_minus_specular_strength = 1.0f - *std::max_element(specular.begin(), specular.end());
    const float metallic = solve_metallic(brightness(diffuse), brightness(specular),
                                          one_minus_specular_strength);
    const float dielectric_denom = std::max(1e-6f,
        (1.0f - kDielectricSpecular) * std::max(1e-6f, 1.0f - metallic));
    const float metallic_denom = std::max(1e-6f, metallic);
    const float blend = metallic * metallic;
    for (std::size_t i = 0; i < 3; ++i) {
        const float from_diffuse = diffuse[i] * one_minus_specular_strength / dielectric_denom;
        const float from_specular = (specular[i] - kDielectricSpecular * (1.0f - metallic)) /
            metallic_denom;
        material.base_color[i] = saturate(from_diffuse * (1.0f - blend) + from_specular * blend);
    }
    material.base_color[3] = saturate(material.diffuse_factor[3]);
    material.metallic = metallic;
    material.roughness = saturate(1.0f - material.glossiness_factor);
    material.base_color_texture = material.diffuse_texture;
    material.metallic_roughness_texture = {};
    // Either legacy texture makes the conversion spatially varying. Keep the
    // extension alive until the shared atlas can evaluate both texture domains.
    material.has_pbr_specular_glossiness = material.diffuse_texture.texture >= 0 ||
        material.specular_glossiness_texture.texture >= 0;
    return true;
}

void clear_emission(MaterialInfo& material) {
    material.emissive = {0.0f, 0.0f, 0.0f};
    material.emissive_strength = 1.0f;
    material.emissive_texture = {};
}

bool emits(const MaterialInfo& material) {
    return material.emissive_texture.texture >= 0 ||
        (material.emissive_strength > 0.0f &&
         std::any_of(material.emissive.begin(), material.emissive.end(),
                     [](float value) { return value > 0.0f; }));
}
} // namespace

bool canonicalize_material_workflows(Scene& scene, MaterialCanonicalizationReport& report,
                                     std::string& error) {
    report = {};
    for (const auto index : used_materials(scene)) {
        auto& material = scene.materials[index];
        if (material.intent != MaterialInfo::Intent::Generated) continue;
        if (material.has_pbr_specular_glossiness) {
            const bool textured = material.diffuse_texture.texture >= 0 ||
                material.specular_glossiness_texture.texture >= 0;
            const bool below_dielectric_floor =
                *std::max_element(material.specular_factor.begin(), material.specular_factor.end()) <
                kDielectricSpecular;
            if (!convert_specular_glossiness(material, error)) return false;
            record(scene, report, index, "specgloss_to_metalrough",
                   textured ? "converted specular-glossiness factors and retained its textures for full-resolution conversion when their sampling domains are compatible, otherwise documented atlas fallback"
                            : "converted specular-glossiness factors to metallic-roughness");
            if (below_dielectric_floor) {
                record(scene, report, index, "specgloss_dielectric_floor",
                       "the target metallic-roughness shader cannot represent authored specular reflectance below its dielectric floor; preserved authored glossiness and mapped the surface to non-metal");
            }
        }
        if (material.has_transmission) {
            if (!std::isfinite(material.transmission)) {
                error = "KHR_materials_transmission contains a non-finite factor";
                return false;
            }
        }
        if (material.has_transmission && material.transmission_texture.texture < 0) {
            material.base_color[3] = saturate(material.base_color[3]) *
                (1.0f - saturate(material.transmission));
            material.alpha_mode = "BLEND";
            material.has_transmission = false;
            material.transmission_texture = {};
            record(scene, report, index, "transmission_to_blend",
                   "approximated transmission with baked transparent-family alpha");
        }
        if (material.has_volume) {
            material.has_volume = false;
            material.thickness_texture = {};
            record(scene, report, index, "volume_ignored",
                   "retained the base surface and omitted volumetric attenuation");
        }
        if (material.has_ior) {
            material.has_ior = false;
            record(scene, report, index, "ior_ignored",
                   "retained the base surface and omitted the authored index of refraction");
        }
        if (material.has_clearcoat) {
            material.has_clearcoat = false;
            material.clearcoat_texture = {};
            material.clearcoat_roughness_texture = {};
            material.clearcoat_normal_texture = {};
            record(scene, report, index, "clearcoat_to_base_pbr",
                   "retained the base PBR surface and omitted the secondary clearcoat lobe");
        }
        if (material.has_sheen) {
            material.has_sheen = false;
            material.sheen_color_texture = {};
            material.sheen_roughness_texture = {};
            record(scene, report, index, "sheen_to_base_pbr",
                   "retained the base PBR surface and omitted the sheen lobe");
        }
        if (material.has_specular) {
            if (!std::isfinite(material.specular) || !finite(material.specular_color)) {
                error = "KHR_materials_specular contains non-finite factors";
                return false;
            }
            const bool textured_specular = material.specular_texture.texture >= 0 || material.specular_color_texture.texture >= 0;
            if (material.specular_texture.texture < 0 && material.specular_color_texture.texture < 0 &&
                material.metallic < 0.5f) {
                std::array<float, 3> adjusted{};
                for (std::size_t i = 0; i < 3; ++i)
                    adjusted[i] = kDielectricSpecular * saturate(material.specular) *
                        saturate(material.specular_color[i]);
                const float base = brightness({material.base_color[0], material.base_color[1], material.base_color[2]});
                const float target = brightness(adjusted);
                if (std::abs(base - kDielectricSpecular) > 1e-6f)
                    material.metallic = saturate((target - kDielectricSpecular) /
                                                 (base - kDielectricSpecular));
            }
            material.has_specular = false;
            material.specular_texture = {};
            material.specular_color_texture = {};
            record(scene, report, index, "specular_to_metalrough",
                   textured_specular
                       ? "approximated scalar dielectric specular response; textured specular lobe was omitted"
                       : "approximated the dielectric specular lobe in the base metallic-roughness surface");
        }
        if (material.has_iridescence) {
            material.has_iridescence = false;
            material.iridescence_texture = {};
            material.iridescence_thickness_texture = {};
            record(scene, report, index, "iridescence_to_base_pbr",
                   "retained the base PBR surface and omitted the iridescence lobe");
        }
        if (material.has_anisotropy) {
            material.has_anisotropy = false;
            material.anisotropy_texture = {};
            record(scene, report, index, "anisotropy_to_base_pbr",
                   "retained the base PBR surface and omitted directional anisotropy");
        }
        if (material.has_diffuse_transmission) {
            material.has_diffuse_transmission = false;
            material.diffuse_transmission_texture = {};
            material.diffuse_transmission_color_texture = {};
            record(scene, report, index, "diffuse_transmission_to_base_pbr",
                   "retained the base PBR surface and omitted diffuse transmission");
        }
        if (material.has_dispersion) {
            material.has_dispersion = false;
            record(scene, report, index, "dispersion_to_base_pbr",
                   "retained the base PBR surface and omitted wavelength dispersion");
        }
    }
    return true;
}

bool finalize_material_profiles(Scene& scene, MaterialCanonicalizationReport& report,
                                std::string& error) {
    for (const auto index : used_materials(scene)) {
        auto& material = scene.materials[index];
        if (material.intent != MaterialInfo::Intent::Generated) continue;
        if (material.unlit) {
            if (material.alpha_mode == "OPAQUE") {
                material.emissive = {material.base_color[0], material.base_color[1], material.base_color[2]};
                material.emissive_strength = 1.0f;
                material.emissive_texture = material.base_color_texture;
                material.base_color = {0.0f, 0.0f, 0.0f, 1.0f};
                material.base_color_texture = {};
            } else {
                material.alpha_mode = "BLEND";
                clear_emission(material);
            }
            material.metallic = 0.0f;
            material.roughness = 1.0f;
            material.unlit = false;
            record(scene, report, index, "unlit_to_emissive_pbr",
                   "approximated unlit shading with a current PBR/emissive or transparent family");
        }
        if (material.alpha_mode != "OPAQUE" && material.alpha_mode != "MASK" &&
            material.alpha_mode != "BLEND") {
            material.alpha_mode = "OPAQUE";
            record(scene, report, index, "alpha_mode_to_opaque",
                   "mapped an unknown alpha mode to the opaque core profile");
        }
        if (material.alpha_mode == "MASK") {
            if (!std::isfinite(material.alpha_cutoff)) {
                error = "MASK alpha cutoff is non-finite";
                return false;
            }
            const float clamped = saturate(material.alpha_cutoff);
            if (clamped != material.alpha_cutoff) {
                material.alpha_cutoff = clamped;
                record(scene, report, index, "alpha_cutoff_clamped",
                       "clamped MASK alpha cutoff to the representable range");
            }
        }
        if (!std::isfinite(material.normal_texture.scale) ||
            !std::isfinite(material.occlusion_texture.strength)) {
            error = "material texture scale and strength must be finite";
            return false;
        }
        if (!std::isfinite(material.emissive_strength) ||
            !std::all_of(material.emissive.begin(), material.emissive.end(),
                         [](float value) { return std::isfinite(value); })) {
            error = "material emissive factor and strength must be finite";
            return false;
        }
        bool clamped_emission = material.emissive_strength < 0.0f;
        material.emissive_strength = std::max(0.0f, material.emissive_strength);
        for (auto& value : material.emissive) {
            clamped_emission = clamped_emission || value < 0.0f;
            value = std::max(0.0f, value);
        }
        if (clamped_emission)
            record(scene, report, index, "negative_emission_clamped",
                   "clamped finite negative emissive factor/strength components to zero");
        const float clamped_occlusion_strength = saturate(material.occlusion_texture.strength);
        if (clamped_occlusion_strength != material.occlusion_texture.strength) {
            material.occlusion_texture.strength = clamped_occlusion_strength;
            record(scene, report, index, "occlusion_strength_clamped",
                   "clamped occlusion strength to the native ORM representable range");
        }
        if (material.alpha_mode == "BLEND" && emits(material)) {
            clear_emission(material);
            record(scene, report, index, "blend_emission_to_base_color",
                   "retained transparent base color and omitted the unsupported simultaneous emission lobe");
        } else if (material.alpha_mode == "MASK" && emits(material)) {
            clear_emission(material);
            record(scene, report, index, "mask_emission_to_base_color",
                   "retained masked base color and omitted the unsupported simultaneous emission lobe");
        }
    }
    return true;
}

} // namespace dtglb::processing
