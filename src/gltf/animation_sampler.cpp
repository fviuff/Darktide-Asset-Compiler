#include "gltf/animation_sampler.h"
#include "scene/scene.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dtglb::gltf {
namespace {
using Value = std::array<double,4>;
bool normalize(Value& value) {
    double norm = 0;
    for (double v : value) norm += v*v;
    if (!std::isfinite(norm) || norm == 0) return false;
    for (double& v : value) v /= std::sqrt(norm);
    return true;
}
}

bool PreparedAnimationSampler::initialize(const AnimationTrack& track, std::string& error) {
    approximation_used_ = false;
    const bool rotation = track.path == AnimationPath::Rotation;
    if (!rotation && track.path != AnimationPath::Translation && track.path != AnimationPath::Scale) {
        error = "source sampler supports only translation, rotation and scale"; return false;
    }
    const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
    if (!cubic && track.interpolation != AnimationInterpolation::Linear && track.interpolation != AnimationInterpolation::Step) {
        error = "source sampler interpolation is unsupported"; return false;
    }
    const std::size_t width = rotation ? 4 : 3, stride = width * (cubic ? 3 : 1);
    if (track.times.empty() ||
        track.value_components != width || track.times.size() > std::numeric_limits<std::size_t>::max()/stride ||
        track.values.size() != track.times.size()*stride) {
        error = "source sampler has invalid query or track dimensions"; return false;
    }
    if (cubic && track.times.size() < 2) {
        error = "source sampler cubic spline requires at least two keyframes"; return false;
    }
    for (std::size_t i = 0; i < track.times.size(); ++i)
        if (!std::isfinite(track.times[i]) || track.times[i] < 0 || (i && track.times[i] <= track.times[i-1])) {
            error = "source sampler times must be finite, nonnegative and strictly increasing"; return false;
        }
    for (float v : track.values) if (!std::isfinite(v)) { error = "source sampler value is non-finite"; return false; }
    track_ = &track;
    return true;
}

bool PreparedAnimationSampler::sample(float time, std::array<float,4>& out, std::string& error) const {
    if (!track_ || !std::isfinite(time) || time < 0) {
        error = "source sampler requires an initialized track and finite nonnegative query"; return false;
    }
    const auto& track = *track_;
    const bool rotation = track.path == AnimationPath::Rotation;
    const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
    const std::size_t width = rotation ? 4 : 3, stride = width * (cubic ? 3 : 1);
    auto value_at = [&](std::size_t key, std::size_t part) {
        Value result{};
        for (std::size_t c = 0; c < width; ++c) result[c] = track.values[key*stride + part*width + c];
        return result;
    };
    const std::size_t part = cubic ? 1 : 0;
    const auto upper = std::upper_bound(track.times.begin(), track.times.end(), time);
    const std::size_t left = upper == track.times.begin() ? 0 : static_cast<std::size_t>(upper-track.times.begin()-1);
    Value result = value_at(left, part);
    const bool cubic_interior = cubic && time > track.times.front() && left+1 < track.times.size() &&
        time != track.times[left];
    const double cubic_u = cubic_interior
        ? (double(time) - track.times[left]) /
            (double(track.times[left + 1]) - track.times[left])
        : 0.0;
    if (time > track.times.front() && left+1 < track.times.size() && time != track.times[left] &&
        track.interpolation != AnimationInterpolation::Step) {
        const double dt = double(track.times[left+1])-track.times[left];
        const double u = (double(time)-track.times[left])/dt;
        Value next = value_at(left+1, part);
        if (cubic) {
            const auto outgoing = value_at(left, 2), incoming = value_at(left+1, 0);
            const double u2 = u*u, u3 = u2*u;
            for (std::size_t c = 0; c < width; ++c)
                result[c] = (2*u3-3*u2+1)*result[c] + dt*(u3-2*u2+u)*outgoing[c]
                          + (-2*u3+3*u2)*next[c] + dt*(u3-u2)*incoming[c];
        } else if (rotation) {
            if (!normalize(result) || !normalize(next)) { error = "source sampler quaternion has zero norm"; return false; }
            double dot = 0; for (std::size_t c = 0; c < 4; ++c) dot += result[c]*next[c];
            if (dot < 0) { for (double& v : next) v = -v; dot = -dot; }
            dot = std::clamp(dot, 0.0, 1.0);
            const double angle = std::acos(dot), sine = std::sin(angle);
            const double a = sine > 1e-8 ? std::sin((1-u)*angle)/sine : 1-u;
            const double b = sine > 1e-8 ? std::sin(u*angle)/sine : u;
            for (std::size_t c = 0; c < 4; ++c) result[c] = a*result[c] + b*next[c];
        } else {
            for (std::size_t c = 0; c < width; ++c) result[c] = (1-u)*result[c] + u*next[c];
        }
    }
    if (cubic_interior) {
        bool valid = true;
        for (double v : result) valid = valid && std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
        if (rotation) valid = valid && normalize(result);
        if (!valid) {
            approximation_used_ = true;
            const std::size_t endpoint = cubic_u <= 0.5 ? left : left + 1;
            result = value_at(endpoint, part);
            if (rotation && !normalize(result)) result = Value{0.0, 0.0, 0.0, 1.0};
        }
    }
    if (rotation && !normalize(result)) { error = "source sampler has invalid quaternion"; return false; }
    std::array<float,4> converted{};
    for (std::size_t c = 0; c < width; ++c) {
        if (!std::isfinite(result[c]) || std::abs(result[c]) > std::numeric_limits<float>::max()) {
            error = "source sampler result is outside finite float range"; return false;
        }
        converted[c] = static_cast<float>(result[c]);
    }
    out = converted;
    return true;
}

bool sample_animation_track(const AnimationTrack& track, float time,
                            std::array<float,4>& out, std::string& error) {
    PreparedAnimationSampler sampler;
    return sampler.initialize(track, error) && sampler.sample(time, out, error);
}
}
