#include "stingray/animation/animation_fitter.h"
#include "stingray/animation/animation_evaluator.h"
#include "gltf/animation_sampler.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace dtglb::stingray::animation {
namespace {
constexpr double kAcceptanceFraction = 0.9;

std::size_t component_count(AnimationPath path) {
    return path == AnimationPath::Rotation ? 4u : 3u;
}

bool finite_options(const FitOptions& options) {
    return std::isfinite(options.translation_tolerance) &&
           std::isfinite(options.scale_tolerance) &&
           std::isfinite(options.rotation_tolerance_radians) &&
           options.translation_tolerance > 0 && options.scale_tolerance > 0 &&
           options.rotation_tolerance_radians > 0 && options.max_controls > 0 &&
           options.max_refinements > 0 && options.max_evaluations >= 2;
}

bool normalize_writer_quaternion(std::array<float, 4>& value) {
    // Keep this deliberately in float, matching animation_writer.cpp.
    float length_squared = 0.0f;
    for (const float component : value) {
        if (!std::isfinite(component)) return false;
        length_squared += component * component;
    }
    if (!std::isfinite(length_squared) || length_squared < 1e-20f) return false;
    const float inverse = 1.0f / std::sqrt(length_squared);
    for (float& component : value) component *= inverse;
    return true;
}

void make_hemispherical(std::array<float, 4>& value,
                        const std::array<float, 4>& previous) {
    double dot = 0;
    for (std::size_t component = 0; component != 4; ++component)
        dot += double(previous[component]) * value[component];
    if (dot < 0)
        for (float& component : value) component = -component;
}

bool rebuild_canonical_rotations(
    TrackFit& fit, const std::map<float, std::array<float, 4>>& source_cache,
    std::string& error) {
    const auto initial = source_cache.find(0.0f);
    if (initial == source_cache.end()) {
        error = "animation fitter internal source cache is incomplete";
        return false;
    }
    fit.initial = initial->second;
    if (!normalize_writer_quaternion(fit.initial)) {
        error = "animation fitter source quaternion is non-finite or zero-length";
        return false;
    }
    std::array<float, 4> previous = fit.initial;
    for (std::size_t index = 0; index != fit.values.size(); ++index) {
        const auto source_value = source_cache.find(fit.times[index]);
        if (source_value == source_cache.end()) {
            error = "animation fitter internal source cache is incomplete";
            return false;
        }
        auto& value = fit.values[index];
        value = source_value->second;
        if (!normalize_writer_quaternion(value)) {
            error = "animation fitter source quaternion is non-finite or zero-length";
            return false;
        }
        make_hemispherical(value, previous);
        previous = value;
    }
    return true;
}

double track_error(AnimationPath path, const std::array<float, 4>& expected,
                   const std::array<float, 4>& actual) {
    if (path == AnimationPath::Rotation) {
        double dot = 0, expected_norm = 0, actual_norm = 0;
        for (std::size_t component = 0; component != 4; ++component) {
            dot += double(expected[component]) * actual[component];
            expected_norm += double(expected[component]) * expected[component];
            actual_norm += double(actual[component]) * actual[component];
        }
        if (!std::isfinite(expected_norm) || !std::isfinite(actual_norm) ||
            expected_norm == 0 || actual_norm == 0)
            return std::numeric_limits<double>::infinity();
        const double cosine = std::clamp(std::abs(dot) / std::sqrt(expected_norm * actual_norm), 0.0, 1.0);
        return 2.0 * std::acos(cosine);
    }
    double squared = 0;
    for (std::size_t component = 0; component != 3; ++component) {
        const double difference = double(expected[component]) - actual[component];
        squared += difference * difference;
    }
    return std::sqrt(squared);
}

std::string measured_failure(const char* reason, double error, float time) {
    return std::string("animation fitter ") + reason + "; measured error=" +
        std::to_string(error) + " at time=" + std::to_string(time);
}

void add_probe(std::vector<float>& probes, float time) {
    if (std::isfinite(time) && time >= 0) probes.push_back(time);
}

void build_probes(const AnimationTrack& source, const TrackFit& fit,
                  std::vector<float>& probes) {
    std::vector<float> boundaries;
    boundaries.reserve(1 + source.times.size() + fit.times.size());
    boundaries.push_back(0.0f);
    boundaries.insert(boundaries.end(), source.times.begin(), source.times.end());
    boundaries.insert(boundaries.end(), fit.times.begin(), fit.times.end());
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    probes.clear();
    probes.reserve(boundaries.size() * 18 + 2);
    for (const float boundary : boundaries) {
        add_probe(probes, boundary);
        if (boundary > 0.0f)
            add_probe(probes, std::nextafter(boundary, -std::numeric_limits<float>::infinity()));
        add_probe(probes, std::nextafter(boundary, std::numeric_limits<float>::infinity()));
    }
    for (std::size_t interval = 0; interval + 1 < boundaries.size(); ++interval) {
        const float left = boundaries[interval];
        const float right = boundaries[interval + 1];
        if (!(left < right)) continue;
        for (int fraction = 1; fraction <= 15; ++fraction) {
            const float probe = static_cast<float>(double(left) +
                (double(right) - left) * fraction / 16.0);
            if (probe > left && probe < right) add_probe(probes, probe);
        }
    }
    // Source and native controls both hold after the final source key.
    add_probe(probes, std::nextafter(source.times.back(), std::numeric_limits<float>::infinity()));
    std::sort(probes.begin(), probes.end());
    probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
}

bool is_explicit_constant_linear(const AnimationTrack& source,
                                 const std::array<float, 4>& initial,
                                 const std::array<float, 4>& final) {
    if (source.interpolation != AnimationInterpolation::Linear || source.times.size() > 2) return false;
    if (source.path == AnimationPath::Rotation) {
        bool same = true, negated = true;
        for (std::size_t component = 0; component != 4; ++component) {
            same = same && final[component] == initial[component];
            negated = negated && final[component] == -initial[component];
        }
        return same || negated;
    }
    const auto count = component_count(source.path);
    for (std::size_t component = 0; component != count; ++component)
        if (initial[component] != final[component]) return false;
    return true;
}
}

