#include "processing/texture_processor.h"
#include "processing/image_sampler.h"
#include "processing/tangent_generator.h"
#include "processing/topology_canonicalizer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <iterator>

namespace dtglb::processing {
namespace {
// Compute atlas sample coordinates in double precision.  UV corners are
// authored floats, so the subtraction/interpolation itself can overflow a
// float even when every authored value is finite.  Such a result is lowered
// to the deterministic constant sample used by the atlas fallback.  A
// non-finite authored corner remains a hard input error.
bool checked_atlas_uv(const std::array<std::array<float, 2>, 3>& corners,
                      float tx, float ty,
                      std::array<float, 2>& uv,
                      std::array<float, 2>& ddx,
                      std::array<float, 2>& ddy,
                      bool& overflow,
                      std::string& error) {
    overflow = false;
    for (const auto& corner : corners) {
        if (!std::isfinite(corner[0]) || !std::isfinite(corner[1])) {
            error = "image sampler received non-finite coordinates or transform";
            return false;
        }
    }
    const double x = tx;
    const double y = ty;
    const double third = 3.0;
    const auto calculate = [&](std::size_t component, double& out_uv, double& out_ddx, double& out_ddy) {
        const double a = corners[0][component];
        const double b = corners[1][component];
        const double c = corners[2][component];
        out_uv = a + x * (b - a) + y * (c - a);
        out_ddx = (b - a) / third;
        out_ddy = (c - a) / third;
    };
    for (std::size_t component = 0; component < 2; ++component) {
        double sample = 0.0, x_derivative = 0.0, y_derivative = 0.0;
        calculate(component, sample, x_derivative, y_derivative);
        if (!std::isfinite(sample) || !std::isfinite(x_derivative) || !std::isfinite(y_derivative) ||
            std::abs(sample) > std::numeric_limits<float>::max() ||
            std::abs(x_derivative) > std::numeric_limits<float>::max() ||
            std::abs(y_derivative) > std::numeric_limits<float>::max()) {
            overflow = true;
            uv = {0.0f, 0.0f};
            ddx = {0.0f, 0.0f};
            ddy = {0.0f, 0.0f};
            return true;
        }
        uv[component] = static_cast<float>(sample);
        ddx[component] = static_cast<float>(x_derivative);
        ddy[component] = static_cast<float>(y_derivative);
    }
    return true;
}

const VertexChannel* find_channel(const std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set) {
    for (const auto& channel : channels) if (channel.semantic == semantic && channel.set == set) return &channel;
    return nullptr;
}

bool atlas_budget_error(const std::string& error) {
    return error == "canonical material atlas exceeds one native 8192-pixel page" ||
        error == "canonical material atlas page capacity overflow" ||
        error == "canonical material atlas vertex/index capacity overflow" ||
        error == "canonical material atlas dimensions overflow addressable memory" ||
        error == "canonical material atlas allocation failed" ||
        error == "canonical material atlas allocation exceeds the container limit" ||
        error == "image sampler mip allocation failed" ||
        error == "image sampler mip allocation is too large";
}

bool clone_material_without_textures(Scene& scene, std::size_t primitive_index) {
    auto& primitive = scene.primitives[primitive_index];
    MaterialInfo material;
    if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size())
        material = scene.materials[static_cast<std::size_t>(primitive.material)];

    const auto had_binding = [](const TextureBinding& binding) { return binding.texture >= 0; };
    const bool discarded_binding =
        had_binding(material.base_color_texture) || had_binding(material.metallic_roughness_texture) ||
        had_binding(material.normal_texture) || had_binding(material.occlusion_texture) ||
        had_binding(material.emissive_texture) || had_binding(material.diffuse_texture) ||
        had_binding(material.specular_glossiness_texture) || had_binding(material.clearcoat_texture) ||
        had_binding(material.clearcoat_roughness_texture) || had_binding(material.clearcoat_normal_texture) ||
        had_binding(material.transmission_texture) || had_binding(material.thickness_texture) ||
        had_binding(material.sheen_color_texture) || had_binding(material.sheen_roughness_texture) ||
        had_binding(material.specular_texture) || had_binding(material.specular_color_texture) ||
        had_binding(material.iridescence_texture) || had_binding(material.iridescence_thickness_texture) ||
        had_binding(material.anisotropy_texture) || had_binding(material.diffuse_transmission_texture) ||
        had_binding(material.diffuse_transmission_color_texture);
    const bool discarded_profile = material.has_pbr_specular_glossiness || material.has_ior ||
        material.has_clearcoat || material.has_transmission || material.has_volume || material.has_sheen ||
        material.has_specular || material.has_iridescence || material.has_anisotropy ||
        material.has_diffuse_transmission || material.has_dispersion;
    const auto drop = [](TextureBinding& binding) { binding = {}; };
    drop(material.base_color_texture);
    drop(material.metallic_roughness_texture);
    drop(material.normal_texture);
    drop(material.occlusion_texture);
    drop(material.emissive_texture);
    drop(material.diffuse_texture);
    drop(material.specular_glossiness_texture);
    drop(material.clearcoat_texture);
    drop(material.clearcoat_roughness_texture);
    drop(material.clearcoat_normal_texture);
    drop(material.transmission_texture);
    drop(material.thickness_texture);
    drop(material.sheen_color_texture);
    drop(material.sheen_roughness_texture);
    drop(material.specular_texture);
    drop(material.specular_color_texture);
    drop(material.iridescence_texture);
    drop(material.iridescence_thickness_texture);
    drop(material.anisotropy_texture);
    drop(material.diffuse_transmission_texture);
    drop(material.diffuse_transmission_color_texture);
    // Capacity fallback is also used by conversions that would otherwise
    // lower extension textures.  Keep their scalar values, but remove the
    // extension profile markers so the cloned material is native-profile
    // representable after its texture bindings are dropped.
    material.has_pbr_specular_glossiness = false;
    material.has_ior = false;
    material.has_clearcoat = false;
    material.has_transmission = false;
    material.has_volume = false;
    material.has_sheen = false;
    material.has_specular = false;
    material.has_iridescence = false;
    material.has_anisotropy = false;
    material.has_diffuse_transmission = false;
    material.has_dispersion = false;
    material.name = "__canonical_material_capacity_fallback_" + std::to_string(primitive_index);
    scene.materials.push_back(std::move(material));
    primitive.material = static_cast<int>(scene.materials.size() - 1);
    primitive.material_name = scene.materials.back().name;
    return discarded_binding || discarded_profile;
}
}

bool fold_constant_color0(const Primitive& primitive, const std::array<float, 4>& base,
                          std::array<float, 4>& folded, ConstantColorBakeStatus& status, std::string& error) {
    status = ConstantColorBakeStatus::NotApplicable;
    if (!std::all_of(base.begin(), base.end(), [](float value) { return std::isfinite(value); })) { error = "base color factor contains a non-finite value"; return false; }
    const auto& channels = primitive.source_channels.empty() ? primitive.channels : primitive.source_channels;
    const auto* color = find_channel(channels, VertexChannel::Semantic::Color, 0);
    if (!color) return true;
    if (color->components != 3 && color->components != 4) { error = "COLOR_0 must have exactly three or four components"; return false; }
    const auto* position = find_channel(channels, VertexChannel::Semantic::Position, 0);
    if (position && (position->components == 0 || position->values.size() % position->components != 0)) { error = "POSITION has an invalid component count"; return false; }
    const std::size_t vertices = position ? position->values.size() / position->components : color->values.size() / color->components;
    if (vertices == 0 || color->values.empty() || color->values.size() != vertices * color->components) { error = "COLOR_0 row count does not match the primitive vertex count"; return false; }
    if (!std::all_of(color->values.begin(), color->values.end(), [](float value) { return std::isfinite(value); })) { error = "COLOR_0 contains a non-finite value"; return false; }
    const std::array<float, 4> constant{color->values[0], color->values[1], color->values[2], color->components == 4 ? color->values[3] : 1.0f};
    for (std::size_t vertex = 1; vertex < vertices; ++vertex) for (std::uint32_t component = 0; component < color->components; ++component)
        if (color->values[vertex * color->components + component] != constant[component]) return true;
    folded = base;
    for (std::size_t component = 0; component < 4; ++component) folded[component] *= constant[component];
    status = ConstantColorBakeStatus::Applied;
    return true;
}

