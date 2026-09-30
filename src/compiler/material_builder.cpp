#include "compiler/material_builder.h"
#include "compiler/source_queries.h"
#include "processing/image_sampler.h"
#include "stingray/material/emissive_profile.h"
#include "stingray/material/mask_profile.h"
#include "stingray/material/pbr_emissive_profile.h"
#include "stingray/material/transparent_profile.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"
#include "stingray/texture/texture_writer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <sstream>

namespace dtglb::compiler {
bool texture_uses_texcoord0(const dtglb::TextureBinding& binding) {
    const auto& transform = binding.transform;
    const int texcoord = transform.texcoord < 0 ? binding.texcoord : transform.texcoord;
    return texcoord == 0;
}

bool repeat_tile_preserving_transform(const dtglb::TextureBinding& binding) {
    const auto& transform = binding.transform;
    const float c = std::cos(transform.rotation), s = std::sin(transform.rotation);
    const float a = c * transform.scale[0], b = -s * transform.scale[1];
    const float d = s * transform.scale[0], e = c * transform.scale[1];
    constexpr float tolerance = 1e-5f;
    const auto near_integer = [](float value) {
        return std::abs(value - std::round(value)) <= tolerance;
    };
    return near_integer(a) && near_integer(b) && near_integer(d) && near_integer(e) &&
        std::abs(std::round(a)) + std::abs(std::round(b)) == 1.0f &&
        std::abs(std::round(d)) + std::abs(std::round(e)) == 1.0f &&
        std::abs(std::round(a)) + std::abs(std::round(d)) == 1.0f &&
        std::abs(std::round(b)) + std::abs(std::round(e)) == 1.0f;
}

bool resolve_binding_image(const Scene& scene, const dtglb::TextureBinding& binding,
                           const ImageInfo*& image, std::string& error) {
    image = nullptr;
    if (binding.texture < 0) return true;
    if (static_cast<std::size_t>(binding.texture) >= scene.textures.size()) {
        error = "material texture index is outside the decoded texture table";
        return false;
    }
    if (!texture_uses_texcoord0(binding)) {
        error = "core substance_basic lowering currently requires TEXCOORD_0";
        return false;
    }
    if (!std::isfinite(binding.transform.offset[0]) || !std::isfinite(binding.transform.offset[1]) ||
        !std::isfinite(binding.transform.scale[0]) || !std::isfinite(binding.transform.scale[1]) ||
        !std::isfinite(binding.transform.rotation)) {
        error = "material texture transform must contain finite values";
        return false;
    }
    if (!repeat_tile_preserving_transform(binding)) {
        error = "core substance_basic lowering can bake only repeat-preserving UV transforms (quarter-turns and axis flips with unit scale)";
        return false;
    }
    const auto& texture = scene.textures[static_cast<std::size_t>(binding.texture)];
    if (texture.sampler >= 0) {
        if (static_cast<std::size_t>(texture.sampler) >= scene.samplers.size()) {
            error = "material texture sampler index is outside the decoded sampler table";
            return false;
        }
        const auto& sampler = scene.samplers[static_cast<std::size_t>(texture.sampler)];
        const bool linear_mag = sampler.mag_filter == 0 || sampler.mag_filter == 9729;
        const bool trilinear_min = sampler.min_filter == 0 || sampler.min_filter == 9987;
        if (sampler.wrap_s != 10497 || sampler.wrap_t != 10497 || !linear_mag || !trilinear_min) {
            error = "core substance_basic lowering currently requires repeating linear/trilinear sampling";
            return false;
        }
    }
    const int source = texture.basisu_source >= 0 ? texture.basisu_source :
        (texture.webp_source >= 0 ? texture.webp_source : (texture.dds_source >= 0 ? texture.dds_source : texture.source));
    if (source < 0 || static_cast<std::size_t>(source) >= scene.images.size()) {
        error = "material texture has no decoded image source";
        return false;
    }
    image = &scene.images[static_cast<std::size_t>(source)];
    return true;
}

bool decode_image(const ImageInfo& image, stingray::texture::ImageRGBA& output, std::string& error) {
    if (image.rgba8) {
        const auto& payload = *image.rgba8;
        const auto pixel_count = static_cast<std::uint64_t>(payload.width) * payload.height;
        if (payload.width == 0 || payload.height == 0 ||
            pixel_count > std::numeric_limits<std::size_t>::max() / 4u ||
            payload.bytes.size() != static_cast<std::size_t>(pixel_count * 4u)) {
            error = "in-memory RGBA8 image has invalid dimensions or pixel payload";
            return false;
        }
        output.width = payload.width;
        output.height = payload.height;
        output.pixels = payload.bytes;
        return true;
    }
    return stingray::texture::decode_image_rgba(image.bytes, image.mime_type, output, error);
}

float sample_scalar_repeat(const stingray::texture::ImageRGBA& image, std::size_t channel,
                           float u, float v);

bool bake_texture_transform(const Scene& scene, const dtglb::TextureBinding& binding,
                            processing::ImageSemantic semantic,
                            stingray::texture::ImageRGBA& image, std::string& error) {
    const auto& transform = binding.transform;
    if (std::abs(transform.offset[0]) <= 1e-6f && std::abs(transform.offset[1]) <= 1e-6f &&
        std::abs(transform.scale[0] - 1.0f) <= 1e-6f && std::abs(transform.scale[1] - 1.0f) <= 1e-6f &&
        std::abs(transform.rotation) <= 1e-6f) return true;

    RGBA8Payload source{image.width, image.height, image.pixels};
    SamplerInfo sampler;
    const auto& texture = scene.textures[static_cast<std::size_t>(binding.texture)];
    if (texture.sampler >= 0) sampler = scene.samplers[static_cast<std::size_t>(texture.sampler)];
    processing::PreparedImageSampler prepared;
    if (!processing::prepare_image_sampler(source, sampler, semantic, prepared, error)) return false;
    // A quarter-turn exchanges the UV axes. Preserve source texel density by
    // exchanging the baked image dimensions as well as its pixel arrangement.
    if (std::abs(std::cos(transform.rotation)) < 1e-5f)
        std::swap(image.width, image.height);
    const std::array<float, 2> zero{};
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
            processing::ImageSample sample;
            if (!processing::sample_image(prepared, binding, {u, v}, zero, zero, sample, error)) return false;
            const auto pixel = (static_cast<std::size_t>(y) * image.width + x) * 4u;
            std::array<std::uint8_t, 4> sampled{};
            for (std::size_t channel = 0; channel < sampled.size(); ++channel)
                sampled[channel] = static_cast<std::uint8_t>(std::lround(sample.rgba[channel] * 255.0f));
            std::copy(sampled.begin(), sampled.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(pixel));
        }
    }
    return true;
}

