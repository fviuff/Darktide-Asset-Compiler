#include "compiler/resource_key.h"

#include <cctype>
#include <stdexcept>

namespace dtglb::compiler {

bool ResourceKey::valid() const noexcept {
    if (type.empty() || name.empty()) return false;
    for (const char c : type) {
        if (static_cast<unsigned char>(c) < 32 || c == '/' || c == '\\' || c == ':') return false;
    }
    if (name.front() == '/' || name.front() == '\\' ||
        (name.size() >= 3 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':' &&
         (name[2] == '/' || name[2] == '\\'))) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (static_cast<unsigned char>(c) < 32 || c == '\\' || c == ':') return false;
        if (c == '.' && i + 1 < name.size() && name[i + 1] == '.' &&
            (i == 0 || name[i - 1] == '/') && (i + 2 == name.size() || name[i + 2] == '/')) return false;
    }
    return true;
}

void ResourceKey::validate() const {
    if (!valid()) throw std::invalid_argument("resource key contains an invalid native name");
}

} // namespace dtglb::compiler
