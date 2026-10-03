#include "processing/dangle_canonicalizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace dtglb::processing {
namespace {
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

Quat multiply(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat conjugate(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }

Vec3 rotate(const Quat& q, const Vec3& v) {
    const Quat p{v[0], v[1], v[2], 0.0f};
    const auto r = multiply(multiply(q, p), conjugate(q));
    return {r[0], r[1], r[2]};
}

Matrix4 rotation_matrix(const Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
            2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0,
            0, 0, 0, 1};
}

Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[static_cast<std::size_t>(c * 4 + row)] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] +
                                                       a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    return r;
}

// Shortest rotation taking +X onto direction d (unit length).
Quat from_x_to(const Vec3& d) {
    const float dot = d[0];
    if (dot < -0.9999f) return {0.0f, 0.0f, 1.0f, 0.0f}; // 180 degrees about Z
    const Vec3 axis{0.0f, -d[2], d[1]};                  // X cross d
    Quat q{axis[0], axis[1], axis[2], 1.0f + dot};
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto& v : q) v /= n;
    return q;
}

// Stingray basis is glTF (x, y, z) -> (x, -z, y); map a Stingray rotation back.
Quat to_gltf(const Quat& q) { return {q[0], q[2], -q[1], q[3]}; }

Vec3 local_translation(const NodeInfo& node) {
    if (node.has_exact_trs) return node.local_translation_stingray;
    return {node.local_stingray[12], node.local_stingray[13], node.local_stingray[14]};
}

bool uniform_scale(const NodeInfo& node) {
    const auto& s = node.local_scale_stingray;
    return std::abs(s[0] - s[1]) <= 1e-4f * std::abs(s[0]) + 1e-6f &&
           std::abs(s[0] - s[2]) <= 1e-4f * std::abs(s[0]) + 1e-6f;
}
} // namespace

bool canonicalize_dangles(Scene& scene, std::string& error) {
    error.clear();
    std::set<int> seen;
    for (auto& dangle : scene.asset_definition.dangles) {
        const int n = dangle.source_node;
        if (n < 0 || static_cast<std::size_t>(n) >= scene.nodes.size() || !seen.insert(n).second) {
            error = "dangling bone node is invalid or listed twice";
            return false;
        }
        auto& node = scene.nodes[static_cast<std::size_t>(n)];
        const bool is_joint = std::any_of(scene.skins.begin(), scene.skins.end(), [&](const SkinInfo& skin) {
            return std::find(skin.joints.begin(), skin.joints.end(), n) != skin.joints.end();
        });
        if (!is_joint) { error = "dangling bone '" + node.name + "' is not a skinned bone"; return false; }
        for (const auto& primitive : scene.primitives)
            if (primitive.render_owner_node == n) {
                error = "dangling bone '" + node.name + "' may not own rigid (unskinned) meshes; skin them to it instead";
                return false;
            }
        if (dangle.jiggle) continue; // springs move the bone's translation; its frame is irrelevant
        if (!uniform_scale(node)) { error = "dangling bone '" + node.name + "' needs uniform scale"; return false; }

        // Direction down the bone in its own (Stingray) frame: towards the first
        // child joint, otherwise the Blender/glTF bone axis (+Y glTF = +Z here).
        Vec3 direction{0.0f, 0.0f, 1.0f};
        float child_distance = 0.0f;
        for (const int child : node.children) {
            if (child < 0 || static_cast<std::size_t>(child) >= scene.nodes.size()) continue;
            const auto t = local_translation(scene.nodes[static_cast<std::size_t>(child)]);
            const float len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (len > 1e-5f) { direction = {t[0] / len, t[1] / len, t[2] / len}; child_distance = len * node.local_scale_stingray[0]; break; }
        }
        if (!(dangle.length > 0.0f)) dangle.length = child_distance;
        if (!(dangle.length > 0.0f) || !std::isfinite(dangle.length)) {
            error = "dangling bone '" + node.name + "' has no child joint; give it a length";
            return false;
        }

        const Quat r = from_x_to(direction);
        const Quat r_inverse = conjugate(r);
        const Matrix4 m = rotation_matrix(r), m_inverse = rotation_matrix(r_inverse);
        const Matrix4 g = rotation_matrix(to_gltf(r)), g_inverse = rotation_matrix(to_gltf(r_inverse));

        node.local_rotation_stingray = multiply(node.local_rotation_stingray, r);
        node.local_stingray = multiply(node.local_stingray, m);
        node.world_stingray = multiply(node.world_stingray, m);
        node.local_gltf = multiply(node.local_gltf, g);
        node.world_gltf = multiply(node.world_gltf, g);
        for (const int child : node.children) {
            if (child < 0 || static_cast<std::size_t>(child) >= scene.nodes.size()) continue;
            auto& c = scene.nodes[static_cast<std::size_t>(child)];
            if (!uniform_scale(c)) { error = "child of dangling bone '" + node.name + "' needs uniform scale"; return false; }
            c.local_translation_stingray = rotate(r_inverse, c.local_translation_stingray);
            c.local_rotation_stingray = multiply(r_inverse, c.local_rotation_stingray);
            c.local_stingray = multiply(m_inverse, c.local_stingray);
            c.local_gltf = multiply(g_inverse, c.local_gltf);
        }
        for (auto& skin : scene.skins) {
            for (std::size_t k = 0; k < skin.joints.size(); ++k) {
                if (skin.joints[k] != n) continue;
                if (k < skin.inverse_bind_matrices_stingray.size())
                    skin.inverse_bind_matrices_stingray[k] = multiply(m_inverse, skin.inverse_bind_matrices_stingray[k]);
                if (k < skin.inverse_bind_matrices_gltf.size())
                    skin.inverse_bind_matrices_gltf[k] = multiply(g_inverse, skin.inverse_bind_matrices_gltf[k]);
            }
        }
        const std::set<int> children(node.children.begin(), node.children.end());
        for (auto& animation : scene.animations) {
            for (auto& track : animation.tracks) {
                if (track.target_node == n && track.path == AnimationPath::Rotation) {
                    for (std::size_t i = 0; i + 3 < track.values.size(); i += 4) {
                        const auto q = multiply(Quat{track.values[i], track.values[i + 1], track.values[i + 2], track.values[i + 3]}, r);
                        std::copy(q.begin(), q.end(), track.values.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                } else if (children.count(track.target_node) && track.path == AnimationPath::Rotation) {
                    for (std::size_t i = 0; i + 3 < track.values.size(); i += 4) {
                        const auto q = multiply(r_inverse, Quat{track.values[i], track.values[i + 1], track.values[i + 2], track.values[i + 3]});
                        std::copy(q.begin(), q.end(), track.values.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                } else if (children.count(track.target_node) && track.path == AnimationPath::Translation) {
                    for (std::size_t i = 0; i + 2 < track.values.size(); i += 3) {
                        const auto t = rotate(r_inverse, {track.values[i], track.values[i + 1], track.values[i + 2]});
                        std::copy(t.begin(), t.end(), track.values.begin() + static_cast<std::ptrdiff_t>(i));
                    }
                }
            }
        }
        scene.notes.push_back("dangling bone '" + node.name + "': rest frame turned so +X runs down the bone (length " +
                              std::to_string(dangle.length) + ")");
    }
    return true;
}

} // namespace dtglb::processing
