#include "processing/image_sampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace dtglb::processing {
namespace {
bool finite2(const std::array<float, 2>& v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }
bool valid_wrap(int value) { return value == 10497 || value == 33071 || value == 33648; }
bool valid_mag(int value) { return value == 0 || value == 9728 || value == 9729; }
bool valid_min(int value) { return value == 0 || (value >= 9728 && value <= 9729) || (value >= 9984 && value <= 9987); }

float wrap(float value, int mode) {
    if (mode == 33071) return std::clamp(value, 0.0f, 1.0f);
    float period = value - std::floor(value);
    if (mode == 33648) {
        if (std::fmod(std::floor(value), 2.0f) != 0.0f) period = 1.0f - period;
    }
    return period;
}

int wrap_index(int value, int size, int mode) {
    if (mode == 33071) return std::clamp(value, 0, size - 1);
    if (mode == 33648) {
        const long long period = static_cast<long long>(size) * 2ll;
        long long index = static_cast<long long>(value) % period; if (index < 0) index += period;
        return index < size ? static_cast<int>(index) : static_cast<int>(period - 1 - index);
    }
    int index = value % size; if (index < 0) index += size; return index;
}

float srgb_to_linear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

std::array<float, 4> decode(const std::uint8_t* p, ImageSemantic semantic) {
    std::array<float, 4> value{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    if (semantic == ImageSemantic::SRGB) {
        for (int i = 0; i != 3; ++i) value[i] = srgb_to_linear(value[i]);
    } else if (semantic == ImageSemantic::TangentNormal) {
        for (int i = 0; i != 3; ++i) value[i] = value[i] * 2.0f - 1.0f;
        const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (length > 1e-20f) for (int i = 0; i != 3; ++i) value[i] /= length;
        else value = {0.0f, 0.0f, 1.0f, value[3]};
    }
    return value;
}

std::array<float, 4> encode(std::array<float, 4> value, ImageSemantic semantic) {
    if (semantic == ImageSemantic::SRGB) {
        for (int i = 0; i != 3; ++i) value[i] = linear_to_srgb(value[i]);
    } else if (semantic == ImageSemantic::TangentNormal) {
        const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (length > 1e-20f) for (int i = 0; i != 3; ++i) value[i] = value[i] / length * 0.5f + 0.5f;
        else value = {0.5f, 0.5f, 1.0f, value[3]};
    }
    for (float& channel : value) channel = std::clamp(channel, 0.0f, 1.0f);
    return value;
}

bool validate_image(const RGBA8Payload& image, std::string& error) {
    const auto count = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width == 0 || image.height == 0 || count > std::numeric_limits<std::size_t>::max() / 4u ||
        image.bytes.size() != static_cast<std::size_t>(count * 4u)) {
        error = "image sampler received invalid RGBA8 dimensions or payload"; return false;
    }
    return true;
}

std::vector<RGBA8Payload> make_mips(const RGBA8Payload& image, ImageSemantic semantic) {
    std::vector<RGBA8Payload> levels{{image.width, image.height, image.bytes}};
    while (levels.back().width > 1 || levels.back().height > 1) {
        const auto& source = levels.back();
        RGBA8Payload next{std::max(1u, source.width / 2u), std::max(1u, source.height / 2u), {}};
        next.bytes.resize(static_cast<std::size_t>(next.width) * next.height * 4u);
        for (std::uint32_t y = 0; y < next.height; ++y) for (std::uint32_t x = 0; x < next.width; ++x) {
            std::array<float, 4> average{};
            for (std::uint32_t dy = 0; dy != 2; ++dy) for (std::uint32_t dx = 0; dx != 2; ++dx) {
                const auto sx = std::min(source.width - 1, x * 2u + dx), sy = std::min(source.height - 1, y * 2u + dy);
                const auto value = decode(&source.bytes[(static_cast<std::size_t>(sy) * source.width + sx) * 4u], semantic);
                for (int c = 0; c != 4; ++c) average[c] += value[c] * 0.25f;
            }
            const auto encoded = encode(average, semantic);
            auto* target = &next.bytes[(static_cast<std::size_t>(y) * next.width + x) * 4u];
            for (int c = 0; c != 4; ++c) target[c] = static_cast<std::uint8_t>(std::lround(encoded[c] * 255.0f));
        }
        levels.push_back(std::move(next));
    }
    return levels;
}

std::array<float, 4> texel(const RGBA8Payload& mip, float u, float v, const SamplerInfo& sampler,
                           bool linear, ImageSemantic semantic) {
    const float x = u * mip.width - 0.5f, y = v * mip.height - 0.5f;
    if (!linear) {
        const int ix = wrap_index(static_cast<int>(std::floor(x + 0.5f)), static_cast<int>(mip.width), sampler.wrap_s);
        const int iy = wrap_index(static_cast<int>(std::floor(y + 0.5f)), static_cast<int>(mip.height), sampler.wrap_t);
        return decode(&mip.bytes[(static_cast<std::size_t>(iy) * mip.width + ix) * 4u], semantic);
    }
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    std::array<float, 4> result{};
    for (int dy = 0; dy != 2; ++dy) for (int dx = 0; dx != 2; ++dx) {
        const int ix = wrap_index(x0 + dx, static_cast<int>(mip.width), sampler.wrap_s);
        const int iy = wrap_index(y0 + dy, static_cast<int>(mip.height), sampler.wrap_t);
        const auto value = decode(&mip.bytes[(static_cast<std::size_t>(iy) * mip.width + ix) * 4u], semantic);
        const float weight = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
        for (int c = 0; c != 4; ++c) result[c] += value[c] * weight;
    }
    return result;
}
}

