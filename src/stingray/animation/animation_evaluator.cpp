#include "stingray/animation/animation_evaluator.h"
#include "stingray/animation/animation_writer.h"
#include "stingray/cooked_resource.h"
#include "gltf/animation_sampler.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace dtglb::stingray::animation {
namespace {
template<class T> T read(const std::vector<std::uint8_t>& data, std::size_t offset) {
    T value{}; std::memcpy(&value, data.data()+offset, sizeof(T)); return value;
}
int channel(AnimationPath path) {
    return path == AnimationPath::Translation ? 0 : path == AnimationPath::Rotation ? 1 : path == AnimationPath::Scale ? 2 : -1;
}
struct Control { float time = 0; std::array<float,4> value{}; };
using Window = std::array<Control,4>;
bool sample_window(const Window& state, float query, int path, std::array<float,4>& out) {
    if (query >= state[3].time || (path != 2 && query >= state[2].time && state[1].time == 0 && state[2].time == 0)) {
        out = state[3].value; return true;
    }
    const Window w = query >= state[2].time ? Window{state[1],state[2],state[3],state[3]} : state;
    const double dt = double(w[2].time)-w[1].time;
    const double d0 = double(w[2].time)-w[0].time, d1 = double(w[3].time)-w[1].time;
    if (dt <= 0 || d0 <= 0 || d1 <= 0) return false;
    const double u = (double(query)-w[1].time)/dt, u2 = u*u, u3 = u2*u;
    std::array<double,4> result{};
    const int width = path == 1 ? 4 : 3;
    for (int c = 0; c < width; ++c) {
        const double m1 = (double(w[2].value[c])-w[0].value[c])*dt/d0;
        const double m2 = (double(w[3].value[c])-w[1].value[c])*dt/d1;
        result[c] = (2*u3-3*u2+1)*w[1].value[c] + (u3-2*u2+u)*m1
                  + (-2*u3+3*u2)*w[2].value[c] + (u3-u2)*m2;
    }
    if (path == 1) {
        double norm = 0; for (double v : result) norm += v*v;
        if (norm == 0) result = {0,0,0,1};
        else { if (!std::isfinite(norm)) return false; for (double& v : result) v /= std::sqrt(norm); }
    }
    for (int c = 0; c < width; ++c) {
        if (!std::isfinite(result[c]) || std::abs(result[c]) > std::numeric_limits<float>::max()) return false;
        out[c] = static_cast<float>(result[c]);
    }
    return true;
}
}

bool decode_evaluation_clip(const std::vector<std::uint8_t>& cooked, EvaluationClip& out, std::string& error) {
    std::vector<std::uint8_t> body; std::string stream;
    if (!parse_cooked_resource_envelope(cooked, "animation", body, stream, error)) return false;
    if (body.size() < 28) { error = "animation evaluator header is truncated"; return false; }
    const auto count = read<std::uint32_t>(body, 8);
    if (!count || count > 4096) { error = "animation evaluator target count is outside supported profile"; return false; }
    if (!validate_skeletal_animation(cooked, count, error)) return false;
    EvaluationClip clip; clip.duration = read<float>(body, 12); clip.initial.resize(count);
    std::size_t offset = 26;
    for (auto& pose : clip.initial) {
        for (int i = 0; i < 3; ++i) { pose.position[i] = read<float>(body, offset); offset += 4; }
        for (int i = 0; i < 4; ++i) { pose.rotation[i] = read<float>(body, offset); offset += 4; }
        for (int i = 0; i < 3; ++i) { pose.scale[i] = read<float>(body, offset); offset += 4; }
    }
    std::vector<std::array<std::vector<float>,3>> history(count);
    std::vector<std::array<float,4>> rotations;
    for (const auto& pose : clip.initial) rotations.push_back(pose.rotation);
    float prefix_gate = 0;
    while (read<std::uint16_t>(body, offset) != 3) {
        const auto kind = read<std::uint16_t>(body, offset); offset += 2;
        EvaluationKey key; key.bone = read<std::uint16_t>(body, offset); offset += 2;
        key.time = read<float>(body, offset); offset += 4;
        key.path = kind == 4 ? AnimationPath::Translation : kind == 5 ? AnimationPath::Rotation : AnimationPath::Scale;
        const int path = channel(key.path), width = kind == 5 ? 4 : 3;
        for (int i = 0; i < width; ++i) { key.value[i] = read<float>(body, offset); offset += 4; }
        auto& times = history[key.bone][path];
        const float local_gate = times.size() < 2 ? 0 : times[times.size()-2];
        prefix_gate = std::max(prefix_gate, local_gate); key.gate = prefix_gate; times.push_back(key.time);
        if (kind == 5) {
            double dot = 0; for (int i = 0; i < 4; ++i) dot += double(rotations[key.bone][i])*key.value[i];
            if (dot < 0) for (float& v : key.value) v = -v;
            rotations[key.bone] = key.value;
        }
        clip.keys.push_back(key);
    }
    out = std::move(clip); return true;
}