bool fit_animation_track(const AnimationTrack& source, const FitOptions& options,
                         TrackFit& out, std::string& error) {
    if (!finite_options(options)) {
        error = "animation fitter options are invalid";
        return false;
    }
    if (source.path == AnimationPath::Weights ||
        (source.interpolation != AnimationInterpolation::Linear &&
         source.interpolation != AnimationInterpolation::CubicSpline)) {
        error = "animation fitter supports only continuous linear or cubic TRS tracks";
        return false;
    }

    gltf::PreparedAnimationSampler sampler;
    if (!sampler.initialize(source, error)) return false;

    TrackFit result;
    std::map<float, std::array<float, 4>> source_cache;
    std::size_t evaluations = 0;
    bool evaluation_limit_hit = false;
    auto source_at = [&](float time, std::array<float, 4>& value) -> bool {
        const auto cached = source_cache.find(time);
        if (cached != source_cache.end()) {
            value = cached->second;
            return true;
        }
        if (evaluations >= options.max_evaluations) {
            evaluation_limit_hit = true;
            return false;
        }
        if (!sampler.sample(time, value, error)) {
            if (source.path == AnimationPath::Rotation)
                error = "animation fitter source quaternion is non-finite or zero-length";
            return false;
        }
        ++evaluations;
        source_cache.emplace(time, value);
        return true;
    };

    std::array<float, 4> initial_value{};
    if (!source_at(0.0f, initial_value)) return false;
    const float end = source.times.back();
    std::array<float, 4> final_value{};
    if (!source_at(end, final_value)) return false;

    std::vector<float> controls;
    controls.reserve(source.times.size() + 1);
    for (const float time : source.times)
        if (time > 0.0f) controls.push_back(time);

    if (is_explicit_constant_linear(source, initial_value, final_value)) {
        controls.assign(1, end);
    } else if (source.times.size() == 2 && source.times.front() == 0.0f) {
        const float middle = static_cast<float>(double(end) / 2.0);
        if (!(middle > 0.0f && middle < end)) {
            controls = {end};
            result.best_effort = true;
        } else {
            controls = {middle, end};
        }
    }
    std::sort(controls.begin(), controls.end());
    controls.erase(std::unique(controls.begin(), controls.end()), controls.end());
    // Controls are bounded output, not a reason to reject an otherwise valid
    // continuous track.  Keep the authored end pose when reducing the set.
    if (controls.size() > options.max_controls) {
        result.best_effort = true;
        controls.resize(options.max_controls);
        if (!controls.empty()) controls.back() = end;
        std::sort(controls.begin(), controls.end());
        controls.erase(std::unique(controls.begin(), controls.end()), controls.end());
    }
    // Sampling a new authored control consumes the same bounded work budget as
    // a probe. The terminal control is already cached above and is always kept.
    const std::size_t uncached_budget = options.max_evaluations - evaluations;
    const auto uncached_controls = static_cast<std::size_t>(std::count_if(
        controls.begin(), controls.end(), [&](float time) { return !source_cache.count(time); }));
    if (uncached_controls > uncached_budget) {
        std::vector<float> bounded;
        bounded.reserve(uncached_budget + 1);
        std::size_t remaining = uncached_budget;
        for (const float time : controls) {
            if (source_cache.count(time)) bounded.push_back(time);
            else if (remaining) { bounded.push_back(time); --remaining; }
        }
        if (std::find(bounded.begin(), bounded.end(), end) == bounded.end()) bounded.push_back(end);
        std::sort(bounded.begin(), bounded.end());
        bounded.erase(std::unique(bounded.begin(), bounded.end()), bounded.end());
        controls = std::move(bounded);
        result.best_effort = true;
    }

    result.times.reserve(controls.size());
    result.values.reserve(controls.size());
    for (const float time : controls) {
        std::array<float, 4> value{};
        if (!source_at(time, value)) return false;
        result.times.push_back(time);
        result.values.push_back(value);
    }
    if (source.path == AnimationPath::Rotation &&
        !rebuild_canonical_rotations(result, source_cache, error)) return false;
    if (source.path != AnimationPath::Rotation) result.initial = initial_value;

    const double requested_tolerance = source.path == AnimationPath::Rotation
        ? options.rotation_tolerance_radians
        : source.path == AnimationPath::Scale ? options.scale_tolerance : options.translation_tolerance;
    const double acceptance_tolerance = requested_tolerance * kAcceptanceFraction;
    std::map<float, std::pair<double, std::array<float, 4>>> measured_probes;

    while (true) {
        std::vector<float> probes;
        build_probes(source, result, probes);
        result.planned_probe_count = probes.size();
        bool stopped_for_budget = false;
        for (const float time : probes) {
            if (measured_probes.count(time)) continue;
            std::array<float, 4> expected{}, actual{};
            if (!source_at(time, expected)) {
                if (evaluation_limit_hit) { stopped_for_budget = true; break; }
                return false;
            }
            if (evaluations >= options.max_evaluations) {
                evaluation_limit_hit = true;
                stopped_for_budget = true;
                break;
            }
            if (!sample_native_track(result.initial, source.path, result.times, result.values,
                                     time, actual, error)) return false;
            ++evaluations;
            const double current_error = track_error(source.path, expected, actual);
            if (!std::isfinite(current_error)) {
                error = measured_failure("encountered a non-finite measured error", current_error, time);
                return false;
            }
            measured_probes[time] = {current_error, expected};
        }
        double worst_error = -1.0;
        float worst_time = 0.0f;
        std::vector<float> evaluated_probes;
        evaluated_probes.reserve(probes.size());
        for (const float time : probes) {
            const auto measured = measured_probes.find(time);
            if (measured == measured_probes.end()) continue;
            evaluated_probes.push_back(time);
            if (measured->second.first > worst_error) {
                worst_error = measured->second.first;
                worst_time = time;
            }
        }
        result.measured_error_available = worst_error >= 0.0;
        result.measured_max_error = result.measured_error_available ? worst_error : 0.0;
        result.measured_max_error_time = worst_time;
        result.evaluations = evaluations;
        if (stopped_for_budget) {
            result.best_effort = true;
            result.evaluated_probe_count = evaluated_probes.size();
            result.final_probe_times = std::move(evaluated_probes);
            if (sampler.approximation_used()) { result.best_effort = true; result.conformant = false; }
            out = std::move(result);
            return true;
        }
        if (result.measured_error_available && worst_error <= acceptance_tolerance) {
            result.conformant = true;
            result.evaluated_probe_count = evaluated_probes.size();
            result.final_probe_times = std::move(evaluated_probes);
            if (sampler.approximation_used()) { result.best_effort = true; result.conformant = false; }
            out = std::move(result);
            return true;
        }
        if (result.refinements >= options.max_refinements) {
            result.best_effort = true;
            result.evaluated_probe_count = evaluated_probes.size();
            result.final_probe_times = std::move(evaluated_probes);
            if (sampler.approximation_used()) { result.best_effort = true; result.conformant = false; }
            out = std::move(result);
            return true;
        }
        if (result.times.size() >= options.max_controls) {
            result.best_effort = true;
            result.evaluated_probe_count = evaluated_probes.size();
            result.final_probe_times = std::move(evaluated_probes);
            if (sampler.approximation_used()) { result.best_effort = true; result.conformant = false; }
            out = std::move(result);
            return true;
        }
        struct Candidate {
            float time;
            double error;
            std::array<float, 4> value;
            std::size_t index;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(probes.size());
        for (const float time : probes) {
            const auto measured = measured_probes.find(time);
            if (measured == measured_probes.end() || measured->second.first <= acceptance_tolerance) continue;
            const auto insertion = std::lower_bound(result.times.begin(), result.times.end(), time);
            if (insertion == result.times.end() || *insertion != time)
                candidates.push_back({time, measured->second.first, measured->second.second,
                    static_cast<std::size_t>(insertion - result.times.begin())});
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.error > b.error;
        });
        const auto remaining_refinements = options.max_refinements - result.refinements;
        const auto remaining_controls = options.max_controls - result.times.size();
        const std::size_t batch_limit = std::min<std::size_t>(
            8, std::min(remaining_refinements, remaining_controls));
        std::vector<Candidate> selected;
        selected.reserve(batch_limit);
        for (const auto& candidate : candidates) {
            const bool separated = std::all_of(selected.begin(), selected.end(), [&](const Candidate& accepted) {
                const auto distance = candidate.index > accepted.index
                    ? candidate.index - accepted.index : accepted.index - candidate.index;
                return distance >= 5;
            });
            if (separated) selected.push_back(candidate);
            if (selected.size() == batch_limit) break;
        }
        if (selected.empty()) {
            result.best_effort = true;
            result.evaluated_probe_count = evaluated_probes.size();
            result.final_probe_times = std::move(evaluated_probes);
            if (sampler.approximation_used()) { result.best_effort = true; result.conformant = false; }
            out = std::move(result);
            return true;
        }
        const auto old_times = result.times;
        const auto old_values = result.values;
        for (const auto& candidate : selected) {
            const auto insertion = std::lower_bound(result.times.begin(), result.times.end(), candidate.time);
            const auto index = static_cast<std::size_t>(insertion - result.times.begin());
            result.times.insert(insertion, candidate.time);
            result.values.insert(result.values.begin() + index,
                source.path == AnimationPath::Rotation ? std::array<float, 4>{} : candidate.value);
        }
        if (source.path == AnimationPath::Rotation &&
            !rebuild_canonical_rotations(result, source_cache, error)) return false;
        // Native Hermite spans use a four-control window. Inserting a control
        // changes only spans whose window includes it. Batch independent
        // high-error spans, retaining measurements for unaffected probes.
        std::vector<std::pair<float, float>> dirty_ranges;
        dirty_ranges.reserve(selected.size());
        std::vector<bool> canonical_sign_changed(old_times.size(), false);
        for (const auto& candidate : selected) {
            const auto old_index = candidate.index;
            dirty_ranges.emplace_back(old_index > 1 ? old_times[old_index - 2] : 0.0f,
                old_index + 1 < old_times.size() ? old_times[old_index + 1]
                                                : std::numeric_limits<float>::infinity());
        }
        if (source.path == AnimationPath::Rotation) {
            for (std::size_t i = 0; i < old_times.size(); ++i) {
                const auto current = std::lower_bound(result.times.begin(), result.times.end(), old_times[i]);
                if (current == result.times.end() || *current != old_times[i]) continue;
                canonical_sign_changed[i] =
                    result.values[static_cast<std::size_t>(current - result.times.begin())] != old_values[i];
            }
        }
        // Re-hemisphering can negate a run of later quaternions. A uniform
        // sign change leaves that run's represented rotations unchanged; only
        // spans crossing a sign boundary can change their Hermite polynomial.
        for (std::size_t i = 0; i + 1 < canonical_sign_changed.size(); ++i) {
            if (canonical_sign_changed[i] == canonical_sign_changed[i + 1]) continue;
            dirty_ranges.emplace_back(i > 1 ? old_times[i - 2] : 0.0f,
                i + 2 < old_times.size() ? old_times[i + 2]
                                         : std::numeric_limits<float>::infinity());
        }
        for (const auto& range : dirty_ranges) {
            for (auto measured = measured_probes.lower_bound(range.first);
                 measured != measured_probes.end() && measured->first <= range.second; )
                measured = measured_probes.erase(measured);
        }
        ++result.refinements;
        result.refinements += selected.size() - 1;
    }
}
}