bool prepare_image_sampler(const RGBA8Payload& image, const SamplerInfo& sampler,
                           ImageSemantic semantic, PreparedImageSampler& output, std::string& error) {
    if (!validate_image(image, error)) return false;
    if (!valid_wrap(sampler.wrap_s) || !valid_wrap(sampler.wrap_t) ||
        !valid_mag(sampler.mag_filter) || !valid_min(sampler.min_filter)) {
        error = "image sampler received an unsupported glTF sampler filter or wrap mode";
        return false;
    }
    output = {};
    output.sampler = sampler;
    output.semantic = semantic;
    try {
        output.mip_levels = make_mips(image, semantic);
    } catch (const std::bad_alloc&) {
        error = "image sampler mip allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "image sampler mip allocation is too large";
        return false;
    }
    return true;
}

bool sample_image(const PreparedImageSampler& prepared, const TextureBinding& binding,
                  const std::array<float, 2>& uv, const std::array<float, 2>& ddx,
                  const std::array<float, 2>& ddy, ImageSample& output, std::string& error) {
    const auto& sampler = prepared.sampler;
    const auto semantic = prepared.semantic;
    output.uv_overflow_clamped = false;
    if (prepared.mip_levels.empty() || !finite2(uv) || !finite2(ddx) || !finite2(ddy) ||
        !finite2(binding.transform.offset) || !finite2(binding.transform.scale) ||
        !std::isfinite(binding.transform.rotation)) {
        if (error.empty()) error = "image sampler received non-finite coordinates or transform";
        return false;
    }
    const float c = std::cos(binding.transform.rotation), s = std::sin(binding.transform.rotation);
    const auto transform = [&](const std::array<float, 2>& point) {
        const float x = point[0] * binding.transform.scale[0], y = point[1] * binding.transform.scale[1];
        return std::array<float, 2>{c * x - s * y + binding.transform.offset[0], s * x + c * y + binding.transform.offset[1]};
    };
    auto transformed_uv = transform(uv);
    const auto origin = transform({0.0f, 0.0f});
    const auto transformed_ddx_value = transform(ddx);
    const auto transformed_ddy_value = transform(ddy);
    auto transformed_ddx = std::array<float, 2>{transformed_ddx_value[0] - origin[0], transformed_ddx_value[1] - origin[1]};
    auto transformed_ddy = std::array<float, 2>{transformed_ddy_value[0] - origin[0], transformed_ddy_value[1] - origin[1]};
    const auto& image = prepared.mip_levels.front();
    float rho = std::max(std::hypot(transformed_ddx[0] * image.width, transformed_ddx[1] * image.height),
                               std::hypot(transformed_ddy[0] * image.width, transformed_ddy[1] * image.height));
    if (!finite2(transformed_uv) || !finite2(transformed_ddx) || !finite2(transformed_ddy) || !std::isfinite(rho)) {
        // Finite but extreme UV inputs can overflow the affine transform or
        // derivative footprint.  Keep malformed/non-finite source data a hard
        // failure, but lower this representable case to a deterministic
        // constant sample rather than allowing an int conversion of an
        // unbounded texel coordinate.
        transformed_uv = {0.0f, 0.0f};
        transformed_ddx = {0.0f, 0.0f};
        transformed_ddy = {0.0f, 0.0f};
        rho = 0.0f;
        output.uv_overflow_clamped = true;
    }
    const auto& levels = prepared.mip_levels;
    const float max_lod = static_cast<float>(levels.size() - 1);
    const float lod = rho > 0.0f ? std::clamp(std::log2(rho), 0.0f, max_lod) : 0.0f;
    output.lod = lod;
    const int mag = sampler.mag_filter == 9728 ? 9728 : 9729;
    const int min = sampler.min_filter == 0 ? 9729 : sampler.min_filter;
    const bool magnified = rho <= 1.0f;
    const bool linear_base = magnified ? mag == 9729 : (min == 9729 || min == 9985 || min == 9987);
    output.sampler_filter_fallback = magnified ? mag == 9728 : min == 9728 || min == 9984 || min == 9986;
    if (magnified || min == 9728 || min == 9729) output.rgba = texel(levels[0], wrap(transformed_uv[0], sampler.wrap_s), wrap(transformed_uv[1], sampler.wrap_t), sampler, linear_base, semantic);
    else if (min == 9984 || min == 9985 || min == 9986 || min == 9987) {
        const bool trilinear = min == 9986 || min == 9987;
        if (!trilinear) {
            const auto level = static_cast<std::size_t>(std::clamp(std::lround(lod), 0l, static_cast<long>(levels.size() - 1)));
            output.rgba = texel(levels[level], wrap(transformed_uv[0], sampler.wrap_s), wrap(transformed_uv[1], sampler.wrap_t), sampler, linear_base, semantic);
        } else {
            const auto lo = static_cast<std::size_t>(std::floor(lod));
            const auto hi = std::min(levels.size() - 1, lo + 1);
            const auto a = texel(levels[lo], wrap(transformed_uv[0], sampler.wrap_s), wrap(transformed_uv[1], sampler.wrap_t), sampler, linear_base, semantic);
            const auto b = texel(levels[hi], wrap(transformed_uv[0], sampler.wrap_s), wrap(transformed_uv[1], sampler.wrap_t), sampler, linear_base, semantic);
            const float t = lod - static_cast<float>(lo);
            for (int cidx = 0; cidx != 4; ++cidx) output.rgba[cidx] = a[cidx] * (1.0f - t) + b[cidx] * t;
        }
    }
    output.rgba = encode(output.rgba, semantic);
    return true;
}

bool sample_image(const RGBA8Payload& image, const TextureBinding& binding, const SamplerInfo& sampler,
                  const std::array<float, 2>& uv, const std::array<float, 2>& ddx,
                  const std::array<float, 2>& ddy, ImageSemantic semantic,
                  ImageSample& output, std::string& error) {
    PreparedImageSampler prepared;
    return prepare_image_sampler(image, sampler, semantic, prepared, error) &&
        sample_image(prepared, binding, uv, ddx, ddy, output, error);
}
}
