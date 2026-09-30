#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dtglb::stingray {

// Current Darktide cooked-resource envelope used by UNIT-adjacent resources such
// as BONES, ANIMATION, TEXTURE and PACKAGE. MATERIAL headers use their own fixed
// 68-byte header and are intentionally not routed through this helper.
std::vector<std::uint8_t> wrap_cooked_resource(
    std::string_view engine_type,
    std::string_view resource_name,
    const std::vector<std::uint8_t>& body,
    std::string_view stream_name = {});

bool parse_cooked_resource_envelope(
    const std::vector<std::uint8_t>& blob,
    std::string_view expected_engine_type,
    std::vector<std::uint8_t>& body,
    std::string& stream_name,
    std::string& error);

} // namespace dtglb::stingray
