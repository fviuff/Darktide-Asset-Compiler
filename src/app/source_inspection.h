#pragma once

#include <filesystem>
#include <optional>

namespace dtglb::app {
int inspect_source(const std::filesystem::path& input, std::optional<int> scene_index);
}
