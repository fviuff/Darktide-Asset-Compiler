#pragma once
#include "stingray/murmur_hash.h"
#include <string_view>

namespace dtglb::stingray {
// Existing CustomAssets identity notation for retail names whose plaintext
// has not been recovered. This denotes the identity itself, not a hashed path.
inline std::uint64_t resource_name_hash(std::string_view name) {
    if (name.size() == 21 && name.starts_with("#ID[") && name.back() == ']') {
        std::uint64_t value = 0;
        for (std::size_t i = 4; i < 20; ++i) {
            const char c = name[i];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return id64(name);
            value = (value << 4) | static_cast<std::uint64_t>(digit);
        }
        return value;
    }
    return id64(name);
}
}