bool remove_consumed_color0(const Primitive& source, Primitive& copy, std::string& error) {
    copy = source;
    const auto remove = [](std::vector<VertexChannel>& channels) { channels.erase(std::remove_if(channels.begin(), channels.end(), [](const VertexChannel& channel) { return channel.semantic == VertexChannel::Semantic::Color && channel.set == 0; }), channels.end()); };
    remove(copy.source_channels); remove(copy.channels); error.clear(); return true;
}

bool canonicalize_constant_color0(Scene& scene, bool& applied, std::string& error) {
    applied = false;
    const auto has_color = [](const Primitive& primitive, std::uint32_t set) {
        const auto scan = [set](const std::vector<VertexChannel>& channels) {
            return std::any_of(channels.begin(), channels.end(), [set](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Color && channel.set == set;
            });
        };
        return scan(primitive.source_channels) || scan(primitive.channels);
    };
    for (std::size_t primitive_index = 0; primitive_index < scene.primitives.size(); ++primitive_index) {
        auto& primitive = scene.primitives[primitive_index];
        if (!has_color(primitive, 0)) continue;
        bool higher_set = false;
        for (std::uint32_t set = 1; set < 8; ++set) higher_set = higher_set || has_color(primitive, set);
        if (higher_set) continue;
        const MaterialInfo* source_material = nullptr;
        MaterialInfo default_material;
        if (primitive.material < 0) source_material = &default_material;
        else if (static_cast<std::size_t>(primitive.material) < scene.materials.size())
            source_material = &scene.materials[static_cast<std::size_t>(primitive.material)];
        else { error = "primitive material index is outside the decoded material table"; return false; }
        std::array<float, 4> folded{};
        ConstantColorBakeStatus status{};
        if (!fold_constant_color0(primitive, source_material->base_color, folded, status, error)) return false;
        if (status != ConstantColorBakeStatus::Applied) continue;
        MaterialInfo private_material = *source_material;
        private_material.base_color = folded;
        if (private_material.has_pbr_specular_glossiness) {
            const auto& source_channels = primitive.source_channels.empty()
                ? primitive.channels : primitive.source_channels;
            const auto* source_color = find_channel(source_channels, VertexChannel::Semantic::Color, 0);
            for (std::size_t component = 0; component < 4; ++component)
                private_material.diffuse_factor[component] *= component < source_color->components
                    ? source_color->values[component] : 1.0f;
        }
        private_material.name = "__canonical_vertex_color_material_" + std::to_string(primitive_index);
        scene.materials.push_back(std::move(private_material));
        primitive.material = static_cast<int>(scene.materials.size() - 1);
        primitive.material_name = scene.materials.back().name;
        Primitive cleaned;
        if (!remove_consumed_color0(primitive, cleaned, error)) return false;
        primitive.source_channels = std::move(cleaned.source_channels);
        primitive.channels = std::move(cleaned.channels);
        applied = true;
    }
    return true;
}

namespace {
std::uint8_t srgb_encode(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float encoded = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::clamp<long>(std::lround(encoded * 255.0f), 0, 255));
}
float srgb_decode(float value) { return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f); }

const VertexChannel* channel_in(const std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set) {
    return find_channel(channels, semantic, set);
}

bool unsupported_material(const MaterialInfo& material) {
    return (material.alpha_mode != "OPAQUE" && material.alpha_mode != "MASK" && material.alpha_mode != "BLEND") ||
        material.has_ior || material.has_clearcoat ||
        material.has_volume || material.has_sheen || material.has_specular ||
        material.has_iridescence || material.has_anisotropy || material.has_diffuse_transmission ||
        material.has_dispersion;
}

bool load_rgba8(const Scene& scene, const TextureBinding& binding, RGBA8Payload& payload, SamplerInfo& sampler, std::string& error) {
    if (binding.texture < 0 || static_cast<std::size_t>(binding.texture) >= scene.textures.size()) { error = "material texture index is invalid"; return false; }
    const auto& texture = scene.textures[static_cast<std::size_t>(binding.texture)];
    const int source = texture.basisu_source >= 0 ? texture.basisu_source :
        (texture.webp_source >= 0 ? texture.webp_source :
            (texture.dds_source >= 0 ? texture.dds_source : texture.source));
    if (source < 0 || static_cast<std::size_t>(source) >= scene.images.size()) { error = "material image source is invalid"; return false; }
    const auto& image = scene.images[static_cast<std::size_t>(source)];
    if (image.rgba8) payload = *image.rgba8;
    else {
        stingray::texture::ImageRGBA decoded;
        if (!stingray::texture::decode_image_rgba(image.bytes, image.mime_type, decoded, error)) return false;
        payload = {decoded.width, decoded.height, std::move(decoded.pixels)};
    }
    sampler = {};
    if (texture.sampler >= 0) {
        if (static_cast<std::size_t>(texture.sampler) >= scene.samplers.size()) { error = "material sampler index is invalid"; return false; }
        sampler = scene.samplers[static_cast<std::size_t>(texture.sampler)];
    }
    return true;
}

bool direct_core_texture_binding(const Scene& scene, const TextureBinding& binding) {
    const int texcoord = binding.transform.texcoord >= 0 ? binding.transform.texcoord : binding.texcoord;
    if (binding.texture < 0 || texcoord != 0 || std::abs(binding.transform.offset[0]) > 1e-6f ||
        std::abs(binding.transform.offset[1]) > 1e-6f || std::abs(binding.transform.scale[0] - 1.0f) > 1e-6f ||
        std::abs(binding.transform.scale[1] - 1.0f) > 1e-6f || std::abs(binding.transform.rotation) > 1e-6f ||
        static_cast<std::size_t>(binding.texture) >= scene.textures.size()) return false;
    const auto& texture = scene.textures[static_cast<std::size_t>(binding.texture)];
    const int source = texture.basisu_source >= 0 ? texture.basisu_source :
        (texture.webp_source >= 0 ? texture.webp_source : (texture.dds_source >= 0 ? texture.dds_source : texture.source));
    if (source < 0 || static_cast<std::size_t>(source) >= scene.images.size()) return false;
    if (texture.sampler < 0) return true;
    if (static_cast<std::size_t>(texture.sampler) >= scene.samplers.size()) return false;
    const auto& sampler = scene.samplers[static_cast<std::size_t>(texture.sampler)];
    return sampler.wrap_s == 10497 && sampler.wrap_t == 10497 &&
        (sampler.mag_filter == 0 || sampler.mag_filter == 9729) &&
        (sampler.min_filter == 0 || sampler.min_filter == 9987);
}

bool direct_core_material_textures(const Scene& scene, const MaterialInfo& material) {
    const std::array<const TextureBinding*, 5> bindings{{&material.base_color_texture,
        &material.metallic_roughness_texture, &material.normal_texture,
        &material.occlusion_texture, &material.emissive_texture}};
    const bool emits = material.emissive_strength > 0.0f &&
        std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; });
    for (std::size_t i = 0; i < bindings.size(); ++i)
        if (bindings[i]->texture >= 0 && (i != 4 || emits) && !direct_core_texture_binding(scene, *bindings[i])) return false;
    return true;
}

