#include "processing/root_motion.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace dtglb::processing {
namespace {
using Vec3 = std::array<float, 3>;

constexpr float kMinTravel = 0.01f; // metres; less is a loop that already closes

int depth(const Scene& scene, int node) {
    int d = 0;
    for (int p = scene.nodes[static_cast<std::size_t>(node)].parent; p >= 0; p = scene.nodes[static_cast<std::size_t>(p)].parent) ++d;
    return d;
}

using Mat3 = std::array<float, 9>; // column-major

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r[static_cast<std::size_t>(c * 3 + row)] = a[row] * b[c * 3] + a[3 + row] * b[c * 3 + 1] + a[6 + row] * b[c * 3 + 2];
    return r;
}

// A node's local rotation/scale at the clip's first key: its animated rotation with the rest scale, or the rest pose.
Mat3 local_at_start(const Scene& scene, const AnimationInfo& animation, int node) {
    const auto& m = scene.nodes[static_cast<std::size_t>(node)].local_stingray;
    const Mat3 rest{m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
    for (const auto& track : animation.tracks) {
        if (track.target_node != node || track.path != AnimationPath::Rotation || track.value_components != 4) continue;
        const std::size_t first = track.interpolation == AnimationInterpolation::CubicSpline ? 4 : 0;
        if (track.values.size() < first + 4) break;
        const float x = track.values[first], y = track.values[first + 1], z = track.values[first + 2], w = track.values[first + 3];
        const Mat3 rotation{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                            2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                            2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
        Mat3 out = rotation;
        for (int c = 0; c < 3; ++c) {
            const float scale = std::sqrt(rest[c * 3] * rest[c * 3] + rest[c * 3 + 1] * rest[c * 3 + 1] + rest[c * 3 + 2] * rest[c * 3 + 2]);
            for (int row = 0; row < 3; ++row) out[static_cast<std::size_t>(c * 3 + row)] *= scale;
        }
        return out;
    }
    return rest;
}

// Rotation/scale of a node's parent in the world at the clip's first key, identity for scene roots. Tracks and
// node transforms are already in Stingray space (Z up) here.
Mat3 parent_basis(const Scene& scene, const AnimationInfo& animation, int node) {
    std::vector<int> chain;
    for (int p = scene.nodes[static_cast<std::size_t>(node)].parent; p >= 0; p = scene.nodes[static_cast<std::size_t>(p)].parent)
        chain.push_back(p);
    Mat3 basis{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) basis = multiply(basis, local_at_start(scene, animation, *it));
    return basis;
}

Vec3 transform_vec(const std::array<float, 9>& m, const Vec3& v) {
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
            m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}

bool invert(const std::array<float, 9>& m, std::array<float, 9>& out) {
    const float a = m[0], b = m[3], c = m[6], d = m[1], e = m[4], f = m[7], g = m[2], h = m[5], i = m[8];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f) return false;
    const float k = 1.0f / det;
    // column-major inverse
    out = {(e * i - f * h) * k, (f * g - d * i) * k, (d * h - e * g) * k,
           (c * h - b * i) * k, (a * i - c * g) * k, (b * g - a * h) * k,
           (b * f - c * e) * k, (c * d - a * f) * k, (a * e - b * d) * k};
    return true;
}

// Key value (Linear/Step: one vec3 per key; CubicSpline: in-tangent, value, out-tangent).
float& component(AnimationTrack& track, std::size_t key, std::size_t slot, std::size_t axis) {
    const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
    return track.values[(cubic ? key * 3 + slot : key) * 3 + axis];
}
} // namespace

bool remove_root_motion(Scene& scene, std::string& error) {
    error.clear();
    std::set<int> joints;
    for (const auto& skin : scene.skins) joints.insert(skin.joints.begin(), skin.joints.end());
    // Travel sits on a joint that carries the body: at least half the skin joints below it (not a limb or tail).
    std::set<int> carriers;
    for (const int joint : joints) {
        std::size_t below = 0;
        for (const int other : joints)
            for (int p = scene.nodes[static_cast<std::size_t>(other)].parent; p >= 0; p = scene.nodes[static_cast<std::size_t>(p)].parent)
                if (p == joint) { ++below; break; }
        if (below * 2 >= joints.size()) carriers.insert(joint);
    }
    for (auto& animation : scene.animations) {
        AnimationTrack* travel = nullptr;
        Vec3 horizontal{}, local{};
        for (auto& track : animation.tracks) {
            if (track.path != AnimationPath::Translation || !carriers.count(track.target_node) || track.times.size() < 2 ||
                track.value_components != 3) continue;
            const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
            if (track.values.size() != track.times.size() * 3 * (cubic ? 3 : 1)) {
                error = "animation '" + animation.name + "' has a malformed translation track";
                return false;
            }
            const std::size_t last = track.times.size() - 1;
            Vec3 delta{};
            for (std::size_t a = 0; a < 3; ++a) delta[a] = component(track, last, 1, a) - component(track, 0, 1, a);
            const auto basis = parent_basis(scene, animation, track.target_node);
            auto world = transform_vec(basis, delta);
            world[2] = 0.0f; // Z is up: keep the vertical motion
            if (std::hypot(world[0], world[1]) < kMinTravel) continue;
            if (travel && depth(scene, travel->target_node) <= depth(scene, track.target_node)) continue;
            Mat3 inverse{};
            if (!invert(basis, inverse)) continue;
            travel = &track;
            horizontal = world;
            local = transform_vec(inverse, world);
        }
        if (!travel) continue;
        const float start = travel->times.front(), span = travel->times.back() - start;
        if (!(span > 0.0f)) continue;
        const bool cubic = travel->interpolation == AnimationInterpolation::CubicSpline;
        for (std::size_t key = 0; key < travel->times.size(); ++key) {
            const float t = (travel->times[key] - start) / span;
            for (std::size_t a = 0; a < 3; ++a) {
                component(*travel, key, 1, a) -= local[a] * t;
                if (cubic) {
                    component(*travel, key, 0, a) -= local[a] / span;
                    component(*travel, key, 2, a) -= local[a] / span;
                }
            }
        }
        const float distance = std::hypot(horizontal[0], horizontal[1]);
        char text[256];
        std::snprintf(text, sizeof text, "clip '%s' plays in place: removed %.2f m of travel on '%s' over %.2f s, "
                      "move the unit at %.2f m/s", animation.name.c_str(), distance,
                      scene.nodes[static_cast<std::size_t>(travel->target_node)].name.c_str(), span, distance / span);
        scene.notes.emplace_back(text);
    }
    return true;
}

} // namespace dtglb::processing
