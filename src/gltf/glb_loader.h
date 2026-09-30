#pragma once
#include "scene/scene.h"
#include <filesystem>
#include <optional>
#include <string>

namespace dtglb::gltf {
struct LoadOptions {
    bool include_unused_meshes = false;
    std::optional<int> scene_index;
    std::optional<int> animation_index;
    std::optional<std::string> material_variant;
};

bool load_scene(const std::filesystem::path& path, Scene& out, std::string& error, const LoadOptions& options = {});
}
