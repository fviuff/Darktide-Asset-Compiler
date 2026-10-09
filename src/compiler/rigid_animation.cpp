#include "compiler/rigid_animation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace dtglb::compiler {
namespace {

constexpr float kEpsilon = 1e-5f;

bool finite_matrix(const Matrix4& m) {
    return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
}

bool rigid_trs_matrix(const Matrix4& m) {
    if (!finite_matrix(m) || std::fabs(m[3]) > kEpsilon || std::fabs(m[7]) > kEpsilon ||
        std::fabs(m[11]) > kEpsilon || std::fabs(m[15] - 1.0f) > kEpsilon) return false;
    const std::array<float, 3> a{m[0],m[1],m[2]}, b{m[4],m[5],m[6]}, c{m[8],m[9],m[10]};
    const auto dot = [](const auto& x, const auto& y) { return x[0]*y[0]+x[1]*y[1]+x[2]*y[2]; };
    const float la = dot(a,a), lb = dot(b,b), lc = dot(c,c);
    return la > 1e-20f && lb > 1e-20f && lc > 1e-20f &&
        std::fabs(dot(a,b)) <= kEpsilon * std::sqrt(la*lb) &&
        std::fabs(dot(a,c)) <= kEpsilon * std::sqrt(la*lc) &&
        std::fabs(dot(b,c)) <= kEpsilon * std::sqrt(lb*lc);
}

bool inverse(const Matrix4& input, Matrix4& output) {
    float a[4][8]{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) a[r][c] = input[c*4+r];
    for (int r=0;r<4;++r) a[r][4+r] = 1.0f;
    for (int c=0;c<4;++c) {
        int pivot = c;
        for (int r=c+1;r<4;++r) if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) <= 1e-12f || !std::isfinite(a[pivot][c])) return false;
        for (int j=0;j<8;++j) std::swap(a[c][j],a[pivot][j]);
        const float scale = a[c][c];
        for (int j=0;j<8;++j) a[c][j] /= scale;
        for (int r=0;r<4;++r) if (r != c) {
            const float factor = a[r][c];
            for (int j=0;j<8;++j) a[r][j] -= factor*a[c][j];
        }
    }
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) output[c*4+r] = a[r][4+c];
    return finite_matrix(output);
}

bool validate_hierarchy(const Scene& s, std::string& error) {
    std::vector<unsigned> child_occurrences(s.nodes.size());
    for (std::size_t i = 0; i < s.nodes.size(); ++i) {
        const int parent = s.nodes[i].parent;
        if (parent >= 0 && (static_cast<std::size_t>(parent) >= s.nodes.size() || parent == static_cast<int>(i))) {
            error = "rigid animation hierarchy has an invalid parent"; return false;
        }
        for (int child : s.nodes[i].children) {
            if (child < 0 || static_cast<std::size_t>(child) >= s.nodes.size() || s.nodes[static_cast<std::size_t>(child)].parent != static_cast<int>(i)) {
                error = "rigid animation hierarchy has inconsistent children"; return false;
            }
            if (++child_occurrences[static_cast<std::size_t>(child)] != 1) {
                error = "rigid animation hierarchy repeats a child"; return false;
            }
        }
    }
    std::vector<unsigned char> state(s.nodes.size());
    for (std::size_t start = 0; start < s.nodes.size(); ++start) {
        if (s.nodes[start].parent >= 0 && child_occurrences[start] != 1) {
            error = "rigid animation parent is missing its child"; return false;
        }
        int node = static_cast<int>(start);
        while (node >= 0 && state[static_cast<std::size_t>(node)] == 0) {
            state[static_cast<std::size_t>(node)] = 1;
            node = s.nodes[static_cast<std::size_t>(node)].parent;
        }
        if (node >= 0 && state[static_cast<std::size_t>(node)] == 1) {
            error = "rigid animation hierarchy contains a cycle"; return false;
        }
        node = static_cast<int>(start);
        while (node >= 0 && state[static_cast<std::size_t>(node)] == 1) {
            state[static_cast<std::size_t>(node)] = 2;
            node = s.nodes[static_cast<std::size_t>(node)].parent;
        }
    }
    return true;
}

bool ancestor_closure(const Scene& s, int node, std::vector<bool>& keep, std::string& error) {
    while (node >= 0) {
        if (static_cast<std::size_t>(node) >= s.nodes.size()) { error = "rigid animation node parent is invalid"; return false; }
        if (keep[static_cast<std::size_t>(node)]) return true;
        keep[static_cast<std::size_t>(node)] = true;
        node = s.nodes[static_cast<std::size_t>(node)].parent;
    }
    return true;
}

