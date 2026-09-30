#pragma once

#include "stingray/texture/texture_writer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::texture {

// Decode a Basis-compatible KTX2 LDR image (ETC1S, UASTC, or XUASTC,
// including Zstd-supercompressed payloads) to a tightly packed RGBA8 base level.
bool decode_ktx2_rgba(const std::vector<std::uint8_t>& bytes, ImageRGBA& out, std::string& error);

} // namespace dtglb::stingray::texture