bool decode_binding(const Scene& scene, const dtglb::TextureBinding& binding,
                    processing::ImageSemantic semantic, stingray::texture::ImageRGBA& output,
                    bool& present, std::string& error) {
    const ImageInfo* image = nullptr;
    if (!resolve_binding_image(scene, binding, image, error)) return false;
    present = image != nullptr;
    if (!present) return true;
    return decode_image(*image, output, error) && bake_texture_transform(scene, binding, semantic, output, error);
}

stingray::texture::ImageRGBA solid_image(std::array<std::uint8_t,4> rgba) {
    stingray::texture::ImageRGBA image;
    image.width = image.height = 512;
    image.pixels.resize(512u * 512u * 4u);
    for (std::size_t offset = 0; offset < image.pixels.size(); offset += 4u)
        std::copy(rgba.begin(), rgba.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
    return image;
}

std::uint8_t unorm8(float value) {
    return static_cast<std::uint8_t>(std::clamp<long>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f), 0, 255));
}

float srgb_to_linear(std::uint8_t value) {
    const float encoded = static_cast<float>(value) / 255.0f;
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t linear_to_srgb(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return unorm8(value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f);
}

bool prepare_core_images(const Scene& scene, const MaterialInfo& material,
                         std::array<stingray::texture::ImageRGBA,4>& images, std::string& error,
                         bool preserve_base_alpha = false, bool decode_emissive = true) {
    bool has_base = false, has_normal = false, has_mr = false, has_occlusion = false;
    stingray::texture::ImageRGBA normal;
    if (!decode_binding(scene, material.base_color_texture, processing::ImageSemantic::SRGB, images[0], has_base, error) ||
        !decode_binding(scene, material.normal_texture, processing::ImageSemantic::TangentNormal, normal, has_normal, error)) return false;
    stingray::texture::ImageRGBA metallic_roughness, occlusion;
    if (!decode_binding(scene, material.metallic_roughness_texture, processing::ImageSemantic::Linear, metallic_roughness, has_mr, error) ||
        !decode_binding(scene, material.occlusion_texture, processing::ImageSemantic::Linear, occlusion, has_occlusion, error)) return false;
    if (!has_base) images[0] = solid_image({255,255,255,255});
    if (!has_normal) normal = solid_image({128,128,255,255});
    const std::uint32_t orm_width = std::min(8192u, std::max(has_mr ? metallic_roughness.width : 0u,
                                                             has_occlusion ? occlusion.width : 0u));
    const std::uint32_t orm_height = std::min(8192u, std::max(has_mr ? metallic_roughness.height : 0u,
                                                              has_occlusion ? occlusion.height : 0u));
    if (orm_width == 0 || orm_height == 0) images[3] = solid_image({255,255,255,255});
    else {
        const auto pixel_count = static_cast<std::uint64_t>(orm_width) * orm_height;
        if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u) {
            error = "ORM output dimensions exceed addressable memory";
            return false;
        }
        images[3] = {};
        images[3].width = orm_width;
        images[3].height = orm_height;
        try { images[3].pixels.assign(static_cast<std::size_t>(pixel_count * 4u), 255); }
        catch (const std::bad_alloc&) { error = "ORM output allocation failed"; return false; }
    }
    bool has_emissive = false;
    if (decode_emissive && !decode_binding(scene, material.emissive_texture, processing::ImageSemantic::SRGB, images[1], has_emissive, error)) return false;
    if (!has_emissive) images[1] = solid_image({255,255,255,255});
    images[2] = std::move(normal);

    for (std::size_t p = 0; p < images[0].pixels.size(); p += 4u) {
        for (std::size_t channel = 0; channel < 3u; ++channel)
            images[0].pixels[p + channel] = linear_to_srgb(
                srgb_to_linear(images[0].pixels[p + channel]) * material.base_color[channel]);
        images[0].pixels[p + 3u] = preserve_base_alpha
            ? unorm8(static_cast<float>(images[0].pixels[p + 3u]) / 255.0f * material.base_color[3]) : 255;
    }
    for (std::size_t p = 0; p < images[2].pixels.size(); p += 4u) {
        float x = (static_cast<float>(images[2].pixels[p]) / 255.0f * 2.0f - 1.0f) * material.normal_texture.scale;
        float y = (static_cast<float>(images[2].pixels[p + 1u]) / 255.0f * 2.0f - 1.0f) * material.normal_texture.scale;
        float z = static_cast<float>(images[2].pixels[p + 2u]) / 255.0f * 2.0f - 1.0f;
        const float length = std::sqrt(x*x + y*y + z*z);
        if (length > 1e-20f) { x /= length; y /= length; z /= length; }
        else { x = y = 0.0f; z = 1.0f; }
        images[2].pixels[p] = unorm8(x * 0.5f + 0.5f);
        images[2].pixels[p + 1u] = unorm8(y * 0.5f + 0.5f);
        images[2].pixels[p + 2u] = unorm8(z * 0.5f + 0.5f);
        images[2].pixels[p + 3u] = 255;
    }
    for (std::size_t p = 0; p < images[3].pixels.size(); p += 4u) {
        const auto pixel = p / 4u;
        const auto x = static_cast<std::uint32_t>(pixel % images[3].width);
        const auto y = static_cast<std::uint32_t>(pixel / images[3].width);
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(images[3].width);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(images[3].height);
        const auto sample = [&](const stingray::texture::ImageRGBA& source, std::size_t channel) {
            if (source.width == images[3].width && source.height == images[3].height) {
                const auto source_offset = (static_cast<std::size_t>(y) * source.width + x) * 4u + channel;
                return static_cast<float>(source.pixels[source_offset]) / 255.0f;
            }
            return sample_scalar_repeat(source, channel, u, v);
        };
        const float sampled_occlusion = has_occlusion ? sample(occlusion, 0) : 1.0f;
        const float sampled_roughness = has_mr ? sample(metallic_roughness, 1) : 1.0f;
        const float sampled_metallic = has_mr ? sample(metallic_roughness, 2) : 1.0f;
        images[3].pixels[p] = unorm8(1.0f + material.occlusion_texture.strength * (sampled_occlusion - 1.0f));
        images[3].pixels[p + 1u] = unorm8(sampled_roughness * material.roughness);
        images[3].pixels[p + 2u] = unorm8(sampled_metallic * material.metallic);
        images[3].pixels[p + 3u] = 255;
    }
    // The PBR-emissive family samples this texture as linear emission. Image
    // decoders retain glTF's sRGB encoding, so convert RGB before writing the
    // native linear profile. This matches the linear emission atlas path.
    for (std::size_t p = 0; p < images[1].pixels.size(); p += 4u) {
        for (std::size_t channel = 0; channel < 3u; ++channel)
            images[1].pixels[p + channel] = unorm8(srgb_to_linear(images[1].pixels[p + channel]));
        images[1].pixels[p + 3u] = 255;
    }
    return true;
}

