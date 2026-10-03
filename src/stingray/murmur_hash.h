#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace dtglb::stingray {
std::uint64_t murmur64(const void* data, std::size_t len, std::uint64_t seed = 0);
inline std::uint64_t id64(std::string_view s) { return murmur64(s.data(), s.size(), 0); }
// IdString32 (node, mesh, actor, material slot, event names...) is the upper 32 bits of the
// seed-zero IdString64 Murmur64A value; Unit.node(unit, "name") looks names up the same way.
inline std::uint32_t id32_from_id64(std::string_view s) { return static_cast<std::uint32_t>(id64(s) >> 32); }
// Node and mesh names: "#1234abcd" is a game name only known by its hash and is written as that hash.
inline std::uint32_t name_id32(std::string_view s) {
    if (s.size() == 9 && s[0] == '#' && s.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string_view::npos)
        return static_cast<std::uint32_t>(std::strtoul(std::string(s.substr(1)).c_str(), nullptr, 16));
    return id32_from_id64(s);
}
}