int effective_texcoord(const TextureBinding& binding) {
    return binding.transform.texcoord < 0 ? binding.texcoord : binding.transform.texcoord;
}

bool same_transform(const TextureBinding& a, const TextureBinding& b) {
    return effective_texcoord(a) == effective_texcoord(b) &&
        a.transform.offset == b.transform.offset && a.transform.scale == b.transform.scale &&
        a.transform.rotation == b.transform.rotation;
}

bool same_sampler(const SamplerInfo& a, const SamplerInfo& b) {
    return a.mag_filter == b.mag_filter && a.min_filter == b.min_filter &&
        a.wrap_s == b.wrap_s && a.wrap_t == b.wrap_t;
}

float color_brightness(const std::array<float, 3>& color) {
    return std::sqrt(0.299f * color[0] * color[0] +
                     0.587f * color[1] * color[1] +
                     0.114f * color[2] * color[2]);
}

float specgloss_metallic(const std::array<float, 3>& diffuse,
                         const std::array<float, 3>& specular) {
    constexpr float dielectric = 0.04f;
    const float specular_brightness = color_brightness(specular);
    // Khronos' reference conversion explicitly maps sub-dielectric specular
    // colors to non-metal.  Omitting this guard made dark texels metallic.
    if (specular_brightness < dielectric) return 0.0f;
    const float one_minus_specular = 1.0f - std::max({specular[0], specular[1], specular[2]});
    const float b = color_brightness(diffuse) * one_minus_specular / (1.0f - dielectric) +
        specular_brightness - 2.0f * dielectric;
    const float c = dielectric - specular_brightness;
    const float discriminant = std::max(0.0f, b * b - 4.0f * dielectric * c);
    return std::clamp((-b + std::sqrt(discriminant)) / (2.0f * dielectric), 0.0f, 1.0f);
}

std::array<float, 3> specgloss_base(const std::array<float, 3>& diffuse,
                                    const std::array<float, 3>& specular,
                                    float metallic) {
    constexpr float dielectric = 0.04f;
    const float one_minus_specular = 1.0f - std::max({specular[0], specular[1], specular[2]});
    const float diffuse_denominator = std::max(1e-6f, (1.0f - dielectric) *
        std::max(1e-6f, 1.0f - metallic));
    const float metallic_denominator = std::max(1e-6f, metallic);
    const float blend = metallic * metallic;
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float from_diffuse = diffuse[channel] * one_minus_specular / diffuse_denominator;
        const float from_specular = (specular[channel] - dielectric * (1.0f - metallic)) /
            metallic_denominator;
        result[channel] = std::clamp(from_diffuse * (1.0f - blend) + from_specular * blend,
                                     0.0f, 1.0f);
    }
    return result;
}

enum class DirectSpecGlossStatus { Applied, NotApplicable };

bool convert_specgloss_texture_space(Scene& scene, std::size_t material_index,
                                     DirectSpecGlossStatus& status, std::string& error) {
    status = DirectSpecGlossStatus::NotApplicable;
    if (material_index >= scene.materials.size()) return true;
    auto& material = scene.materials[material_index];
    if (!material.has_pbr_specular_glossiness) return true;
    const bool has_diffuse = material.diffuse_texture.texture >= 0;
    const bool has_specgloss = material.specular_glossiness_texture.texture >= 0;
    if (!has_diffuse && !has_specgloss) return true;

    RGBA8Payload diffuse_image, specgloss_image;
    SamplerInfo diffuse_sampler, specgloss_sampler;
    if (has_diffuse && !load_rgba8(scene, material.diffuse_texture, diffuse_image, diffuse_sampler, error)) return false;
    if (has_specgloss && !load_rgba8(scene, material.specular_glossiness_texture, specgloss_image, specgloss_sampler, error)) return false;
    if (has_diffuse && has_specgloss &&
        (!same_transform(material.diffuse_texture, material.specular_glossiness_texture) ||
         !same_sampler(diffuse_sampler, specgloss_sampler) ||
         diffuse_image.width != specgloss_image.width || diffuse_image.height != specgloss_image.height)) {
        return true;
    }
    const auto& domain = has_diffuse ? material.diffuse_texture : material.specular_glossiness_texture;
    const auto& source = has_diffuse ? diffuse_image : specgloss_image;
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(source.width) * source.height;
    if (source.width == 0 || source.height == 0 || pixel_count > std::numeric_limits<std::size_t>::max() / 4u) {
        error = "specular-glossiness texture dimensions are invalid";
        return false;
    }
    const auto expected = static_cast<std::size_t>(pixel_count * 4u);
    if ((has_diffuse && diffuse_image.bytes.size() != expected) ||
        (has_specgloss && specgloss_image.bytes.size() != expected)) {
        error = "specular-glossiness decoded image byte count is invalid";
        return false;
    }

    RGBA8Payload base{source.width, source.height, std::vector<std::uint8_t>(expected, 255)};
    RGBA8Payload orm{source.width, source.height, std::vector<std::uint8_t>(expected, 255)};
    for (std::size_t pixel = 0; pixel < expected; pixel += 4) {
        std::array<float, 3> diffuse{}, specular{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float diffuse_texel = has_diffuse
                ? srgb_decode(diffuse_image.bytes[pixel + channel] / 255.0f) : 1.0f;
            const float specular_texel = has_specgloss
                ? srgb_decode(specgloss_image.bytes[pixel + channel] / 255.0f) : 1.0f;
            diffuse[channel] = std::clamp(diffuse_texel * material.diffuse_factor[channel], 0.0f, 1.0f);
            specular[channel] = std::clamp(specular_texel * material.specular_factor[channel], 0.0f, 1.0f);
        }
        const float metallic = specgloss_metallic(diffuse, specular);
        const auto base_color = specgloss_base(diffuse, specular, metallic);
        for (std::size_t channel = 0; channel < 3; ++channel)
            base.bytes[pixel + channel] = srgb_encode(base_color[channel]);
        const float diffuse_alpha = has_diffuse ? diffuse_image.bytes[pixel + 3] / 255.0f : 1.0f;
        base.bytes[pixel + 3] = static_cast<std::uint8_t>(std::clamp<long>(
            std::lround(std::clamp(diffuse_alpha * material.diffuse_factor[3], 0.0f, 1.0f) * 255.0f), 0, 255));
        const float gloss = has_specgloss ? specgloss_image.bytes[pixel + 3] / 255.0f : 1.0f;
        const float roughness = std::clamp(1.0f - gloss * material.glossiness_factor, 0.0f, 1.0f);
        orm.bytes[pixel] = 255;
        orm.bytes[pixel + 1] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(roughness * 255.0f), 0, 255));
        orm.bytes[pixel + 2] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(metallic * 255.0f), 0, 255));
        orm.bytes[pixel + 3] = 255;
    }

    const int sampler_index = scene.textures[static_cast<std::size_t>(domain.texture)].sampler;
    const auto append = [&](const char* suffix, RGBA8Payload payload) {
        ImageInfo image;
        image.name = "__specgloss_direct_" + std::string(suffix) + "_image_" + std::to_string(material_index);
        image.mime_type = "image/raw-rgba8";
        image.rgba8 = std::move(payload);
        const int image_index = static_cast<int>(scene.images.size());
        scene.images.push_back(std::move(image));
        TextureInfo texture;
        texture.name = "__specgloss_direct_" + std::string(suffix) + "_texture_" + std::to_string(material_index);
        texture.source = image_index;
        texture.sampler = sampler_index;
        const int texture_index = static_cast<int>(scene.textures.size());
        scene.textures.push_back(std::move(texture));
        return texture_index;
    };
    TextureBinding base_binding = domain;
    TextureBinding orm_binding = domain;
    base_binding.texture = append("base", std::move(base));
    orm_binding.texture = append("orm", std::move(orm));
    material.base_color = {1, 1, 1, 1};
    material.base_color_texture = base_binding;
    material.metallic = 1.0f;
    material.roughness = 1.0f;
    material.metallic_roughness_texture = orm_binding;
    material.has_pbr_specular_glossiness = false;
    material.diffuse_texture = {};
    material.specular_glossiness_texture = {};
    status = DirectSpecGlossStatus::Applied;
    return true;
}