float sample_scalar_repeat(const stingray::texture::ImageRGBA& image, std::size_t channel,
                           float u, float v) {
    const float x = u * static_cast<float>(image.width) - 0.5f;
    const float y = v * static_cast<float>(image.height) - 0.5f;
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto wrap_index = [](int index, std::uint32_t extent) {
        const int size = static_cast<int>(extent);
        index %= size;
        return index < 0 ? index + size : index;
    };
    float result = 0.0f;
    for (int dy = 0; dy != 2; ++dy) for (int dx = 0; dx != 2; ++dx) {
        const auto sx = wrap_index(x0 + dx, image.width);
        const auto sy = wrap_index(y0 + dy, image.height);
        const auto offset = (static_cast<std::size_t>(sy) * image.width + static_cast<std::size_t>(sx)) * 4u + channel;
        const float weight = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
        result += static_cast<float>(image.pixels[offset]) / 255.0f * weight;
    }
    return result;
}

std::string core_material_gap(const Scene& scene, const MaterialInfo& material) {
    if (material.intent == MaterialInfo::Intent::External) return {};
    if (material.intent == MaterialInfo::Intent::Donor) return {};
    static const std::array<const char*, 8> allowed_surface_materials{{
        "default", "metal_solid", "metal_sheet", "cloth", "concrete", "brick", "bone", "plastic"}};
    if (std::find_if(allowed_surface_materials.begin(), allowed_surface_materials.end(),
                     [&](const char* value) { return material.surface_material == value; }) ==
        allowed_surface_materials.end())
        return "surface material is outside the evidenced substance_basic child profile";
    if (material.intent == MaterialInfo::Intent::Template)
        return "named Darktide material templates are unsupported because no qualified template catalog is available";
    if (material.unlit)
        return "core material requires a lit substance_basic profile";
    if (material.alpha_mode != "OPAQUE" && material.alpha_mode != "MASK" &&
        material.alpha_mode != "BLEND")
        return "core material requires OPAQUE, MASK, or BLEND alpha mode";
    if (material.alpha_mode == "MASK" &&
        (!std::isfinite(material.alpha_cutoff) || material.alpha_cutoff < 0.0f || material.alpha_cutoff > 1.0f))
        return "MASK alpha cutoff must be finite and within [0,1]";
    if (material.has_clearcoat || material.has_transmission || material.has_volume || material.has_sheen ||
        material.has_specular || material.has_iridescence || material.has_anisotropy ||
        material.has_diffuse_transmission || material.has_dispersion)
        return "advanced KHR_materials_* profile has no proven Darktide parent-material mapping";
    if (material.has_pbr_specular_glossiness)
        return "KHR_materials_pbrSpecularGlossiness has no proven Darktide parent-material mapping";
    if (material.has_ior)
        return "KHR_materials_ior has no proven Darktide parent-material mapping";
    if (!std::isfinite(material.emissive_strength) || material.emissive_strength < 0.0f ||
        std::any_of(material.emissive.begin(), material.emissive.end(),
                    [](float value) { return !std::isfinite(value) || value < 0.0f; }))
        return "emissive factor and strength must be finite and nonnegative";
    if (material.alpha_mode == "MASK" && material.emissive_strength > 0.0f &&
        std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; }))
        return "MASK materials with effective emission have no proven Darktide parent-material mapping";
    const bool emits = material.emissive_strength > 0.0f &&
        std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; });
    const std::array<const dtglb::TextureBinding*,5> bindings{{&material.base_color_texture,
        &material.metallic_roughness_texture, &material.normal_texture, &material.occlusion_texture,
        &material.emissive_texture}};
    std::array<stingray::texture::ImageRGBA,5> decoded_images;
    std::array<bool,5> decoded{};
    for (std::size_t binding_index = 0; binding_index < bindings.size(); ++binding_index) {
        if (binding_index == 4 && !emits) continue;
        const auto* binding = bindings[binding_index];
        const ImageInfo* image = nullptr;
        std::string error;
        if (!resolve_binding_image(scene, *binding, image, error)) return error;
        if (image && !image->rgba8 && image->mime_type != "image/png" && image->mime_type != "image/jpeg" &&
            image->mime_type != "image/webp" && image->mime_type != "image/ktx2" &&
            image->mime_type != "image/vnd-ms.dds")
            return "automatic core-PBR material images currently require PNG, JPEG, WebP, KTX2, or DDS decoding";
        if (image) {
            std::string decode_error;
            decoded[binding_index] = decode_image(*image, decoded_images[binding_index], decode_error);
            if (!decoded[binding_index])
                return decode_error.empty() ? "material image decoding failed" : decode_error;
            if (decoded[binding_index]) {
                const auto width = decoded_images[binding_index].width;
                const auto height = decoded_images[binding_index].height;
                if (width == 0 || height == 0 ||
                    decoded_images[binding_index].pixels.size() !=
                        static_cast<std::size_t>(width) * height * 4u)
                    return "decoded image has invalid dimensions or pixel payload";
            }
        }
    }
    if (!std::isfinite(material.normal_texture.scale) ||
        !std::isfinite(material.occlusion_texture.strength) || material.occlusion_texture.strength < 0.0f ||
        material.occlusion_texture.strength > 1.0f)
        return "normal scale must be finite and occlusion strength must be within the supported finite range";
    return {};
}


}

