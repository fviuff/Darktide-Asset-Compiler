#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dtglb::gltf {
std::vector<std::uint8_t> read_image_file(const std::filesystem::path& path);
std::string percent_decode_image_uri(std::string_view text);
std::filesystem::path resolve_image_path(const std::filesystem::path& gltf_path, std::string_view uri);
std::string infer_image_mime(const std::vector<std::uint8_t>& bytes, std::string_view uri);
std::vector<std::uint8_t> decode_data_uri(std::string_view uri, std::string* mime = nullptr);
}