template<class Channel> bool remap_channel(Channel& channel, const std::vector<std::uint32_t>& corners, std::size_t source_vertices, std::string& error) {
    if (channel.components == 0 || source_vertices > std::numeric_limits<std::size_t>::max() / channel.components ||
        channel.values.size() != source_vertices * channel.components ||
        corners.size() > std::numeric_limits<std::size_t>::max() / channel.components) {
        error = "row-based primitive channel count is malformed"; return false;
    }
    std::vector<float> values; values.reserve(corners.size() * channel.components);
    for (const auto index : corners) values.insert(values.end(), channel.values.begin() + static_cast<std::ptrdiff_t>(index * channel.components), channel.values.begin() + static_cast<std::ptrdiff_t>((index + 1) * channel.components));
    channel.values = std::move(values); return true;
}

bool remap_channels(std::vector<VertexChannel>& channels, const std::vector<std::uint32_t>& corners, std::size_t source_vertices, std::string& error) {
    for (auto& channel : channels) if (!remap_channel(channel, corners, source_vertices, error)) return false;
    return true;
}

bool allocate_atlas(std::uint32_t side, stingray::texture::ImageRGBA& image, std::string& error) {
    const auto pixels = static_cast<std::uint64_t>(side) * side;
    if (side == 0 || pixels > std::numeric_limits<std::size_t>::max() / 4u) {
        error = "canonical material atlas dimensions overflow addressable memory";
        return false;
    }
    try {
        image.width = image.height = side;
        image.pixels.assign(static_cast<std::size_t>(pixels * 4u), 255);
    } catch (const std::bad_alloc&) {
        error = "canonical material atlas allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "canonical material atlas allocation exceeds the container limit";
        return false;
    }
    return true;
}
}