namespace dtglb::compiler {
namespace {
std::string opaque_id(std::uint64_t hash) {
    std::ostringstream text;
    text << "#ID[" << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << hash << ']';
    return text.str();
}

// This is a deliberately narrow retained-world-surface contract. The current
// UNIT writer has no renderer-qualified general material-template interface.
bool world_surface_blend_v1(const stingray::material::MaterialStream& donor) {
    static constexpr std::array<std::uint32_t, 4> channels{
        0x2fadcc8cu, 0xcdd75b95u, 0xb45e95f3u, 0x8655079cu};
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> variables{{
        {0x221fd3eau, 24u}, {0x962c2f49u, 28u}, {0xed4bd4a3u, 20u},
        {0xb4f7e4eau, 12u}, {0x2567cfb7u, 16u}}};
    if (donor.version != 61 || donor.direct_shader_selector_hash != 0 ||
        donor.shader_provider_material_hash != 0xfca4919a8f02c7c0ull ||
        donor.parent_material_hash != 0x6338fcbda2d49d0aull ||
        donor.textures.size() != channels.size() || donor.variables.size() != variables.size() ||
        donor.contexts.size() != 1 || donor.contexts[0].name_hash != 0x0254a04bu ||
        donor.variable_data.size() != 32 || !donor.unknown_u32.empty() ||
        !donor.unknown_5.empty() || !donor.unknown_8.empty() ||
        !donor.shader_blob.empty() || !donor.other_blob.empty()) return false;
    for (std::size_t i = 0; i < channels.size(); ++i)
        if (donor.textures[i].channel_hash != channels[i] || !donor.textures[i].resource_hash) return false;
    for (std::size_t i = 0; i < variables.size(); ++i) {
        const auto& actual = donor.variables[i];
        if (actual.name_hash != variables[i].first || actual.data_offset != variables[i].second ||
            actual.klass != 0 || actual.elements != 0 || actual.element_stride != 0) return false;
    }
    return true;
}

bool build_donor_material(const Scene& scene, const MaterialInfo& material, const CompilationContext& context,
                          const std::string& stem, const std::string& slot,
                          ResourceGraph& graph,
                          std::vector<std::pair<std::string, std::string>>& bindings,
                          std::string& error) {
    if (material.donor_family != "world_surface_blend_v1") {
        error = "unknown native material donor family: " + material.donor_family;
        return false;
    }
    const auto in_unit_range = [](float value) {
        return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
    };
    if (material.double_sided || material.unlit || material.alpha_mode != "OPAQUE" ||
        material.has_clearcoat || material.has_transmission || material.has_volume || material.has_sheen ||
        material.has_specular || material.has_iridescence || material.has_anisotropy ||
        material.has_diffuse_transmission || material.has_dispersion || material.has_ior ||
        material.has_pbr_specular_glossiness || material.diffuse_texture.texture >= 0 ||
        material.specular_glossiness_texture.texture >= 0 || material.emissive_texture.texture >= 0 ||
        (material.emissive_strength > 0.0f &&
         std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value != 0.0f; }))) {
        error = "world_surface_blend_v1 donor does not map authored alpha, sidedness, emissive, or extended material semantics";
        return false;
    }
    if (!std::all_of(material.base_color.begin(), material.base_color.end(), in_unit_range) ||
        !in_unit_range(material.metallic) || !in_unit_range(material.roughness) ||
        !std::isfinite(material.normal_texture.scale) || material.normal_texture.scale < 0.0f ||
        !in_unit_range(material.occlusion_texture.strength)) {
        error = "world_surface_blend_v1 donor requires finite glTF factors within the supported ranges";
        return false;
    }
    std::filesystem::path path(material.donor_stream_path);
#ifdef _WIN32
    // A donor may live under a deeply nested game extraction. Windows still
    // requires an extended path here when the compiler is not long-path aware.
    if (path.is_absolute()) {
        const auto native = path.native();
        if (!native.starts_with(L"\\\\?\\")) {
            path = native.starts_with(L"\\\\")
                ? std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2))
                : std::filesystem::path(L"\\\\?\\" + native);
        }
    }
