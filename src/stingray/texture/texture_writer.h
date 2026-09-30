#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::texture {

enum class TextureFiltering { ColorLinear, ColorSrgb, Normal };

struct TextureProfile {
    std::string id;
    std::uint32_t dxgi_format = 0;
    std::uint32_t block_bytes = 0;
    bool srgb = false;
    std::uint32_t body_flags = 0;
    std::uint32_t footer_word = 0;
    TextureFiltering filtering = TextureFiltering::ColorLinear;
};

const TextureProfile& substance_basic_bc_profile();
// Base color plus alpha used by the evidenced masked/transparent families.
const TextureProfile& substance_basic_bca_profile();
const TextureProfile& substance_basic_nm_profile();
const TextureProfile& substance_basic_orm_profile();
// Linear BC1 emission used by the evidenced current PBR-emissive family.
const TextureProfile& pbr_emissive_em_profile();

struct ImageRGBA {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct CookedTexture {
    std::string resource_name;
    std::string stream_name;
    std::filesystem::path texture_path;
    std::filesystem::path stream_path;
    std::string profile;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 0;
    std::uint32_t streamed_mip_count = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t resident_dds_bytes = 0;
    bool dimensions_normalized = false;
};

bool decode_image_rgba(const std::vector<std::uint8_t>& bytes, const std::string& mime_type, ImageRGBA& out, std::string& error);
bool normalize_image_for_verified_family(const ImageRGBA& source, const TextureProfile& profile, ImageRGBA& out, bool& changed, std::string& error);
bool normalize_image_for_verified_family(const ImageRGBA& source, ImageRGBA& out, bool& changed, std::string& error);

std::string make_texture_stream_name(const std::string& resource_name);

bool configure_oodle(const std::filesystem::path& dll_or_game_directory, std::string& error);

bool write_native_texture_rgba(
    const ImageRGBA& source,
    const TextureProfile& profile,
    const std::string& resource_name,
    const std::filesystem::path& texture_path,
    const std::filesystem::path& stream_path,
    CookedTexture& out,
    std::string& error);

bool inspect_texture_blob(const std::vector<std::uint8_t>& blob, std::string& report, std::string& error);

} // namespace dtglb::stingray::texture
