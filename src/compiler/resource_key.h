#pragma once

#include <compare>
#include <string>

namespace dtglb::compiler {

struct ResourceKey {
    std::string type;
    std::string name;

    bool operator==(const ResourceKey&) const = default;
    auto operator<=>(const ResourceKey&) const = default;

    bool valid() const noexcept;
    void validate() const;
};

} // namespace dtglb::compiler
