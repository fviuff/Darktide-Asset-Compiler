#pragma once

#include "scene/scene.h"

#include <array>
#include <string>
#include <vector>

namespace dtglb::processing {

enum class ImageSemantic { SRGB, Linear, TangentNormal };

struct ImageSample {
    std::array<float, 4> rgba{};
    float lod = 0.0f;
    bool sampler_filter_fallback = false;
    bool uv_overflow_clamped = false;
};

struct PreparedImageSampler {
    std::vector<RGBA8Payload> mip_levels;
    SamplerInfo sampler;
    ImageSemantic semantic = ImageSemantic::Linear;
};

bool prepare_image_sampler(const RGBA8Payload& image, const SamplerInfo& sampler,
                           ImageSemantic semantic, PreparedImageSampler& output,
                           std::string& error);

bool sample_image(const PreparedImageSampler& prepared, const TextureBinding& binding,
                  const std::array<float, 2>& uv, const std::array<float, 2>& ddx,
                  const std::array<float, 2>& ddy, ImageSample& output, std::string& error);

// Samples an in-memory RGBA8 image using glTF UV transforms and sampler rules.
// RGB is returned in the semantic's natural representation: sRGB values for
// SRGB, normalized values for Linear, and encoded tangent-normal values for
// TangentNormal. Alpha is always linearly filtered.
bool sample_image(const RGBA8Payload& image, const TextureBinding& binding,
                  const SamplerInfo& sampler, const std::array<float, 2>& uv,
                  const std::array<float, 2>& ddx, const std::array<float, 2>& ddy,
                  ImageSemantic semantic, ImageSample& output, std::string& error);

} // namespace dtglb::processing
