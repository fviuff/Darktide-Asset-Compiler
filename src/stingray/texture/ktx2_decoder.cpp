#include "stingray/texture/ktx2_decoder.h"

#include <transcoder/basisu_transcoder.h>

#include <limits>
#include <mutex>
#include <utility>

namespace dtglb::stingray::texture {
namespace {

void initialize_transcoder() {
    basist::basisu_transcoder_init();
}

} // namespace

bool decode_ktx2_rgba(const std::vector<std::uint8_t>& bytes, ImageRGBA& out, std::string& error) {
    if (bytes.empty()) {
        error = "KTX2 payload is empty";
        return false;
    }
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "KTX2 payload is too large";
        return false;
    }

    static std::once_flag init_once;
    std::call_once(init_once, initialize_transcoder);

    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
        error = "KTX2 header or level index is invalid";
        return false;
    }
    // Basis Universal 2.50 also exposes XUASTC LDR through the KTX2
    // transcoder.  It has the same RGBA32 destination contract, so accept all
    // LDR source families and reject HDR explicitly.
    if (!transcoder.is_ldr()) {
        error = "KTX2 texture format is unsupported (expected a Basis-compatible LDR source)";
        return false;
    }
    if (transcoder.get_faces() != 1 || transcoder.get_layers() > 1) {
        error = "KTX2 texture arrays and cubemaps are unsupported";
        return false;
    }

    const auto width = transcoder.get_width();
    const auto height = transcoder.get_height();
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width == 0 || height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 4 ||
        pixels > std::numeric_limits<std::uint32_t>::max()) {
        error = "KTX2 image dimensions are invalid or too large";
        return false;
    }

    if (!transcoder.start_transcoding()) {
        error = "KTX2 transcoder could not start (corrupt or unsupported payload)";
        return false;
    }

    ImageRGBA decoded;
    decoded.width = width;
    decoded.height = height;
    try {
        decoded.pixels.resize(pixels * 4);
    } catch (const std::bad_alloc&) {
        error = "KTX2 RGBA allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "KTX2 RGBA allocation is too large";
        return false;
    }
    if (!transcoder.transcode_image_level(
            0, 0, 0, decoded.pixels.data(), static_cast<std::uint32_t>(pixels),
            basist::transcoder_texture_format::cTFRGBA32, 0, width, height)) {
        error = "KTX2 RGBA8 transcode failed";
        return false;
    }

    out = std::move(decoded);
    return true;
}

} // namespace dtglb::stingray::texture
