#include "stingray/murmur_hash.h"
#include <cstring>

namespace dtglb::stingray {

std::uint64_t murmur64(const void* key, std::size_t len, std::uint64_t seed) {
    constexpr std::uint64_t m = 0xc6a4a7935bd1e995ULL;
    constexpr int r = 47;
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(len) * m);
    const auto* data = static_cast<const std::uint8_t*>(key);
    while (len >= 8) {
        std::uint64_t k;
        std::memcpy(&k, data, 8);
        k *= m; k ^= k >> r; k *= m;
        h ^= k; h *= m;
        data += 8; len -= 8;
    }
    switch (len) {
        case 7: h ^= static_cast<std::uint64_t>(data[6]) << 48; [[fallthrough]];
        case 6: h ^= static_cast<std::uint64_t>(data[5]) << 40; [[fallthrough]];
        case 5: h ^= static_cast<std::uint64_t>(data[4]) << 32; [[fallthrough]];
        case 4: h ^= static_cast<std::uint64_t>(data[3]) << 24; [[fallthrough]];
        case 3: h ^= static_cast<std::uint64_t>(data[2]) << 16; [[fallthrough]];
        case 2: h ^= static_cast<std::uint64_t>(data[1]) << 8; [[fallthrough]];
        case 1: h ^= static_cast<std::uint64_t>(data[0]); h *= m; break;
        default: break;
    }
    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

} // namespace dtglb::stingray
