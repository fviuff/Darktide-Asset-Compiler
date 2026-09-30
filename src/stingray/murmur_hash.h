#pragma once
#include <cstdint>
#include <string_view>

namespace dtglb::stingray {
std::uint32_t murmur32(const void* data, std::size_t len, std::uint32_t seed = 0);
std::uint64_t murmur64(const void* data, std::size_t len, std::uint64_t seed = 0);
inline std::uint32_t id32(std::string_view s) { return murmur32(s.data(), s.size(), 0); }
inline std::uint64_t id64(std::string_view s) { return murmur64(s.data(), s.size(), 0); }
// Stingray resource/material-domain IdString32 values are frequently stored as
// the upper 32 bits of the seed-zero IdString64 Murmur64A value. Keep this
// explicit instead of conflating it with legacy standalone MurmurHash2-32.
inline std::uint32_t id32_from_id64(std::string_view s) { return static_cast<std::uint32_t>(id64(s) >> 32); }
}