bool validate_track(const AnimationTrack& track, std::size_t node_count, std::string& error) {
    if (track.target_node < 0 || static_cast<std::size_t>(track.target_node) >= node_count) { error = "rigid animation target node is invalid"; return false; }
    if (track.path == AnimationPath::Weights) { error = "rigid animation does not support morph weight tracks"; return false; }
    const std::uint32_t components = track.path == AnimationPath::Rotation ? 4u : 3u;
    if (track.value_components != components || track.times.empty()) { error = "rigid animation has unsupported TRS sampler shape"; return false; }
    if (track.times.front() < 0 || std::adjacent_find(track.times.begin(), track.times.end(),
        [](float a, float b) { return a >= b; }) != track.times.end()) {
        error = "rigid animation times must be nonnegative and strictly increasing"; return false;
    }
    const std::size_t multiplier = track.interpolation == AnimationInterpolation::CubicSpline ? 3u : 1u;
    if (track.values.size() != track.times.size() * components * multiplier ||
        !std::all_of(track.times.begin(), track.times.end(), [](float v) { return std::isfinite(v); }) ||
        !std::all_of(track.values.begin(), track.values.end(), [](float v) { return std::isfinite(v); })) {
        error = "rigid animation contains invalid sampler values"; return false;
    }
    return true;
}

const VertexChannel* find_position(const Primitive& primitive) {
    for (const auto& channel : primitive.channels)
        if (channel.semantic == VertexChannel::Semantic::Position && channel.set == 0) return &channel;
    return nullptr;
}

} // namespace

