#include "app/source_inspection.h"

#include "gltf/glb_loader.h"
#include "scene/scene.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>

namespace dtglb::app {
namespace {
#ifdef _WIN32
std::filesystem::path windows_long_path(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto native = absolute.native();
    if (native.rfind(LR"(\\?\)", 0) == 0) return absolute;
    if (native.rfind(LR"(\\)", 0) == 0)
        return std::filesystem::path(LR"(\\?\UNC\)" + native.substr(2));
    return std::filesystem::path(LR"(\\?\)" + native);
}
#endif

const char* shown(const std::string& value) {
    return value.empty() ? "(unnamed)" : value.c_str();
}

void print_names(const char* label, const std::vector<std::string>& names) {
    std::cout << label << " (" << names.size() << "):";
    if (names.empty()) std::cout << " none";
    std::cout << '\n';
    for (const auto& name : names) std::cout << "  " << name << '\n';
}
}

int inspect_source(const std::filesystem::path& input, std::optional<int> scene_index) {
    Scene scene;
    std::string error;
    gltf::LoadOptions options;
    options.scene_index = scene_index;
#ifdef _WIN32
    const auto input_path = windows_long_path(input);
#else
    const auto& input_path = input;
#endif
    if (!gltf::load_scene(input_path, scene, error, options)) {
#ifdef _WIN32
        const auto extended_input = input_path.string();
        const auto ordinary_input = std::filesystem::absolute(input).lexically_normal().string();
        for (auto at = error.find(extended_input); at != std::string::npos;
             at = error.find(extended_input, at + ordinary_input.size()))
            error.replace(at, extended_input.size(), ordinary_input);
#endif
        std::cerr << "Source inspection failed: " << error << '\n';
        return 2;
    }

    std::cout << "Source: " << input.string() << '\n';
    std::cout << "Scenes (" << scene.scenes.size() << "):\n";
    for (std::size_t i = 0; i < scene.scenes.size(); ++i) {
        std::cout << "  [" << i << "] " << shown(scene.scenes[i].name);
        if (static_cast<int>(i) == scene.default_scene) std::cout << " (default)";
        if (static_cast<int>(i) == scene.selected_scene) std::cout << " (selected)";
        std::cout << '\n';
    }
    if (scene.selected_scene < 0) std::cout << "Selected scene: none (root fallback)\n";
    else std::cout << "Selected scene: [" << scene.selected_scene << "] "
                   << shown(scene.scenes[static_cast<std::size_t>(scene.selected_scene)].name) << '\n';

    std::set<int> active_meshes;
    for (const auto& primitive : scene.primitives)
        if (primitive.source_mesh >= 0) active_meshes.insert(primitive.source_mesh);
    std::cout << "Active meshes: " << active_meshes.size() << ", primitives: " << scene.primitives.size() << '\n';
    for (const auto& primitive : scene.primitives) {
        std::cout << "  mesh[" << primitive.source_mesh << "].primitive[" << primitive.source_primitive
                  << "] " << shown(primitive.name) << " material=" << shown(primitive.material_name) << '\n';
    }

    std::cout << "Materials (" << scene.materials.size() << "):\n";
    if (scene.materials.empty()) std::cout << "  none\n";
    for (std::size_t i = 0; i < scene.materials.size(); ++i)
        std::cout << "  [" << i << "] " << shown(scene.materials[i].name) << '\n';

    std::cout << "Skins (" << scene.skins.size() << "):\n";
    if (scene.skins.empty()) std::cout << "  none\n";
    for (std::size_t i = 0; i < scene.skins.size(); ++i)
        std::cout << "  [" << i << "] " << shown(scene.skins[i].name)
                  << " joints=" << scene.skins[i].joints.size() << '\n';

    std::cout << "Clips (" << scene.animations.size() << "):\n";
    if (scene.animations.empty()) std::cout << "  none\n";
    for (const auto& animation : scene.animations) {
        float min_time = 0.0f, max_time = 0.0f;
        bool has_time = false;
        for (const auto& track : animation.tracks) {
            for (const float time : track.times) {
                if (!has_time) {
                    min_time = max_time = time;
                    has_time = true;
                } else {
                    min_time = std::min(min_time, time);
                    max_time = std::max(max_time, time);
                }
            }
        }
        std::cout << "  [" << animation.source_index << "] " << shown(animation.name)
                  << " duration=" << std::fixed << std::setprecision(6)
                  << (has_time ? max_time - min_time : 0.0f)
                  << " tracks=" << animation.tracks.size() << '\n';
    }

    const auto& definition = scene.asset_definition;
    const std::size_t body_count = definition.node_bodies.size() + (definition.body ? 1u : 0u);
    std::cout << "Authored physics: bodies=" << body_count
              << " colliders=" << definition.colliders.size() << '\n';
    print_names("Extensions used", scene.extensions_used);
    print_names("Extensions required", scene.extensions_required);
    print_names("Active extensions used", scene.active_extensions_used);
    return 0;
}
}