bool canonicalize_varying_color0(const Scene& scene, std::size_t primitive_index,
                                 CanonicalAtlasResult& result, std::string& error) {
    result = {};
    if (primitive_index >= scene.primitives.size()) { error = "primitive index is outside the scene"; return false; }
    const auto& source = scene.primitives[primitive_index];
    const auto& channels = source.source_channels.empty() ? source.channels : source.source_channels;
    const auto* color = channel_in(channels, VertexChannel::Semantic::Color, 0);
    for (const auto& channel : source.source_channels) if (channel.semantic == VertexChannel::Semantic::Color && channel.set > 0) return true;
    for (const auto& channel : source.channels) if (channel.semantic == VertexChannel::Semantic::Color && channel.set > 0) return true;
    if (source.mode != 4) return true;
    MaterialInfo default_material;
    const MaterialInfo* material_ptr = source.material < 0 ? &default_material :
        (static_cast<std::size_t>(source.material) < scene.materials.size() ? &scene.materials[static_cast<std::size_t>(source.material)] : nullptr);
    if (!material_ptr) return true;
    const auto& material = *material_ptr;
    const bool has_core_texture = material.base_color_texture.texture >= 0 ||
        material.metallic_roughness_texture.texture >= 0 || material.occlusion_texture.texture >= 0 ||
        material.normal_texture.texture >= 0 || material.emissive_texture.texture >= 0 ||
        material.specular_glossiness_texture.texture >= 0 || material.transmission_texture.texture >= 0;
    if (unsupported_material(material) || (!color && !has_core_texture)) return true;
    if (color && color->components != 3 && color->components != 4) { error = "COLOR_0 must have exactly three or four components"; return false; }
    if (!std::all_of(material.base_color.begin(), material.base_color.end(), [](float value) { return std::isfinite(value); })) { error = "base color factor contains a non-finite value"; return false; }
    result.color_range_clamped = std::any_of(material.base_color.begin(), material.base_color.end(),
        [](float value) { return value < 0.0f || value > 1.0f; });
    const auto* position = channel_in(channels, VertexChannel::Semantic::Position, 0);
    if (!position || position->components == 0 || position->values.size() % position->components != 0) { error = "primitive POSITION channel is malformed"; return false; }
    const std::size_t vertex_count = position->values.size() / position->components;
    if (color && (color->values.size() != vertex_count * color->components || !std::all_of(color->values.begin(), color->values.end(), [](float value) { return std::isfinite(value); }))) { error = "COLOR_0 rows are malformed or non-finite"; return false; }
    if (color) result.color_range_clamped = result.color_range_clamped ||
        std::any_of(color->values.begin(), color->values.end(), [](float value) { return value < 0.0f || value > 1.0f; });
    if (source.indices.empty() || source.indices.size() % 3 != 0) { error = "triangle primitive indices are malformed"; return false; }
    for (const auto index : source.indices) if (index >= vertex_count) { error = "triangle index exceeds vertex count"; return false; }
    const std::size_t triangles = source.indices.size() / 3;
    // Atlas conversion deindexes triangles, so every corner becomes a native
    // vertex.  The downstream UNIT representation stores these indices as
    // uint16; reject the conversion before allocating or mutating anything.
    if (source.indices.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "canonical material atlas vertex/index capacity overflow";
        return false;
    }
    const std::size_t max_blocks = 2048u * 2048u;
    if (triangles > max_blocks) { error = "canonical material atlas exceeds one native 8192-pixel page"; return false; }
    std::uint32_t side = 512;
    while ((static_cast<std::size_t>(side / 4) * (side / 4) < triangles) && side < 8192) side *= 2;
    if (static_cast<std::size_t>(side / 4) * (side / 4) < triangles) { error = "canonical material atlas page capacity overflow"; return false; }
    PreparedImageSampler prepared_base;
    SamplerInfo base_sampler;
    RGBA8Payload base_image;
    if (material.base_color_texture.texture >= 0) {
        if (!load_rgba8(scene, material.base_color_texture, base_image, base_sampler, error) ||
            !prepare_image_sampler(base_image, base_sampler, ImageSemantic::SRGB, prepared_base, error)) return false;
    }
    const bool has_specgloss = material.has_pbr_specular_glossiness;
    const bool has_specgloss_texture = has_specgloss &&
        material.specular_glossiness_texture.texture >= 0;
    PreparedImageSampler prepared_specgloss;
    SamplerInfo specgloss_sampler;
    RGBA8Payload specgloss_image;
    if (has_specgloss_texture) {
        if (!load_rgba8(scene, material.specular_glossiness_texture, specgloss_image, specgloss_sampler, error) ||
            !prepare_image_sampler(specgloss_image, specgloss_sampler, ImageSemantic::SRGB, prepared_specgloss, error)) return false;
    }
    const bool has_transmission_texture = material.has_transmission && material.transmission_texture.texture >= 0;
    PreparedImageSampler prepared_transmission;
    SamplerInfo transmission_sampler;
    RGBA8Payload transmission_image;
    if (has_transmission_texture) {
        if (!load_rgba8(scene, material.transmission_texture, transmission_image, transmission_sampler, error) ||
            !prepare_image_sampler(transmission_image, transmission_sampler, ImageSemantic::Linear, prepared_transmission, error)) return false;
    }
    const std::vector<std::uint32_t> corners = source.indices;
    Primitive baked = source;
    if (!remap_channels(baked.source_channels, corners, vertex_count, error) || !remap_channels(baked.channels, corners, vertex_count, error)) return false;
    for (auto& target : baked.morph_targets) if (!remap_channels(target.channels, corners, vertex_count, error)) return false;
    for (auto& custom : baked.custom_attributes) {
        if (custom.components == 0 || vertex_count > std::numeric_limits<std::size_t>::max() / custom.components ||
            custom.values.size() != vertex_count * custom.components ||
            corners.size() > std::numeric_limits<std::size_t>::max() / custom.components) { error = "custom primitive attribute rows are malformed"; return false; }
        std::vector<float> values; values.reserve(corners.size() * custom.components);
        for (const auto index : corners) values.insert(values.end(), custom.values.begin() + static_cast<std::ptrdiff_t>(index * custom.components), custom.values.begin() + static_cast<std::ptrdiff_t>((index + 1) * custom.components));
        custom.values = std::move(values);
    }
    auto rewrite = [](std::vector<VertexChannel>& list, const std::vector<std::array<float, 2>>& uv) {
        list.erase(std::remove_if(list.begin(), list.end(), [](const VertexChannel& channel) {
            return (channel.semantic == VertexChannel::Semantic::Color && channel.set == 0) ||
                channel.semantic == VertexChannel::Semantic::Tangent;
        }), list.end());
        auto uv0 = std::find_if(list.begin(), list.end(), [](const VertexChannel& channel) {
            return channel.semantic == VertexChannel::Semantic::Texcoord && channel.set == 0;
        });
        if (uv0 == list.end()) {
            VertexChannel channel; channel.semantic = VertexChannel::Semantic::Texcoord;
            channel.components = 2; channel.values.resize(uv.size() * 2);
            list.push_back(std::move(channel)); uv0 = std::prev(list.end());
        }
        if (uv0->components < 2) return false;
        std::vector<float> values; values.reserve(uv.size() * uv0->components);
        for (const auto point : uv) { values.push_back(point[0]); values.push_back(point[1]); for (std::uint32_t c = 2; c < uv0->components; ++c) values.push_back(0.0f); }
        uv0->values = std::move(values); return true;
    };
    std::vector<std::array<float, 2>> uvs; uvs.reserve(corners.size());
    stingray::texture::ImageRGBA atlas;
    if (!allocate_atlas(side, atlas, error)) return false;
    std::optional<stingray::texture::ImageRGBA> specgloss_mr;
    if (has_specgloss) {
        specgloss_mr.emplace();
        if (!allocate_atlas(side, *specgloss_mr, error)) return false;
    }
    const std::size_t blocks_per_row = side / 4;
    for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
        const auto bx = triangle % blocks_per_row, by = triangle / blocks_per_row;
        const auto uv = [&](std::size_t x, std::size_t y) { return std::array<float, 2>{static_cast<float>(bx * 4 + x + 0.5) / side, static_cast<float>(by * 4 + y + 0.5) / side}; };
        uvs.push_back(uv(0, 0)); uvs.push_back(uv(3, 0)); uvs.push_back(uv(0, 3));
        const auto base = triangle * 3;
        std::array<std::array<float, 4>, 3> colors{};
        std::array<std::array<float, 4>, 3> vertex_colors{};
        const int uv_set = material.base_color_texture.transform.texcoord >= 0 ? material.base_color_texture.transform.texcoord : material.base_color_texture.texcoord;
        const auto* source_uv = channel_in(channels, VertexChannel::Semantic::Texcoord, static_cast<std::uint32_t>(std::max(0, uv_set)));
        if (material.base_color_texture.texture >= 0 && !source_uv) result.missing_texcoord_fallback = true;
        if (material.base_color_texture.texture >= 0 && source_uv && (source_uv->components < 2 || source_uv->values.size() != vertex_count * source_uv->components)) { error = "base color TEXCOORD set is malformed"; return false; }
        const int spec_uv_set = material.specular_glossiness_texture.transform.texcoord >= 0 ? material.specular_glossiness_texture.transform.texcoord : material.specular_glossiness_texture.texcoord;
        const auto* spec_source_uv = has_specgloss_texture ? channel_in(channels, VertexChannel::Semantic::Texcoord, static_cast<std::uint32_t>(std::max(0, spec_uv_set))) : nullptr;
        if (has_specgloss_texture && !spec_source_uv) result.missing_texcoord_fallback = true;
        if (has_specgloss_texture && spec_source_uv && (spec_source_uv->components < 2 || spec_source_uv->values.size() != vertex_count * spec_source_uv->components)) { error = "specular-glossiness TEXCOORD set is malformed"; return false; }
        const int transmission_uv_set = material.transmission_texture.transform.texcoord >= 0 ? material.transmission_texture.transform.texcoord : material.transmission_texture.texcoord;
        const auto* transmission_source_uv = has_transmission_texture ? channel_in(channels, VertexChannel::Semantic::Texcoord, static_cast<std::uint32_t>(std::max(0, transmission_uv_set))) : nullptr;
        if (has_transmission_texture && !transmission_source_uv) result.missing_texcoord_fallback = true;
        if (has_transmission_texture && transmission_source_uv && (transmission_source_uv->components < 2 || transmission_source_uv->values.size() != vertex_count * transmission_source_uv->components)) { error = "transmission TEXCOORD set is malformed"; return false; }
        std::array<std::array<float, 2>, 3> source_uvs{};
        std::array<std::array<float, 2>, 3> spec_source_uvs{};
        std::array<std::array<float, 2>, 3> transmission_source_uvs{};
        for (int corner = 0; corner < 3; ++corner) {
            const auto vertex = static_cast<std::size_t>(source.indices[base + corner]);
            if (source_uv) source_uvs[corner] = {source_uv->values[vertex * source_uv->components], source_uv->values[vertex * source_uv->components + 1]};
            if (spec_source_uv) spec_source_uvs[corner] = {spec_source_uv->values[vertex * spec_source_uv->components], spec_source_uv->values[vertex * spec_source_uv->components + 1]};
            if (transmission_source_uv) transmission_source_uvs[corner] = {transmission_source_uv->values[vertex * transmission_source_uv->components], transmission_source_uv->values[vertex * transmission_source_uv->components + 1]};
            const auto index = color ? vertex * color->components : 0;
            const float cr = color ? std::clamp(color->values[index], 0.0f, 1.0f) : 1.0f;
            const float cg = color ? std::clamp(color->values[index + 1], 0.0f, 1.0f) : 1.0f;
            const float cb = color ? std::clamp(color->values[index + 2], 0.0f, 1.0f) : 1.0f;
            const float ca = color && color->components == 4 ? std::clamp(color->values[index + 3], 0.0f, 1.0f) : 1.0f;
            vertex_colors[corner] = {cr, cg, cb, ca};
            colors[corner] = {std::clamp(material.base_color[0], 0.0f, 1.0f) * cr,
                              std::clamp(material.base_color[1], 0.0f, 1.0f) * cg,
                              std::clamp(material.base_color[2], 0.0f, 1.0f) * cb,
                              material.alpha_mode == "OPAQUE" ? 1.0f : std::clamp(material.base_color[3], 0.0f, 1.0f) * ca};
        }
        for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
            const float tx = static_cast<float>(x) / 3.0f, ty = static_cast<float>(y) / 3.0f;
            const auto pixel = (static_cast<std::size_t>(by * 4 + y) * side + bx * 4 + x) * 4;
            std::array<float, 4> sampled{1,1,1,1};
            if (material.base_color_texture.texture >= 0) {
                std::array<float, 2> uv{}, ddx{}, ddy{}; bool uv_overflow = false;
                if (!checked_atlas_uv(source_uvs, tx, ty, uv, ddx, ddy, uv_overflow, error)) return false;
                result.uv_overflow_clamped = result.uv_overflow_clamped || uv_overflow;
                ImageSample sample; if (!sample_image(prepared_base, material.base_color_texture, uv, ddx, ddy, sample, error)) return false; sampled = sample.rgba; for (int c = 0; c < 3; ++c) sampled[c] = srgb_decode(sampled[c]); result.sampler_filter_fallback = result.sampler_filter_fallback || sample.sampler_filter_fallback; result.uv_overflow_clamped = result.uv_overflow_clamped || sample.uv_overflow_clamped;
            }
            std::array<float, 3> core_color{};
            float core_metallic = material.metallic;
            float core_roughness = material.roughness;
            if (has_specgloss) {
                ImageSample sample; sample.rgba = {1, 1, 1, 1};
                if (has_specgloss_texture) {
                    std::array<float, 2> uv{}, ddx{}, ddy{}; bool uv_overflow = false;
                    if (!checked_atlas_uv(spec_source_uvs, tx, ty, uv, ddx, ddy, uv_overflow, error)) return false;
                    result.uv_overflow_clamped = result.uv_overflow_clamped || uv_overflow;
                    if (!sample_image(prepared_specgloss, material.specular_glossiness_texture, uv, ddx, ddy, sample, error)) return false;
                    result.sampler_filter_fallback = result.sampler_filter_fallback || sample.sampler_filter_fallback; result.uv_overflow_clamped = result.uv_overflow_clamped || sample.uv_overflow_clamped;
                }
                std::array<float, 3> diffuse{};
                std::array<float, 3> specular{};
                float diffuse_brightness = 0.0f, specular_brightness = 0.0f;
                for (std::size_t c = 0; c < 3; ++c) {
                    const float vertex_color = vertex_colors[0][c] + tx * (vertex_colors[1][c] - vertex_colors[0][c]) + ty * (vertex_colors[2][c] - vertex_colors[0][c]);
                    diffuse[c] = std::clamp(sampled[c] * material.diffuse_factor[c] * vertex_color, 0.0f, 1.0f);
                    specular[c] = std::clamp(srgb_decode(sample.rgba[c]) * material.specular_factor[c], 0.0f, 1.0f);
                    diffuse_brightness += diffuse[c] * diffuse[c] * (c == 0 ? 0.299f : c == 1 ? 0.587f : 0.114f);
                    specular_brightness += specular[c] * specular[c] * (c == 0 ? 0.299f : c == 1 ? 0.587f : 0.114f);
                }
                diffuse_brightness = std::sqrt(diffuse_brightness);
                specular_brightness = std::sqrt(specular_brightness);
                const float one_minus_specular = 1.0f - std::max({specular[0], specular[1], specular[2]});
                if (specular_brightness < 0.04f) {
                    core_metallic = 0.0f;
                } else {
                    const float a = 0.04f;
                    const float b = diffuse_brightness * one_minus_specular / 0.96f + specular_brightness - 0.08f;
                    const float c = 0.04f - specular_brightness;
                    const float discriminant = std::max(0.0f, b * b - 4.0f * a * c);
                    core_metallic = std::clamp((-b + std::sqrt(discriminant)) / (2.0f * a), 0.0f, 1.0f);
                }
                const float dielectric = std::max(1e-6f, 0.96f * std::max(1e-6f, 1.0f - core_metallic));
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float from_diffuse = diffuse[channel] * one_minus_specular / dielectric;
                    const float from_specular = (specular[channel] - 0.04f * (1.0f - core_metallic)) / std::max(1e-6f, core_metallic);
                    const float blend = core_metallic * core_metallic;
                    core_color[channel] = std::clamp(from_diffuse * (1.0f - blend) + from_specular * blend, 0.0f, 1.0f);
                }
                core_roughness = std::clamp(1.0f - sample.rgba[3] * material.glossiness_factor, 0.0f, 1.0f);
            } else {
                for (std::size_t c = 0; c < 3; ++c) core_color[c] = colors[0][c] + tx * (colors[1][c] - colors[0][c]) + ty * (colors[2][c] - colors[0][c]);
            }
            for (std::size_t c = 0; c < 3; ++c) {
                const float factor = has_specgloss ? core_color[c] : core_color[c] * sampled[c];
                atlas.pixels[pixel + c] = srgb_encode(factor);
            }
            const float alpha_factor = colors[0][3] + tx * (colors[1][3] - colors[0][3]) + ty * (colors[2][3] - colors[0][3]);
            float alpha = alpha_factor * sampled[3];
            if (has_transmission_texture) {
                std::array<float, 2> uv{}, ddx{}, ddy{}; bool uv_overflow = false;
                if (!checked_atlas_uv(transmission_source_uvs, tx, ty, uv, ddx, ddy, uv_overflow, error)) return false;
                result.uv_overflow_clamped = result.uv_overflow_clamped || uv_overflow;
                ImageSample sample; if (!sample_image(prepared_transmission, material.transmission_texture, uv, ddx, ddy, sample, error)) return false;
                result.sampler_filter_fallback = result.sampler_filter_fallback || sample.sampler_filter_fallback; result.uv_overflow_clamped = result.uv_overflow_clamped || sample.uv_overflow_clamped;
                alpha *= 1.0f - std::clamp(material.transmission * sample.rgba[0], 0.0f, 1.0f);
            }
            atlas.pixels[pixel + 3] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(alpha * 255.0f), 0, 255));
            if (specgloss_mr) {
                (*specgloss_mr).pixels[pixel] = 255;
                (*specgloss_mr).pixels[pixel + 1] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(core_roughness * 255.0f), 0, 255));
                (*specgloss_mr).pixels[pixel + 2] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(core_metallic * 255.0f), 0, 255));
            }
        }
    }
    const auto bake_domain = [&](const TextureBinding& binding, ImageSemantic semantic, std::optional<stingray::texture::ImageRGBA>& destination) -> bool {
        if (binding.texture < 0) return true;
        RGBA8Payload payload; SamplerInfo sampler; PreparedImageSampler prepared;
        if (!load_rgba8(scene, binding, payload, sampler, error) || !prepare_image_sampler(payload, sampler, semantic, prepared, error)) return false;
        stingray::texture::ImageRGBA image;
        if (!allocate_atlas(side, image, error)) return false;
        const int uv_set = binding.transform.texcoord >= 0 ? binding.transform.texcoord : binding.texcoord;
        const auto* uv_channel = channel_in(channels, VertexChannel::Semantic::Texcoord, static_cast<std::uint32_t>(std::max(0, uv_set)));
        if (!uv_channel) result.missing_texcoord_fallback = true;
        if (uv_channel && (uv_channel->components < 2 || uv_channel->values.size() != vertex_count * uv_channel->components)) { error = "material texture TEXCOORD set is malformed"; return false; }
        for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
            const auto bx = triangle % blocks_per_row, by = triangle / blocks_per_row, base = triangle * 3;
            std::array<std::array<float, 2>, 3> corners_uv{};
            if (uv_channel) for (int corner = 0; corner < 3; ++corner) {
                const auto v = static_cast<std::size_t>(source.indices[base + corner]);
                corners_uv[corner] = {uv_channel->values[v * uv_channel->components],
                                      uv_channel->values[v * uv_channel->components + 1]};
            }
            for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x) {
                const float tx = static_cast<float>(x) / 3.0f, ty = static_cast<float>(y) / 3.0f;
                std::array<float, 2> uv{}, ddx{}, ddy{}; bool uv_overflow = false;
                if (!checked_atlas_uv(corners_uv, tx, ty, uv, ddx, ddy, uv_overflow, error)) return false;
                result.uv_overflow_clamped = result.uv_overflow_clamped || uv_overflow;
                ImageSample sample; if (!sample_image(prepared, binding, uv, ddx, ddy, sample, error)) return false;
                result.uv_overflow_clamped = result.uv_overflow_clamped || sample.uv_overflow_clamped;
                result.sampler_filter_fallback = result.sampler_filter_fallback || sample.sampler_filter_fallback;
                const auto pixel = (static_cast<std::size_t>(by * 4 + y) * side + bx * 4 + x) * 4;
                for (int c = 0; c < 4; ++c) image.pixels[pixel + c] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(sample.rgba[c] * 255.0f), 0, 255));
            }
        }
        destination = std::move(image); return true;
    };
    if (!bake_domain(material.metallic_roughness_texture, ImageSemantic::Linear, result.metallic_roughness) ||
        !bake_domain(material.occlusion_texture, ImageSemantic::Linear, result.occlusion) ||
        !bake_domain(material.normal_texture, ImageSemantic::TangentNormal, result.normal) ||
        !bake_domain(material.emissive_texture, ImageSemantic::SRGB, result.emission)) return false;
    if (!rewrite(baked.source_channels, uvs) || !rewrite(baked.channels, uvs)) { error = "TEXCOORD_0 has fewer than two components"; return false; }
    baked.indices.resize(corners.size()); std::iota(baked.indices.begin(), baked.indices.end(), 0u); baked.mode = 4;
    if (regenerate_atlas_tangents(baked, error) == TangentRegenerationStatus::Invalid) return false;
    result.status = AtlasBakeStatus::Applied; result.primitive = std::move(baked); result.material = material;
    result.material.base_color = {1,1,1,1}; result.material.base_color_texture.texture = 0; result.material.base_color_texture.texcoord = 0; result.material.base_color_texture.transform = {};
    if (has_specgloss) {
        result.material.has_pbr_specular_glossiness = false;
        result.material.diffuse_texture = {};
        result.material.specular_glossiness_texture = {};
        result.metallic_roughness = std::move(specgloss_mr);
        result.material.metallic = 1.0f;
        result.material.roughness = 1.0f;
        result.material.metallic_roughness_texture.texture = 0;
        result.material.metallic_roughness_texture.texcoord = 0;
        result.material.metallic_roughness_texture.transform = {};
    }
    if (has_transmission_texture) {
        result.material.has_transmission = false;
        result.material.transmission_texture = {};
        result.material.alpha_mode = "BLEND";
    }
    result.image = std::move(atlas); return true;
}