#endif
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot locate native material donor stream: " + material.donor_stream_path + " (" + ec.message() + ")";
        return false;
    }
    if (size != 264) {
        error = "world_surface_blend_v1 needs its 264-byte native material stream: " + material.donor_stream_path;
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read native material donor stream: " + path.string();
        return false;
    }
    stingray::material::MaterialStream parsed;
    if (!stingray::material::parse_material_stream(bytes, parsed, error) ||
        !world_surface_blend_v1(parsed)) {
        error = "native material donor is not the evidenced world_surface_blend_v1 layout";
        return false;
    }

    static constexpr std::array<std::uint32_t, 3> channels{
        0x2fadcc8cu, 0xb45e95f3u, 0x8655079cu}; // bca, nm, orm
    const std::array<const dtglb::TextureBinding*, 4> source_bindings{
        &material.base_color_texture, &material.normal_texture,
        &material.metallic_roughness_texture, &material.occlusion_texture};
    std::array<bool, 4> has_image{};
    for (std::size_t i = 0; i < source_bindings.size(); ++i) {
        const ImageInfo* image = nullptr;
        if (!resolve_binding_image(scene, *source_bindings[i], image, error)) return false;
        has_image[i] = image != nullptr;
    }
    const std::array<bool, 3> replace_channel{
        has_image[0], has_image[1], has_image[2] || has_image[3]};
    if (!replace_channel[0] &&
        std::any_of(material.base_color.begin(), material.base_color.end(),
                    [](float value) { return std::abs(value - 1.0f) > 1e-6f; })) {
        error = "world_surface_blend_v1 cannot apply baseColor factors without an authored baseColor image";
        return false;
    }
    if (!replace_channel[2] &&
        (std::abs(material.metallic - 1.0f) > 1e-6f ||
         std::abs(material.roughness - 1.0f) > 1e-6f ||
         std::abs(material.occlusion_texture.strength - 1.0f) > 1e-6f)) {
        error = "world_surface_blend_v1 cannot apply metallic, roughness, or occlusion factors without an authored ORM image";
        return false;
    }
    if (!replace_channel[1] && std::abs(material.normal_texture.scale - 1.0f) > 1e-6f) {
        error = "world_surface_blend_v1 cannot apply normal strength without an authored normal image";
        return false;
    }
    std::array<std::size_t, 3> donor_texture_indices{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto found = std::find_if(parsed.textures.begin(), parsed.textures.end(), [&](const auto& texture) {
            return texture.channel_hash == channels[i];
        });
        if (found == parsed.textures.end()) {
            error = "world_surface_blend_v1 donor is missing an editable texture channel";
            return false;
        }
        donor_texture_indices[i] = static_cast<std::size_t>(found - parsed.textures.begin());
    }
    std::array<stingray::texture::ImageRGBA, 4> images;
    std::map<std::uint32_t, ResourceKey> owned_textures;
    std::vector<stingray::material::TextureResourceHashEdit> texture_edits;
    if (std::any_of(replace_channel.begin(), replace_channel.end(), [](bool present) { return present; })) {
        if (!prepare_core_images(scene, material, images, error, true, false)) return false;
        const std::array<std::size_t, 3> image_indices{0u, 2u, 3u};
        const std::array<const stingray::texture::TextureProfile*, 3> profiles{
            &stingray::texture::substance_basic_bca_profile(),
            &stingray::texture::substance_basic_nm_profile(),
            &stingray::texture::substance_basic_orm_profile()};
        static constexpr std::array<const char*, 3> suffixes{"bca", "nm", "orm"};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (!replace_channel[i]) continue;
            const std::string texture_stem = stem + "_" + suffixes[i];
            const auto key = context.generated_key("texture", texture_stem);
            const auto& old_texture = parsed.textures[donor_texture_indices[i]];
            texture_edits.push_back({channels[i], old_texture.resource_hash,
                                     stingray::resource_name_hash(key.name)});
            owned_textures.emplace(channels[i], key);
            graph.owned.push_back({key, texture_stem + ".texture",
                TextureResource{std::move(images[image_indices[i]]), *profiles[i]}, {}});
        }
    }
    for (const auto& [name, value] : material.donor_variable_overrides) {
        if (!std::isfinite(value)) { error = "native material variable must be finite: " + name; return false; }
        const auto name_hash = stingray::id32_from_id64(name);
        const auto found = std::find_if(parsed.variables.begin(), parsed.variables.end(),
                                        [&](const auto& variable) { return variable.name_hash == name_hash; });
        if ((name != "nm_r_blend" && name != "shared_blend" && name != "bc_blend") ||
            found == parsed.variables.end()) {
            error = "world_surface_blend_v1 has no editable reflected scalar: " + name;
            return false;
        }
        std::vector<std::uint8_t> expected(4), replacement(4), edited;
        std::memcpy(expected.data(), parsed.variable_data.data() + found->data_offset, 4);
        std::memcpy(replacement.data(), &value, 4);
        if (!stingray::material::clone_material_v61_with_variable_data_edit(
                bytes, found->data_offset, expected, replacement, edited, error)) return false;
        bytes = std::move(edited);
    }
    if (!texture_edits.empty()) {
        std::vector<std::uint8_t> edited;
        if (!stingray::material::clone_material_v61_with_texture_hash_edits(
                bytes, texture_edits, edited, error)) return false;
        bytes = std::move(edited);
        stingray::material::MaterialStream texture_edited;
        if (!stingray::material::parse_material_stream(bytes, texture_edited, error) ||
            !world_surface_blend_v1(texture_edited)) {
            error = "edited world_surface_blend_v1 stream no longer matches the retained donor layout";
            return false;
        }
        parsed = std::move(texture_edited);
    }
    stingray::material::MaterialDependencyHashes hashes;
    if (!stingray::material::validate_preserved_material_v61(bytes, hashes, error)) return false;
    const auto key = context.generated_key("material", stem);
    std::vector<ResourceKey> dependencies;
    const auto add_external = [&](const char* type, std::uint64_t hash) {
        if (!hash) return;
        ResourceKey dependency{type, opaque_id(hash)};
        graph.external.push_back({dependency, true});
        dependencies.push_back(std::move(dependency));
    };
    add_external("material", hashes.shader_provider_material_hash);
    add_external("material", hashes.parent_material_hash);
    for (const auto& texture : parsed.textures) {
        const auto owned = owned_textures.find(texture.channel_hash);
        if (owned != owned_textures.end()) {
            if (stingray::resource_name_hash(owned->second.name) != texture.resource_hash) {
                error = "edited donor texture reference does not match its owned resource";
                return false;
            }
            dependencies.push_back(owned->second);
        } else add_external("texture", texture.resource_hash);
    }
    graph.owned.push_back({key, stem + ".material",
        stingray::material::PreservedMaterialStream{std::move(bytes)}, std::move(dependencies)});
    bindings.push_back({slot, key.name});
    return true;
}
}

