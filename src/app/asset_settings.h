#pragma once

#include "scene/scene.h"
#include "stingray/animation/animation_fitter.h"
#include "stingray/physics/physx_cooking.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dtglb::app {

struct AssetSettings {
    bool has_physics = false;
    bool physics_enabled = true;
    bool allow_fixed_morph_bake = false;
    bool allow_animation_resampling = false;
    bool allow_native_animation_interpolation = false;
    std::optional<stingray::animation::FitOptions> animation_fit;
    std::optional<float> asset_scale;
    std::optional<std::string> asset_path;
    std::optional<std::string> material_variant;
    std::optional<stingray::physics::FittedActorOptions> fitted_physics;
    std::optional<int> scene_index;
    std::optional<int> animation_index;
    std::vector<std::string> unknown_keys;
};

struct ResolvedPhysicsInventory {
    std::uint64_t requested = 0;
    std::uint64_t effective = 0;
    std::uint64_t suppressed = 0;
    std::vector<std::string> locations;
};

bool load_asset_settings(const std::filesystem::path& path, AssetSettings& out, std::string& error);
ResolvedPhysicsInventory resolve_physics_inventory(const SourceFeatureInventory& source,
                                                   const AssetSettings& settings,
                                                   bool physics_enabled);

} // namespace dtglb::app