bool lower_rigid_animation(const Scene& source, Scene& lowered, std::string& error) {
    error.clear();
    if (&source == &lowered) { error = "rigid animation requires a separate output scene"; return false; }
    if (source.primitives.empty() || source.animations.empty()) { error = "rigid animation requires geometry and animations"; return false; }
    if (!validate_hierarchy(source, error)) return false;
    std::vector<bool> active(source.nodes.size());
    std::vector<int> pending(source.scene_roots);
    while (!pending.empty()) {
        const int node = pending.back(); pending.pop_back();
        if (node < 0 || static_cast<std::size_t>(node) >= source.nodes.size()) {
            error = "rigid animation has an invalid scene root"; return false;
        }
        if (active[static_cast<std::size_t>(node)]) continue;
        active[static_cast<std::size_t>(node)] = true;
        const auto& children = source.nodes[static_cast<std::size_t>(node)].children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    for (const auto& animation : source.animations) if (animation.tracks.empty()) { error = "rigid animation clip is empty"; return false; }
    std::vector<bool> keep(source.nodes.size(), false);
    for (const auto& p : source.primitives) {
        if (p.source_node < 0 || static_cast<std::size_t>(p.source_node) >= source.nodes.size()) { error = "rigid animation primitive owner is invalid"; return false; }
        if (!active[static_cast<std::size_t>(p.source_node)]) { error = "rigid primitive owner is outside the selected scene"; return false; }
        const auto& owner = source.nodes[static_cast<std::size_t>(p.source_node)];
        if (owner.skin >= 0 || !owner.instance_attributes.empty()) { error = "rigid animation rejects existing skins and GPU instancing"; return false; }
        for (const auto& c : p.source_channels) if (c.semantic == VertexChannel::Semantic::BlendIndices || c.semantic == VertexChannel::Semantic::BlendWeights) { error = "rigid animation rejects authored blend inputs"; return false; }
        for (const auto& c : p.channels) if (c.semantic == VertexChannel::Semantic::BlendIndices || c.semantic == VertexChannel::Semantic::BlendWeights) { error = "rigid animation rejects authored blend inputs"; return false; }
        const auto* position = find_position(p);
        if (!position || position->components != 3 || position->values.empty() || position->values.size() % position->components != 0 ||
            !std::all_of(position->values.begin(), position->values.end(), [](float v) { return std::isfinite(v); })) {
            error = "rigid animation requires a finite POSITION channel"; return false;
        }
        const auto vertices = position->values.size() / 3;
        for (const auto& channel : p.channels) {
            if (!channel.components || channel.values.size() / channel.components != vertices ||
                channel.values.size() % channel.components != 0 ||
                !std::all_of(channel.values.begin(), channel.values.end(), [](float v) { return std::isfinite(v); })) {
                error = "rigid animation vertex channels disagree"; return false;
            }
        }
        if (!ancestor_closure(source, p.source_node, keep, error)) return false;
    }
    const auto retain_physics_node = [&](int node, const char* description) {
        if (node < 0 || static_cast<std::size_t>(node) >= source.nodes.size()) {
            error = std::string("rigid animation ") + description + " node is invalid";
            return false;
        }
        if (!active[static_cast<std::size_t>(node)]) {
            error = std::string("rigid animation ") + description + " node is outside the selected scene";
            return false;
        }
        return ancestor_closure(source, node, keep, error);
    };
    for (const auto& primitive : source.primitives) {
        if (primitive.render_owner_node < -1) {
            error = "rigid animation render primitive has an invalid owner node";
            return false;
        }
        if (primitive.render_owner_node >= 0 && !retain_physics_node(primitive.render_owner_node, "render owner")) return false;
    }
    if (source.asset_definition.body) {
        const int node = source.asset_definition.body->source_node;
        if (node < -1) {
            error = "rigid animation asset body node is invalid";
            return false;
        }
        if (node >= 0 && !retain_physics_node(node, "asset body")) return false;
    }
    for (const auto& body : source.asset_definition.node_bodies)
        if (!retain_physics_node(body.source_node, "body")) return false;
    for (const auto& joint : source.asset_definition.joints)
        if (!retain_physics_node(joint.source_node, "joint")) return false;
    for (const auto& collider : source.asset_definition.colliders)
        if (!retain_physics_node(collider.source_node, "collider")) return false;
    for (const auto& actor : source.asset_definition.node_actors)
        if (!retain_physics_node(actor.source_node, "actor")) return false;
    for (const auto& primitive : source.collider_primitives) {
        if (primitive.source_node < -1 || primitive.collision_object < -1 || primitive.render_owner_node < -1) {
            error = "rigid animation collider geometry has an invalid node reference";
            return false;
        }
        if (primitive.source_node >= 0 && !retain_physics_node(primitive.source_node, "collider geometry")) return false;
        if (primitive.collision_object >= 0 && !retain_physics_node(primitive.collision_object, "collision object")) return false;
        if (primitive.render_owner_node >= 0 && !retain_physics_node(primitive.render_owner_node, "render owner")) return false;
    }
    for (const auto& animation : source.animations) for (const auto& track : animation.tracks) {
        if (!validate_track(track, source.nodes.size(), error)) return false;
        if (!active[static_cast<std::size_t>(track.target_node)]) { error = "rigid animation target is outside the selected scene"; return false; }
        if (!ancestor_closure(source, track.target_node, keep, error)) return false;
    }
    for (std::size_t i=0;i<source.nodes.size();++i) if (keep[i]) {
        if (source.nodes[i].skin >= 0 || !source.nodes[i].instance_attributes.empty()) { error = "rigid animation rejects existing skins and GPU instancing"; return false; }
        if (!rigid_trs_matrix(source.nodes[i].world_stingray) || !rigid_trs_matrix(source.nodes[i].local_stingray)) { error = "rigid animation requires finite, nonsingular, unsheared transforms"; return false; }
        const auto& node = source.nodes[i];
        Matrix4 expected = node.local_stingray;
        if (node.parent >= 0) {
            expected = {};
            const auto& parent = source.nodes[static_cast<std::size_t>(node.parent)].world_stingray;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) expected[c*4+r] += parent[k*4+r]*node.local_stingray[c*4+k];
        }
        for (std::size_t element = 0; element < 16; ++element) {
            if (std::abs(expected[element] - node.world_stingray[element]) >
                1e-4f * std::max(1.0f, std::abs(expected[element]))) {
                error = "rigid animation rest-world transform disagrees with hierarchy"; return false;
            }
        }
    }
    std::vector<int> order, remap(source.nodes.size(), -1);
    for (std::size_t i=0;i<keep.size();++i) if (keep[i]) { remap[i] = static_cast<int>(order.size()); order.push_back(static_cast<int>(i)); }
    if (order.size() > 256u) { error = "rigid animation retained node closure exceeds 256 joints"; return false; }
    Scene result = source;
    result.nodes.clear(); result.nodes.reserve(order.size());
    for (int old : order) {
        NodeInfo node = source.nodes[static_cast<std::size_t>(old)];
        node.parent = node.parent < 0 ? -1 : remap[static_cast<std::size_t>(node.parent)];
        node.children.clear();
        for (int child : source.nodes[static_cast<std::size_t>(old)].children)
            if (remap[static_cast<std::size_t>(child)] >= 0) node.children.push_back(remap[static_cast<std::size_t>(child)]);
        node.skin = -1;
        result.nodes.push_back(std::move(node));
    }
    const auto remap_roots = [&](const std::vector<int>& roots) {
        std::vector<int> mapped;
        for (int root : roots)
            if (root >= 0 && static_cast<std::size_t>(root) < remap.size() && remap[root] >= 0) mapped.push_back(remap[root]);
        return mapped;
    };
    result.scene_roots = remap_roots(source.scene_roots);
    for (std::size_t i = 0; i < result.scenes.size(); ++i) result.scenes[i].roots = remap_roots(source.scenes[i].roots);
    if (result.asset_definition.body && result.asset_definition.body->source_node >= 0)
        result.asset_definition.body->source_node = remap[static_cast<std::size_t>(result.asset_definition.body->source_node)];
    for (auto& body : result.asset_definition.node_bodies)
        body.source_node = remap[static_cast<std::size_t>(body.source_node)];
    for (auto& joint : result.asset_definition.joints)
        joint.source_node = remap[static_cast<std::size_t>(joint.source_node)];
    for (auto& collider : result.asset_definition.colliders)
        collider.source_node = remap[static_cast<std::size_t>(collider.source_node)];
    for (auto& actor : result.asset_definition.node_actors)
        actor.source_node = remap[static_cast<std::size_t>(actor.source_node)];
    for (auto& member : result.asset_definition.visibility_groups)
        member.source_node = remap[static_cast<std::size_t>(member.source_node)];
    for (auto& level : result.asset_definition.lod_levels)
        level.source_node = remap[static_cast<std::size_t>(level.source_node)];
    for (auto& setting : result.asset_definition.render_settings)
        setting.source_node = remap[static_cast<std::size_t>(setting.source_node)];
    for (auto& node : result.asset_definition.shadow_lights) node = remap[static_cast<std::size_t>(node)];
    for (auto& primitive : result.collider_primitives) {
        if (primitive.source_node >= 0) primitive.source_node = remap[static_cast<std::size_t>(primitive.source_node)];
        if (primitive.collision_object >= 0) primitive.collision_object = remap[static_cast<std::size_t>(primitive.collision_object)];
        if (primitive.render_owner_node >= 0) primitive.render_owner_node = remap[static_cast<std::size_t>(primitive.render_owner_node)];
    }
    SkinInfo skin;
    skin.name = "__rigid_animation_skin";
    skin.skeleton_root = result.scene_roots.size() == 1 ? result.scene_roots.front() : -1;
    skin.joints.resize(order.size());
    std::iota(skin.joints.begin(), skin.joints.end(), 0);
    for (int old : order) {
        skin.joint_names.push_back(source.nodes[static_cast<std::size_t>(old)].name);
        Matrix4 ibm{};
        if (!inverse(source.nodes[static_cast<std::size_t>(old)].world_stingray, ibm)) {
            error = "rigid animation transform is singular"; return false;
        }
        skin.inverse_bind_matrices_stingray.push_back(ibm);
    }
    const int skin_index = static_cast<int>(result.skins.size()); result.skins.push_back(std::move(skin));
    for (std::size_t i = 0; i < result.primitives.size(); ++i) {
        auto& p = result.primitives[i];
        const int old = source.primitives[i].source_node;
        p.source_node = remap[static_cast<std::size_t>(old)];
        if (p.render_owner_node >= 0) p.render_owner_node = remap[static_cast<std::size_t>(p.render_owner_node)];
        result.nodes[static_cast<std::size_t>(p.source_node)].skin = skin_index;
        p.source_channels = p.channels;
        p.source_bounds_min = p.bounds_min; p.source_bounds_max = p.bounds_max; p.source_bounds_radius = p.bounds_radius;
        const auto* position = find_position(p);
        const std::size_t vertices = position->values.size() / position->components;
        VertexChannel ji{VertexChannel::Semantic::BlendIndices, 0, 4, std::vector<float>(vertices * 4, 0)};
        VertexChannel wi{VertexChannel::Semantic::BlendWeights, 0, 4, std::vector<float>(vertices * 4, 0)};
        const float slot = static_cast<float>(p.source_node);
        for (std::size_t v = 0; v < vertices; ++v) { ji.values[v * 4] = slot; wi.values[v * 4] = 1; }
        p.channels.push_back(ji); p.channels.push_back(wi);
        p.source_channels.push_back(std::move(ji)); p.source_channels.push_back(std::move(wi));
    }
    for (auto& animation : result.animations) for (auto& track : animation.tracks)
        track.target_node = remap[static_cast<std::size_t>(track.target_node)];
    lowered = std::move(result);
    return true;
}

} // namespace dtglb::compiler