bool sample_native_animation(const EvaluationClip& clip, float time, std::vector<EvaluatedPose>& out, std::string& error) {
    if (!std::isfinite(time) || time < 0 || clip.initial.empty() || clip.initial.size() > 4096) {
        error = "animation evaluator requires nonnegative finite time and a nonempty supported clip"; return false;
    }
    std::vector<std::array<Window,3>> windows(clip.initial.size());
    for (std::size_t i = 0; i < windows.size(); ++i) {
        for (auto& c : windows[i][0]) c.value = clip.initial[i].position;
        for (auto& c : windows[i][1]) c.value = clip.initial[i].rotation;
        for (auto& c : windows[i][2]) c.value = clip.initial[i].scale;
    }
    for (const auto& key : clip.keys) {
        if (key.gate > time) break;
        const int path = channel(key.path);
        if (key.bone >= windows.size() || path < 0) { error = "animation evaluator key target is invalid"; return false; }
        auto& w = windows[key.bone][path];
        w = {w[1], w[2], w[3], Control{key.time,key.value}};
    }
    std::vector<EvaluatedPose> poses(windows.size());
    for (std::size_t i = 0; i < poses.size(); ++i) {
        if (!sample_window(windows[i][0], time, 0, poses[i].position) ||
            !sample_window(windows[i][1], time, 1, poses[i].rotation) ||
            !sample_window(windows[i][2], time, 2, poses[i].scale)) {
            error = "native animation curve produced a non-finite value or degenerate interval"; return false;
        }
    }
    out = std::move(poses); return true;
}

bool sample_native_track(const std::array<float,4>& initial, AnimationPath path,
                         const std::vector<float>& times,
                         const std::vector<std::array<float,4>>& values,
                         float time, std::array<float,4>& out, std::string& error) {
    const int channel_id = channel(path);
    if (channel_id < 0 || path == AnimationPath::Weights || !std::isfinite(time) || time < 0 ||
        times.size() != values.size()) {
        error = "native track sampler has an invalid contract"; return false;
    }
    Window state{};
    for (auto& control : state) { control.time = 0; control.value = initial; }
    const std::size_t admitted = std::min(times.size(),
        static_cast<std::size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin()) + 2);
    // The native consumer retains only its most recent four admitted controls.
    // Starting from implicit initial padding gives the same window without
    // replaying the full prefix for every probe.
    const std::size_t first = admitted > 4 ? admitted - 4 : 0;
    for (std::size_t i = first; i < admitted; ++i)
        state = {state[1], state[2], state[3], Control{times[i], values[i]}};
    if (!sample_window(state, time, channel_id, out)) {
        error = "native track sampler produced a non-finite value or degenerate interval"; return false;
    }
    return true;
}

bool measure_animation_error(const std::vector<std::uint8_t>& cooked, const SkinInfo& skin,
                             const AnimationInfo& source, SampledAnimationError& out, std::string& error) {
    EvaluationClip clip;
    if (!decode_evaluation_clip(cooked, clip, error)) return false;
    if (clip.initial.size() != skin.joints.size()) { error = "animation fidelity bone count mismatch"; return false; }
    std::map<int,std::size_t> bone_by_node;
    for (std::size_t i = 0; i < skin.joints.size(); ++i)
        if (!bone_by_node.emplace(skin.joints[i], i).second) { error = "animation fidelity requires distinct joint nodes"; return false; }
    // Bounded diagnostic sampling, explicitly not a certified error bound.
    // Preserve a uniform grid and supplement it with at most 192 authored-key/
    // interval-midpoint probes spread across the complete source key domain.
    std::vector<float> candidates;
    for (const auto& track : source.tracks) {
        if (!bone_by_node.count(track.target_node) || channel(track.path) < 0) {
            error = "animation fidelity source channel is outside emitted skeleton"; return false;
        }
        std::array<float,4> checked;
        if (!gltf::sample_animation_track(track, 0, checked, error)) return false;
        for (std::size_t i = 0; i < track.times.size(); ++i) {
            candidates.push_back(track.times[i]);
            if (i) candidates.push_back(static_cast<float>((double(track.times[i-1])+track.times[i])*0.5));
        }
    }
    if (candidates.empty()) { error = "animation fidelity has no source keys"; return false; }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    std::vector<float> times;
    for (int i = 0; i <= 64; ++i) times.push_back(static_cast<float>(double(clip.duration)*i/64));
    const std::size_t probes = std::min<std::size_t>(192, candidates.size());
    for (std::size_t i = 0; i < probes; ++i) times.push_back(candidates[probes == 1 ? 0 : i*(candidates.size()-1)/(probes-1)]);
    std::sort(times.begin(), times.end()); times.erase(std::unique(times.begin(), times.end()), times.end());
    SampledAnimationError report;
    for (const float time : times) {
        std::vector<EvaluatedPose> poses;
        if (!sample_native_animation(clip, time, poses, error)) return false;
        for (const auto& track : source.tracks) {
            std::array<float,4> expected;
            if (!gltf::sample_animation_track(track, time, expected, error)) return false;
            const auto& pose = poses[bone_by_node.at(track.target_node)];
            const auto& actual = track.path == AnimationPath::Translation ? pose.position : track.path == AnimationPath::Rotation ? pose.rotation : pose.scale;
            if (track.path == AnimationPath::Rotation) {
                double dot = 0, a = 0, b = 0;
                for (int i = 0; i < 4; ++i) { dot += double(expected[i])*actual[i]; a += double(expected[i])*expected[i]; b += double(actual[i])*actual[i]; }
                if (a == 0 || b == 0) { error = "animation fidelity encountered a zero quaternion"; return false; }
                report.rotation_radians = std::max(report.rotation_radians, 2*std::acos(std::clamp(std::abs(dot)/std::sqrt(a*b), 0.0, 1.0)));
            } else {
                double squared = 0; for (int i = 0; i < 3; ++i) { const double d = double(expected[i])-actual[i]; squared += d*d; }
                auto& maximum = track.path == AnimationPath::Translation ? report.translation : report.scale;
                maximum = std::max(maximum, std::sqrt(squared));
            }
        }
    }
    report.available = true; report.samples = static_cast<std::uint32_t>(times.size());
    out = std::move(report); return true;
}
}
