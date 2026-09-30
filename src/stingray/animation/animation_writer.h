#pragma once
#include "stingray/animation/animation_evaluator.h"
#include "stingray/animation/animation_fitter.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dtglb { struct Scene; struct SkinInfo; struct AnimationInfo; }
namespace dtglb::stingray::animation {

// Compiler work budget per channel, not a recovered engine/resource-wide limit.
inline constexpr std::size_t max_step_controls_per_track = 10000;

struct BuiltAnimation {
    std::string resource_name;
    std::string clip_name;
    std::vector<std::uint8_t> cooked;
    float duration = 0.0f;
    std::uint32_t track_entries = 0;
    bool best_effort = false;
    std::optional<SampledAnimationError> fidelity;
    struct FitReport {
        int source_node = -1;
        AnimationPath path = AnimationPath::Translation;
        double tolerance = 0, measured_max_error = 0, measured_max_error_time = 0;
        bool measured_error_available = false;
        bool conformant = false, best_effort = false;
        std::size_t controls = 0, refinements = 0, evaluations = 0;
        std::size_t planned_probe_count = 0, evaluated_probe_count = 0;
    };
    std::optional<FitOptions> fit_options;
    std::vector<FitReport> fits;
};

bool build_skeletal_animation(
    const Scene& scene,
    const SkinInfo& skin,
    const AnimationInfo& source,
    std::string resource_name,
    BuiltAnimation& out,
    std::string& error,
    const FitOptions* fit_options = nullptr); // null keeps the raw native-profile constructor for diagnostics/tests

bool write_skeletal_animation(
    const BuiltAnimation& animation,
    const std::filesystem::path& path,
    std::string& error);

bool validate_skeletal_animation(
    const std::vector<std::uint8_t>& cooked,
    std::uint32_t expected_bones,
    std::string& error);

} // namespace dtglb::stingray::animation