bool build_core_materials(const Scene& scene, const CompilationContext& context,
                          ResourceGraph& graph,
                          std::vector<std::pair<std::string, std::string>>& bindings,
                          std::string& error,
                          const stingray::unit::MaterialSlotLowering* slots) {
    stingray::unit::MaterialSlotLowering resolved;
    if (!slots) {
        std::vector<std::string> names;
        std::vector<int> indices;
        for (const auto& material : scene.materials) names.push_back(material.name);
        for (const auto& primitive : scene.primitives) indices.push_back(primitive.material);
        resolved = stingray::unit::lower_material_slots(names, indices);
        slots = &resolved;
    }
    if (slots->material_slots.size() != scene.materials.size() ||
        slots->primitive_slots.size() != scene.primitives.size()) {
        error = "material slot lowering does not match source scene";
        return false;
    }
    const auto basic_profiles = std::array<const stingray::texture::TextureProfile*, 3>{
        &stingray::texture::substance_basic_bc_profile(), &stingray::texture::substance_basic_nm_profile(),
        &stingray::texture::substance_basic_orm_profile()};
    const auto mask_profiles = std::array<const stingray::texture::TextureProfile*, 3>{
        &stingray::texture::substance_basic_bca_profile(), &stingray::texture::substance_basic_nm_profile(),
        &stingray::texture::substance_basic_orm_profile()};
    const auto profiles = std::array<const stingray::texture::TextureProfile*, 4>{
        &stingray::texture::substance_basic_bc_profile(), &stingray::texture::pbr_emissive_em_profile(),
        &stingray::texture::substance_basic_nm_profile(), &stingray::texture::substance_basic_orm_profile()};
    const ResourceKey basic_shader_provider{"material", "content/parent_materials/substance_basic"};
    const ResourceKey basic_parent_material{"material", "#ID[cfe7f61e8e475a1a]"};
    const ResourceKey shader_provider_material{"material", "#ID[eba5d203d733a235]"};
    const ResourceKey parent_material{"material", "#ID[e04dba68a38d1429]"};
    const ResourceKey textureless_emissive_provider{
        "material", opaque_id(stingray::material::emissive_shader_provider_material_hash())};
    bool basic_external_added = false;
    bool emissive_external_added = false;
    bool textureless_emissive_external_added = false;
    bool mask_external_added = false;
    const auto add_basic_externals = [&]() {
        if (basic_external_added) return;
        graph.external.push_back({basic_shader_provider, false});
        graph.external.push_back({basic_parent_material, false});
        basic_external_added = true;
    };
    const auto add_emissive_externals = [&]() {
        if (emissive_external_added) return;
        graph.external.push_back({shader_provider_material, false});
        graph.external.push_back({parent_material, false});
        emissive_external_added = true;
    };
    const auto add_textureless_emissive_external = [&]() {
        if (textureless_emissive_external_added) return;
        graph.external.push_back({textureless_emissive_provider, false});
        textureless_emissive_external_added = true;
    };
    const ResourceKey mask_shader_provider{"material", "#ID[e03ea781c4162268]"};
    const ResourceKey mask_parent_material{"material", "#ID[345c8a27a643dc8e]"};
    const auto add_mask_externals = [&]() {
        if (mask_external_added) return;
        graph.external.push_back({mask_shader_provider, false});
        graph.external.push_back({mask_parent_material, false});
        mask_external_added = true;
    };
    const ResourceKey transparent_shader_provider{"material", "content/parent_materials/substance_basic_transparent"};
    const ResourceKey transparent_parent_material{"material", "#ID[40bc90ba5e9b0852]"};
    bool transparent_external_added = false;
    const auto add_transparent_externals = [&]() {
        if (transparent_external_added) return;
        graph.external.push_back({transparent_shader_provider, false});
        graph.external.push_back({transparent_parent_material, false});
        transparent_external_added = true;
    };
    const std::array<const char*, 4> channels{"bc", "em", "nm", "orm"};
    auto build_one = [&](const MaterialInfo& material, const std::string& stem, const std::string& slot) {
        const bool emits = material.emissive_strength > 0.0f &&
            std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; });
        const bool textureless = material.base_color_texture.texture < 0 &&
            material.metallic_roughness_texture.texture < 0 && material.normal_texture.texture < 0 &&
            material.occlusion_texture.texture < 0 && material.emissive_texture.texture < 0 &&
            material.diffuse_texture.texture < 0 && material.specular_glossiness_texture.texture < 0;
        const bool black_base = std::abs(material.base_color[0]) <= 1e-6f &&
            std::abs(material.base_color[1]) <= 1e-6f && std::abs(material.base_color[2]) <= 1e-6f &&
            std::abs(material.base_color[3] - 1.0f) <= 1e-6f;
        if (material.intent == MaterialInfo::Intent::Emissive) {
            if (material.alpha_mode != "OPAQUE" || !textureless || !black_base || !emits) {
                error = "direct Blender Emission material requires opaque, textureless, black-base glTF emission";
                return false;
            }
            add_textureless_emissive_external();
            stingray::material::EmissiveProfileSpec spec;
            spec.color = material.emissive;
            spec.intensity = material.emissive_strength;
            spec.multiplier = 1.0f;
            spec.surface_material = material.surface_material == "default"
                ? "plastic" : material.surface_material;
            const auto key = context.generated_key("material", stem);
            stingray::material::MaterialStream resource;
            if (!stingray::material::build_emissive_profile(spec, resource, error)) return false;
            graph.owned.push_back({key, stem + ".material", std::move(resource),
                {textureless_emissive_provider}});
            bindings.push_back({slot, key.name});
            return true;
        }
        std::array<stingray::texture::ImageRGBA, 4> images;
        const bool masked = material.alpha_mode == "MASK";
        const bool transparent = material.alpha_mode == "BLEND";
        if (!prepare_core_images(scene, material, images, error, masked || transparent,
                                 emits && !transparent)) return false;
        if (transparent) {
            add_transparent_externals();
            stingray::material::TransparentProfileSpec spec;
            spec.texture_hashes = {};
            spec.surface_material = material.surface_material;
            const std::array<std::size_t, 3> image_indices{{0, 2, 3}};
            const std::array<const char*, 3> transparent_channels{{"bca", "nm", "orm"}};
            std::vector<ResourceKey> dependencies{transparent_shader_provider, transparent_parent_material};
            for (std::size_t i = 0; i < transparent_channels.size(); ++i) {
                const auto texture_stem = stem + "_" + transparent_channels[i];
                const auto key = context.generated_key("texture", texture_stem);
                graph.owned.push_back({key, texture_stem + ".texture",
                    TextureResource{std::move(images[image_indices[i]]), *mask_profiles[i]}, {}});
                spec.texture_hashes[i] = stingray::resource_name_hash(key.name);
                dependencies.push_back(key);
            }
            const auto key = context.generated_key("material", stem);
            stingray::material::MaterialStream resource;
            if (!stingray::material::build_transparent_profile(spec, resource, error)) return false;
            graph.owned.push_back({key, stem + ".material", std::move(resource), std::move(dependencies)});
            bindings.push_back({slot, key.name});
            return true;
        }
        if (masked && !emits) {
            add_mask_externals();
            stingray::material::MaskProfileSpec spec;
            spec.texture_hashes = {};
            spec.alpha_cutoff = material.alpha_cutoff;
            spec.surface_material = material.surface_material;
            const std::array<std::size_t, 3> image_indices{{0, 2, 3}};
            const std::array<const char*, 3> mask_channels{{"bca", "nm", "orm"}};
            std::vector<ResourceKey> dependencies{mask_shader_provider, mask_parent_material};
            for (std::size_t i = 0; i < mask_channels.size(); ++i) {
                const auto texture_stem = stem + "_" + mask_channels[i];
                const auto key = context.generated_key("texture", texture_stem);
                graph.owned.push_back({key, texture_stem + ".texture",
                    TextureResource{std::move(images[image_indices[i]]), *mask_profiles[i]}, {}});
                spec.texture_hashes[i] = stingray::resource_name_hash(key.name);
                dependencies.push_back(key);
            }
            const auto key = context.generated_key("material", stem);
            stingray::material::MaterialStream resource;
            if (!stingray::material::build_mask_profile(spec, resource, error)) return false;
            graph.owned.push_back({key, stem + ".material", std::move(resource), std::move(dependencies)});
            bindings.push_back({slot, key.name});
            return true;
        }
        if (!emits) {
            add_basic_externals();
            stingray::material::InheritedMaterialSpec spec;
            spec.shader_provider_material_hash = stingray::id64(basic_shader_provider.name);
            spec.parent_material_hash = stingray::resource_name_hash(basic_parent_material.name);
            spec.surface_material = material.surface_material;
            const std::array<std::size_t, 3> image_indices{{0, 2, 3}};
            const std::array<const char*, 3> basic_channels{{"bc", "nm", "orm"}};
            std::vector<ResourceKey> dependencies{basic_shader_provider, basic_parent_material};
            for (std::size_t i = 0; i < basic_channels.size(); ++i) {
                const auto texture_stem = stem + "_" + basic_channels[i];
                const auto key = context.generated_key("texture", texture_stem);
                graph.owned.push_back({key, texture_stem + ".texture",
                    TextureResource{std::move(images[image_indices[i]]), *basic_profiles[i]}, {}});
                spec.textures.push_back({stingray::id32_from_id64(basic_channels[i]), stingray::id64(key.name)});
                dependencies.push_back(key);
            }
            const auto key = context.generated_key("material", stem);
            stingray::material::MaterialStream resource;
            if (!stingray::material::build_inherited_material(spec, resource, error)) return false;
            graph.owned.push_back({key, stem + ".material", std::move(resource), std::move(dependencies)});
            bindings.push_back({slot, key.name});
            return true;
        }
        add_emissive_externals();
        stingray::material::PbrEmissiveProfileSpec spec;
        spec.texture_hashes = {};
        spec.color = material.emissive;
        spec.intensity = material.emissive_strength;
        spec.surface_material = material.surface_material;
        std::vector<ResourceKey> dependencies{shader_provider_material, parent_material};
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const auto texture_stem = stem + "_" + channels[i];
            const auto key = context.generated_key("texture", texture_stem);
            graph.owned.push_back({key, texture_stem + ".texture", TextureResource{std::move(images[i]), *profiles[i]}, {}});
            spec.texture_hashes[i] = stingray::resource_name_hash(key.name);
            dependencies.push_back(key);
        }
        const auto key = context.generated_key("material", stem);
        stingray::material::MaterialStream resource;
        if (!stingray::material::build_pbr_emissive_profile(spec, resource, error)) return false;
        graph.owned.push_back({key, stem + ".material", std::move(resource), std::move(dependencies)});
        bindings.push_back({slot, key.name});
        return true;
    };
    for (const auto i : used_material_indices(scene)) {
        const auto& slot = slots->material_slots[i];
        if (slot.empty()) { error = "used material has no lowered slot"; return false; }
        const auto& material = scene.materials[i];
        if (material.intent == MaterialInfo::Intent::Donor) {
            if (!build_donor_material(scene, material, context,
                    context.file_base() + "_material_" + std::to_string(i), slot,
                    graph, bindings, error)) return false;
            continue;
        }
        if (material.intent == MaterialInfo::Intent::Template) {
            error = "material " + std::to_string(i) + " requests named template '" +
                material.external_resource + "', but no qualified Darktide material template catalog is available";
            return false;
        }
        if (material.intent == MaterialInfo::Intent::External) {
            const ResourceKey key{"material", material.external_resource};
            if (!key.valid()) {
                error = "material " + std::to_string(i) + " has invalid external material resource identity: " +
                    material.external_resource;
                return false;
            }
            graph.external.push_back({key, true});
            bindings.push_back({slot, key.name});
            continue;
        }
        if (!build_one(scene.materials[i], context.file_base() + "_material_" + std::to_string(i), slot)) return false;
    }
    if (std::any_of(scene.primitives.begin(), scene.primitives.end(), [](const Primitive& p) { return p.material < 0; })) {
        const auto& slot = slots->missing_slot;
        if (slot.empty()) { error = "default material has no lowered slot"; return false; }
        if (!build_one(MaterialInfo{}, context.file_base() + "_material_default", slot)) return false;
    }
    return true;
}
}
