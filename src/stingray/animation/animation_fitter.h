#pragma once
#include "scene/scene.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace dtglb::stingray::animation {
struct FitOptions {
    double translation_tolerance = 1e-4;
    double scale_tolerance = 1e-4;
    double rotation_tolerance_radians = 1e-3;
    std::size_t max_controls = 10000;
    std::size_t max_refinements = 4096;
    std::size_t max_evaluations = 2000000;
};
struct TrackFit {
    std::array<float,4> initial{};
    std::vector<float> times;
    std::vector<std::array<float,4>> values;
    double measured_max_error = 0;
    double measured_max_error_time = 0;
    bool measured_error_available = false;
    // A bounded fit may still produce a valid native clip when its requested
    // tolerance cannot be met.  Keep that fact explicit for reporting.
    bool conformant = false;
    bool best_effort = false;
    std::size_t evaluations = 0;
    std::size_t refinements = 0;
    std::size_t planned_probe_count = 0;
    std::size_t evaluated_probe_count = 0;
    std::vector<float> final_probe_times;
};
bool fit_animation_track(const AnimationTrack& source, const FitOptions& options,
                         TrackFit& out, std::string& error);
}