bool canonicalize_vertex_colors(Scene& scene, bool automatic_materials,
                                VertexColorCanonicalizationReport& report, std::string& error) {
    report = {};
    const auto remove_sets = [](Primitive& primitive, bool all) {
        const auto remove = [all](std::vector<VertexChannel>& channels) {
            channels.erase(std::remove_if(channels.begin(), channels.end(), [all](const VertexChannel& channel) {
                return channel.semantic == VertexChannel::Semantic::Color && (all || channel.set > 0);
            }), channels.end());
        };
        remove(primitive.source_channels); remove(primitive.channels);
    };
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        auto& primitive = scene.primitives[i];
        bool has_color = false, has_color0 = false, has_higher = false;
        const auto scan = [&](const std::vector<VertexChannel>& channels) {
            for (const auto& channel : channels) if (channel.semantic == VertexChannel::Semantic::Color) {
                has_color = true; has_color0 = has_color0 || channel.set == 0; has_higher = has_higher || channel.set > 0;
            }
        };
        scan(primitive.source_channels); scan(primitive.channels);
        if (!has_color) continue;
        if (automatic_materials && primitive.material >= 0 &&
            static_cast<std::size_t>(primitive.material) < scene.materials.size() &&
            scene.materials[static_cast<std::size_t>(primitive.material)].intent == MaterialInfo::Intent::External) {
            error = "primitive " + std::to_string(i) +
                ": external material with COLOR attributes cannot be lowered: the current UNIT vertex profile has no color stream, and its tint cannot be baked into an external MATERIAL; use generated material intent or remove the colors";
            return false;
        }
        if (!automatic_materials) {
            remove_sets(primitive, true); report.changed = report.explicit_drop = true; report.diagnostics.push_back("primitive " + std::to_string(i) + ": dropped vertex colors for explicit material approximation"); continue;
        }
        if (has_higher) { remove_sets(primitive, false); report.changed = report.higher_set_drop = true; report.diagnostics.push_back("primitive " + std::to_string(i) + ": dropped unconsumed COLOR_1+ channels"); }
        if (!has_color0) continue;
    }
    if (!automatic_materials) { scene.notes.insert(scene.notes.end(), report.diagnostics.begin(), report.diagnostics.end()); return true; }
    bool constant = false;
    if (!canonicalize_constant_color0(scene, constant, error)) return false;
    report.constant_applied = constant;
    report.changed = report.changed || constant;
    std::vector<bool> converted_materials(scene.materials.size(), false);
    for (const auto& primitive : scene.primitives) {
        if (primitive.material < 0 || static_cast<std::size_t>(primitive.material) >= scene.materials.size()) continue;
        const auto index = static_cast<std::size_t>(primitive.material);
        if (scene.materials[index].intent != MaterialInfo::Intent::Generated) continue;
        if (converted_materials[index]) continue;
        converted_materials[index] = true;
        DirectSpecGlossStatus status{};
        if (!convert_specgloss_texture_space(scene, index, status, error)) return false;
        if (status == DirectSpecGlossStatus::Applied) {
            report.changed = true;
            report.direct_specgloss_applied = true;
            report.approximations.push_back("material:" + std::to_string(index) +
                                            ":specgloss_direct_texture_conversion");
            report.diagnostics.push_back("material " + std::to_string(index) +
                ": converted compatible specular-glossiness textures at full source resolution; preserved indexed geometry and UVs");
        }
    }
    for (std::size_t i = 0; i < scene.primitives.size(); ++i) {
        const auto& primitive = scene.primitives[i];
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size() &&
            scene.materials[static_cast<std::size_t>(primitive.material)].intent != MaterialInfo::Intent::Generated)
            continue;
        const auto& candidate_channels = primitive.source_channels.empty() ? primitive.channels : primitive.source_channels;
        const bool has_color0 = find_channel(candidate_channels, VertexChannel::Semantic::Color, 0) != nullptr;
        bool texture_domain_needs_atlas = false;
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size()) {
            const auto& material = scene.materials[static_cast<std::size_t>(primitive.material)];
            const bool emits = material.emissive_strength > 0.0f &&
                std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; });
            const std::array<const TextureBinding*, 7> bindings{{&material.base_color_texture,
                &material.metallic_roughness_texture, &material.normal_texture,
                &material.occlusion_texture, &material.emissive_texture,
                &material.specular_glossiness_texture, &material.transmission_texture}};
            for (std::size_t binding_index = 0; binding_index < bindings.size(); ++binding_index) {
                const auto& binding = *bindings[binding_index];
                if (binding.texture < 0 || (binding_index == 4 && !emits)) continue;
                const int texcoord = binding.transform.texcoord < 0 ? binding.texcoord : binding.transform.texcoord;
                texture_domain_needs_atlas = texture_domain_needs_atlas ||
                    !find_channel(candidate_channels, VertexChannel::Semantic::Texcoord,
                                  static_cast<std::uint32_t>(std::max(0, texcoord))) || texcoord != 0 ||
                    std::abs(binding.transform.offset[0]) > 1e-6f || std::abs(binding.transform.offset[1]) > 1e-6f ||
                    std::abs(binding.transform.scale[0] - 1.0f) > 1e-6f || std::abs(binding.transform.scale[1] - 1.0f) > 1e-6f ||
                    std::abs(binding.transform.rotation) > 1e-6f;
                if (static_cast<std::size_t>(binding.texture) >= scene.textures.size()) texture_domain_needs_atlas = true;
                else {
                    const int sampler_index = scene.textures[static_cast<std::size_t>(binding.texture)].sampler;
                    if (sampler_index >= 0) {
                        if (static_cast<std::size_t>(sampler_index) >= scene.samplers.size()) texture_domain_needs_atlas = true;
                        else {
                            const auto& sampler = scene.samplers[static_cast<std::size_t>(sampler_index)];
                            texture_domain_needs_atlas = texture_domain_needs_atlas || sampler.wrap_s != 10497 || sampler.wrap_t != 10497 ||
                                (sampler.mag_filter != 0 && sampler.mag_filter != 9729) ||
                                (sampler.min_filter != 0 && sampler.min_filter != 9987);
                        }
                    }
                }
            }
            texture_domain_needs_atlas = texture_domain_needs_atlas ||
                !direct_core_material_textures(scene, material) ||
                material.has_pbr_specular_glossiness ||
                (material.has_transmission && material.transmission_texture.texture >= 0);
        }
        if (!has_color0 && !texture_domain_needs_atlas) continue;
        CanonicalAtlasResult atlas;
        if (!canonicalize_varying_color0(scene, i, atlas, error)) {
            if (!atlas_budget_error(error)) return false;
            constexpr std::size_t atlas_triangle_budget = std::numeric_limits<std::uint16_t>::max() / 3u;
            std::vector<Primitive> parts;
            const auto original_error = error;
            // Partitioning fixes the deindexed vertex limit, not source-image
            // decoding or allocation failures. Do not retry those per page.
            if (error == "canonical material atlas vertex/index capacity overflow" &&
                !partition_triangle_primitive(scene.primitives[i], atlas_triangle_budget, parts, error)) return false;
            if (parts.size() > 1) {
                scene.primitives[i] = std::move(parts.front());
                scene.primitives.insert(scene.primitives.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                        std::make_move_iterator(parts.begin() + 1),
                                        std::make_move_iterator(parts.end()));
                error.clear();
                i = i == 0 ? std::numeric_limits<std::size_t>::max() : i - 1;
                continue;
            }
            error = original_error;
            // A valid primitive may need more atlas space than the native single-page
            // representation permits. Clone the material before dropping texture
            // bindings so shared materials remain unchanged and the fallback stays
            // representable by the native material profile.
            const bool material_loss = clone_material_without_textures(scene, i);
            if (!has_color0) {
                report.changed = report.atlas_capacity_fallback = true;
                report.approximations.push_back("material_texture_atlas_capacity_fallback");
                report.diagnostics.push_back("primitive " + std::to_string(i) + ": dropped material texture bindings because canonical material atlas exceeded bounded native capacity");
                error.clear();
                continue;
            }
            auto& fallback = scene.primitives[i];
            const auto drop_color0 = [](std::vector<VertexChannel>& channels) {
                channels.erase(std::remove_if(channels.begin(), channels.end(), [](const VertexChannel& channel) {
                    return channel.semantic == VertexChannel::Semantic::Color && channel.set == 0;
                }), channels.end());
            };
            drop_color0(fallback.source_channels);
            drop_color0(fallback.channels);
            report.changed = report.explicit_drop = report.atlas_capacity_fallback = true;
            report.approximations.push_back("vertex_color_atlas_capacity_drop");
            if (material_loss) report.approximations.push_back("material_texture_atlas_capacity_fallback");
            report.diagnostics.push_back("primitive " + std::to_string(i) + ": dropped COLOR_0 and material texture bindings because canonical material atlas exceeded bounded native capacity");
            error.clear();
            continue;
        }
        if (atlas.status != AtlasBakeStatus::Applied) continue;
        const auto append_image = [&](const char* suffix, const stingray::texture::ImageRGBA& source) {
            ImageInfo image; image.name = std::string("__canonical_material_") + suffix + "_image_" + std::to_string(i); image.mime_type = "image/raw-rgba8"; image.rgba8 = RGBA8Payload{source.width, source.height, source.pixels};
            const int image_index = static_cast<int>(scene.images.size()); scene.images.push_back(std::move(image));
            TextureInfo texture; texture.name = std::string("__canonical_material_") + suffix + "_texture_" + std::to_string(i); texture.source = image_index; texture.sampler = -1;
            const int texture_index = static_cast<int>(scene.textures.size()); scene.textures.push_back(std::move(texture)); return texture_index;
        };
        MaterialInfo material = atlas.material; material.name = "__canonical_vertex_color_material_" + std::to_string(i); material.base_color_texture.texture = append_image("base", atlas.image); material.base_color_texture.texcoord = 0; material.base_color_texture.transform = {};
        if (atlas.metallic_roughness) { material.metallic_roughness_texture.texture = append_image("metallic_roughness", *atlas.metallic_roughness); material.metallic_roughness_texture.texcoord = 0; material.metallic_roughness_texture.transform = {}; }
        if (atlas.occlusion) { material.occlusion_texture.texture = append_image("occlusion", *atlas.occlusion); material.occlusion_texture.texcoord = 0; material.occlusion_texture.transform = {}; }
        if (atlas.normal) { material.normal_texture.texture = append_image("normal", *atlas.normal); material.normal_texture.texcoord = 0; material.normal_texture.transform = {}; }
        if (atlas.emission) { material.emissive_texture.texture = append_image("emission", *atlas.emission); material.emissive_texture.texcoord = 0; material.emissive_texture.transform = {}; }
        const int material_index = static_cast<int>(scene.materials.size()); scene.materials.push_back(std::move(material));
        auto& target = scene.primitives[i]; target = std::move(atlas.primitive); target.material = material_index; target.material_name = scene.materials.back().name;
        report.atlas_applied = report.changed = true; report.sampler_filter_fallback = report.sampler_filter_fallback || atlas.sampler_filter_fallback;
        if (atlas.missing_texcoord_fallback) {
            report.approximations.push_back("missing_texcoord_constant_uv");
            report.diagnostics.push_back("primitive " + std::to_string(i) + ": missing referenced TEXCOORD attribute approximated with constant UV (0,0)");
        }
        if (atlas.uv_overflow_clamped) {
            report.approximations.push_back("uv_overflow_clamped");
            report.diagnostics.push_back("primitive " + std::to_string(i) + ": finite extreme UV/derivative values approximated with a constant texture sample");
        }
        if (atlas.color_range_clamped) {
            report.approximations.push_back("material_color_range_clamped");
            report.diagnostics.push_back("primitive " + std::to_string(i) + ": clamped finite baseColorFactor/COLOR_0 values to [0,1] during atlas lowering");
        }
        report.diagnostics.push_back("primitive " + std::to_string(i) + (has_color0
            ? ": canonicalized varying COLOR_0 into atlas"
            : ": canonicalized material texture domains into atlas"));
        if (atlas.sampler_filter_fallback) report.diagnostics.push_back("primitive " + std::to_string(i) + ": sampler_filter_fallback");
    }
    scene.notes.insert(scene.notes.end(), report.diagnostics.begin(), report.diagnostics.end());
    return true;
}
} // namespace dtglb::processing
