#include "processing/animation_canonicalizer.h"

#include "compiler/rigid_animation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace dtglb::processing {
namespace {

bool finite_matrix(const Matrix4& matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
}

bool finite_channel(const VertexChannel& channel) {
    return channel.components != 0 && channel.values.size() % channel.components == 0 &&
        std::all_of(channel.values.begin(), channel.values.end(), [](float value) { return std::isfinite(value); });
}

bool inverse_matrix(const Matrix4& input, Matrix4& output) {
    float a[4][8]{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) a[r][c] = input[c * 4 + r];
    for (int r = 0; r < 4; ++r) a[r][4 + r] = 1.0f;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (!std::isfinite(a[pivot][c]) || std::fabs(a[pivot][c]) <= 1e-12f) return false;
        for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const float scale = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= scale;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const float factor = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= factor * a[c][j];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) output[c * 4 + r] = a[r][4 + c];
    return finite_matrix(output);
}

bool same_track(const AnimationTrack& a, const AnimationTrack& b) {
    return a.target_node == b.target_node && a.path == b.path &&
        a.interpolation == b.interpolation && a.times == b.times &&
        a.values == b.values && a.value_components == b.value_components;
}

bool same_step_value(const AnimationTrack& track, std::size_t first, std::size_t second) {
    const std::size_t components = track.value_components;
    const std::size_t a = first * components;
    const std::size_t b = second * components;
    return std::equal(track.values.begin() + static_cast<std::ptrdiff_t>(a),
                      track.values.begin() + static_cast<std::ptrdiff_t>(a + components),
                      track.values.begin() + static_cast<std::ptrdiff_t>(b));
}

void note(Scene& scene, AnimationCanonicalizationReport& report, std::size_t index,
          const char* code, const std::string& message, bool lossy = true);

void canonicalize_clip(AnimationInfo& animation, Scene& scene,
                       AnimationCanonicalizationReport& report, std::size_t index) {
    std::vector<AnimationTrack> tracks;
    std::vector<std::pair<int, AnimationPath>> keys;
    tracks.reserve(animation.tracks.size());
    keys.reserve(animation.tracks.size());
    for (auto& track : animation.tracks) {
        const auto key = std::make_pair(track.target_node, track.path);
        auto found = std::find(keys.begin(), keys.end(), key);
        if (found == keys.end()) {
            keys.push_back(key);
            tracks.push_back(std::move(track));
        } else {
            const std::size_t position = static_cast<std::size_t>(found - keys.begin());
            const bool identical = same_track(tracks[position], track);
            if (!identical) tracks[position] = std::move(track);
            note(scene, report, index, "duplicate_animation_channel",
                 "animation " + std::to_string(index) + (identical
                    ? ": removed an identical duplicate animation channel"
                    : ": duplicate animation channel; last authored channel wins"), !identical);
        }
    }
    animation.tracks = std::move(tracks);
    for (auto& track : animation.tracks) {
        if (track.interpolation != AnimationInterpolation::Step || track.times.size() < 3) continue;
        std::vector<float> times;
        std::vector<float> values;
        times.reserve(track.times.size());
        values.reserve(track.values.size());
        times.push_back(track.times.front());
        values.insert(values.end(), track.values.begin(), track.values.begin() + track.value_components);
        for (std::size_t i = 1; i < track.times.size(); ++i) {
            const bool redundant = i + 1 < track.times.size() && same_step_value(track, i - 1, i);
            if (redundant) continue;
            times.push_back(track.times[i]);
            const auto begin = track.values.begin() + static_cast<std::ptrdiff_t>(i * track.value_components);
            values.insert(values.end(), begin, begin + track.value_components);
        }
        if (times.size() != track.times.size()) {
            track.times = std::move(times);
            track.values = std::move(values);
            note(scene, report, index, "step_animation_compacted",
                 "animation " + std::to_string(index) + ": compacted redundant STEP keys", false);
        }
        // The native writer expands STEP boundaries.  Keep the first and
        // final samples and deterministically thin only the genuinely
        // non-redundant transitions when that expansion would exceed its
        // per-track budget.
        constexpr std::size_t max_step_transitions = 2500u;
        if (track.interpolation == AnimationInterpolation::Step &&
            track.times.size() > max_step_transitions + 1u) {
            const std::size_t old_size = track.times.size();
            std::vector<float> decimated_times;
            std::vector<float> decimated_values;
            decimated_times.reserve(max_step_transitions + 1u);
            decimated_values.reserve((max_step_transitions + 1u) * track.value_components);
            for (std::size_t out = 0; out <= max_step_transitions; ++out) {
                const std::size_t source = (out * (old_size - 1u)) / max_step_transitions;
                decimated_times.push_back(track.times[source]);
                const auto begin = track.values.begin() + static_cast<std::ptrdiff_t>(source * track.value_components);
                decimated_values.insert(decimated_values.end(), begin, begin + track.value_components);
            }
            track.times = std::move(decimated_times);
            track.values = std::move(decimated_values);
            note(scene, report, index, "step_animation_decimated",
                 "animation " + std::to_string(index) + ": decimated non-redundant STEP transitions to the native limit");
        }
    }
}

void canonicalize_rotation_keys(AnimationInfo& animation, Scene& scene,
                                AnimationCanonicalizationReport& report, std::size_t index) {
    bool used_identity_fallback = false;
    for (auto& track : animation.tracks) {
        if (track.path != AnimationPath::Rotation || track.value_components != 4) continue;
        const std::size_t stride = track.interpolation == AnimationInterpolation::CubicSpline ? 12u : 4u;
        const std::size_t value_offset = track.interpolation == AnimationInterpolation::CubicSpline ? 4u : 0u;
        for (std::size_t key = 0; key < track.times.size(); ++key) {
            float* quaternion = track.values.data() + key * stride + value_offset;
            const double length = std::hypot(std::hypot(static_cast<double>(quaternion[0]),
                                                        static_cast<double>(quaternion[1])),
                                             std::hypot(static_cast<double>(quaternion[2]),
                                                        static_cast<double>(quaternion[3])));
            if (!(length > 0.0) || !std::isfinite(length)) {
                quaternion[0] = quaternion[1] = quaternion[2] = 0.0f;
                quaternion[3] = 1.0f;
                used_identity_fallback = true;
                continue;
            }
            for (std::size_t component = 0; component < 4; ++component)
                quaternion[component] = static_cast<float>(static_cast<double>(quaternion[component]) / length);
        }
    }
    if (used_identity_fallback)
        note(scene, report, index, "animation_rotation_identity_fallback",
             "animation " + std::to_string(index) +
             ": replaced a zero-length rotation key with the identity quaternion");
}

void synthesize_empty_clips(Scene& scene, AnimationCanonicalizationReport& report,
                            int preferred_target = -1) {
    if (scene.nodes.empty()) return;
    int target = preferred_target;
    if (target < 0 || static_cast<std::size_t>(target) >= scene.nodes.size())
        target = scene.scene_roots.empty() ? 0 : scene.scene_roots.front();
    if (target < 0 || static_cast<std::size_t>(target) >= scene.nodes.size()) target = 0;
    for (std::size_t index = 0; index < scene.animations.size(); ++index) {
        auto& clip = scene.animations[index];
        if (!clip.tracks.empty()) continue;
        AnimationTrack track;
        track.target_node = target;
        track.path = AnimationPath::Translation;
        track.interpolation = AnimationInterpolation::Linear;
        track.value_components = 3;
        track.times = {0.0f};
        track.values.assign(scene.nodes[static_cast<std::size_t>(target)].local_translation_stingray.begin(),
                            scene.nodes[static_cast<std::size_t>(target)].local_translation_stingray.end());
        clip.tracks.push_back(std::move(track));
        note(scene, report, index, "animation_static_clip_synthesized",
             "animation " + std::to_string(index) +
             ": all authored channels were lowered away; retained the clip as a deterministic static transform");
    }
}

std::set<std::size_t> active_skin_indices(const Scene& scene) {
    std::set<std::size_t> result;
    for (const auto& primitive : scene.primitives) {
        if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= scene.nodes.size()) continue;
        const int skin = scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin;
        if (skin >= 0 && static_cast<std::size_t>(skin) < scene.skins.size()) result.insert(static_cast<std::size_t>(skin));
    }
    if (!result.empty()) return result;
    std::vector<int> pending(scene.scene_roots.begin(), scene.scene_roots.end());
    std::set<int> visited;
    while (!pending.empty()) {
        const int node_index = pending.back(); pending.pop_back();
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= scene.nodes.size() || !visited.insert(node_index).second) continue;
        const auto& node = scene.nodes[static_cast<std::size_t>(node_index)];
        if (node.skin >= 0 && static_cast<std::size_t>(node.skin) < scene.skins.size()) result.insert(static_cast<std::size_t>(node.skin));
        pending.insert(pending.end(), node.children.begin(), node.children.end());
    }
    return result;
}

void select_animation_skin(Scene& scene, AnimationCanonicalizationReport& report) {
    const bool primitive_skin = std::any_of(scene.primitives.begin(), scene.primitives.end(), [&](const Primitive& primitive) {
        return primitive.source_node >= 0 && static_cast<std::size_t>(primitive.source_node) < scene.nodes.size() &&
            scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin >= 0;
    });
    if (primitive_skin) return;
    const auto active = active_skin_indices(scene);
    if (active.size() <= 1) return;
    std::size_t selected = *active.begin();
    std::size_t selected_targets = 0;
    for (const auto skin_index : active) {
        std::set<int> joints(scene.skins[skin_index].joints.begin(), scene.skins[skin_index].joints.end());
        std::size_t targets = 0;
        for (const auto& animation : scene.animations) for (const auto& track : animation.tracks)
            if (joints.count(track.target_node)) ++targets;
        if (targets > selected_targets) {
            selected = skin_index;
            selected_targets = targets;
        }
    }
    const std::set<int> selected_joints(scene.skins[selected].joints.begin(), scene.skins[selected].joints.end());
    std::set<int> foreign_joints;
    for (const auto skin_index : active) {
        if (skin_index == selected) continue;
        foreign_joints.insert(scene.skins[skin_index].joints.begin(), scene.skins[skin_index].joints.end());
    }
    for (std::size_t animation_index = 0; animation_index < scene.animations.size(); ++animation_index) {
        auto& tracks = scene.animations[animation_index].tracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const AnimationTrack& track) {
            if (!foreign_joints.count(track.target_node) || selected_joints.count(track.target_node)) return false;
            note(scene, report, animation_index, "animation_track_foreign_skin_drop",
                 "animation " + std::to_string(animation_index) +
                 ": dropped a channel belonging exclusively to a non-selected animation skin");
            return true;
        }), tracks.end());
    }
    for (auto& node : scene.nodes) {
        if (node.skin >= 0 && active.count(static_cast<std::size_t>(node.skin)) &&
            static_cast<std::size_t>(node.skin) != selected) node.skin = -1;
    }
    note(scene, report, 0, "animation_skin_selected",
         "animation-only scene selected native skin " + std::to_string(selected) +
         " by maximum animation target retention; non-selected active attachments were cleared");
    const int static_target = scene.skins[selected].joints.empty() ? -1 : scene.skins[selected].joints.front();
    synthesize_empty_clips(scene, report, static_target);
}

void synthesize_animation_only_skin(Scene& scene, AnimationCanonicalizationReport& report) {
    if (!scene.primitives.empty() || scene.animations.empty() || !active_skin_indices(scene).empty() ||
        scene.nodes.empty()) return;
    constexpr std::size_t palette_limit = 4096u;
    SkinInfo skin;
    skin.name = "__canonical_animation_only_skin";
    std::set<int> retained;
    for (std::size_t animation_index = 0; animation_index < scene.animations.size(); ++animation_index) {
        auto& tracks = scene.animations[animation_index].tracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const AnimationTrack& track) {
            std::vector<int> closure;
            std::set<int> seen;
            for (int node = track.target_node; node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size() &&
                 seen.insert(node).second; node = scene.nodes[static_cast<std::size_t>(node)].parent)
                closure.push_back(node);
            std::reverse(closure.begin(), closure.end());
            std::size_t additions = 0;
            for (const int node : closure) if (!retained.count(node)) ++additions;
            if (retained.size() + additions > palette_limit) {
                if (!retained.count(track.target_node) && retained.size() < palette_limit) {
                    retained.insert(track.target_node);
                    skin.joints.push_back(track.target_node);
                    note(scene, report, animation_index, "animation_ancestry_truncated",
                         "animation target retained without its complete ancestry to fit the native BONES limit");
                    return false;
                }
                note(scene, report, animation_index, "animation_track_palette_drop",
                     "animation track dropped because the synthesized animation palette is full");
                return true;
            }
            for (const int node : closure) if (retained.insert(node).second) skin.joints.push_back(node);
            return false;
        }), tracks.end());
    }
    synthesize_empty_clips(scene, report, skin.joints.empty() ? -1 : skin.joints.front());
    if (skin.joints.empty()) return;
    skin.skeleton_root = skin.joints.front();
    for (const int joint : skin.joints)
        skin.joint_names.push_back(scene.nodes[static_cast<std::size_t>(joint)].name);
    const int skin_index = static_cast<int>(scene.skins.size());
    scene.skins.push_back(std::move(skin));
    int attachment = scene.scene_roots.empty() ? scene.skins.back().joints.front() : scene.scene_roots.front();
    if (scene.scene_roots.empty()) scene.scene_roots.push_back(attachment);
    scene.nodes[static_cast<std::size_t>(attachment)].skin = skin_index;
    note(scene, report, 0, "animation_only_skin_synthesized",
         "unskinned animation-only nodes were lowered to a deterministic native BONES palette", false);
}

void retain_skin_targets(Scene& scene, AnimationCanonicalizationReport& report) {
    const auto active_skins = active_skin_indices(scene);
    if (active_skins.size() != 1) return;
    auto& skin = scene.skins[*active_skins.begin()];
    const std::size_t palette_limit = scene.primitives.empty() ? 4096u : 256u;

    auto append_joint = [&](int node_index) {
        if (std::find(skin.joints.begin(), skin.joints.end(), node_index) != skin.joints.end() ||
            node_index < 0 || static_cast<std::size_t>(node_index) >= scene.nodes.size() ||
            skin.joints.size() >= palette_limit) return false;
        Matrix4 stingray_inverse{};
        if (!inverse_matrix(scene.nodes[static_cast<std::size_t>(node_index)].world_stingray, stingray_inverse)) return false;
        Matrix4 gltf_inverse{};
        if (!skin.inverse_bind_matrices_gltf.empty() &&
            !inverse_matrix(scene.nodes[static_cast<std::size_t>(node_index)].world_gltf, gltf_inverse)) return false;
        skin.joints.push_back(node_index);
        skin.joint_names.push_back(scene.nodes[static_cast<std::size_t>(node_index)].name);
        if (!skin.inverse_bind_matrices_stingray.empty()) skin.inverse_bind_matrices_stingray.push_back(stingray_inverse);
        if (!skin.inverse_bind_matrices_gltf.empty()) skin.inverse_bind_matrices_gltf.push_back(gltf_inverse);
        return true;
    };

    // Animation-only scenes can carry a very large imported palette. Keep
    // only target joints and their root-to-leaf ancestor closure.
    if (scene.primitives.empty() && skin.joints.size() > palette_limit) {
        const auto original = skin;
        std::set<int> wanted;
        for (const auto& animation : scene.animations) for (const auto& track : animation.tracks) {
            int node = track.target_node;
            while (node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size()) {
                wanted.insert(node);
                node = scene.nodes[static_cast<std::size_t>(node)].parent;
            }
        }
        std::vector<std::size_t> keep;
        for (std::size_t i = 0; i < original.joints.size(); ++i)
            if (wanted.count(original.joints[i])) keep.push_back(i);
        if (keep.size() <= palette_limit) {
            skin.joints.clear(); skin.joint_names.clear();
            skin.inverse_bind_matrices_gltf.clear(); skin.inverse_bind_matrices_stingray.clear();
            for (const auto i : keep) {
                skin.joints.push_back(original.joints[i]);
                if (i < original.joint_names.size()) skin.joint_names.push_back(original.joint_names[i]);
                if (i < original.inverse_bind_matrices_gltf.size()) skin.inverse_bind_matrices_gltf.push_back(original.inverse_bind_matrices_gltf[i]);
                if (i < original.inverse_bind_matrices_stingray.size()) skin.inverse_bind_matrices_stingray.push_back(original.inverse_bind_matrices_stingray[i]);
            }
            note(scene, report, 0, "animation_skin_trimmed",
                 "animation-only skin palette was trimmed to target-relevant joints and ancestor closure", false);
        } else {
            // Closure itself is too large: retain authored track order as the
            // stable priority and drop the lowest-priority tracks that do not fit.
            skin = original;
            std::set<int> retained_targets;
            for (auto& animation : scene.animations) {
                animation.tracks.erase(std::remove_if(animation.tracks.begin(), animation.tracks.end(), [&](const AnimationTrack& track) {
                    std::set<int> closure;
                    int node = track.target_node;
                    while (node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size()) {
                        if (std::find(original.joints.begin(), original.joints.end(), node) != original.joints.end()) closure.insert(node);
                        node = scene.nodes[static_cast<std::size_t>(node)].parent;
                    }
                    std::set<int> combined = retained_targets;
                    combined.insert(closure.begin(), closure.end());
                    if (combined.size() > palette_limit) {
                        note(scene, report, &animation - scene.animations.data(), "animation_track_palette_drop",
                             "animation track dropped because target ancestor closure exceeds the native skin palette limit");
                        return true;
                    }
                    retained_targets.insert(closure.begin(), closure.end());
                    return false;
                }), animation.tracks.end());
            }
            std::vector<std::size_t> keep;
            for (std::size_t i = 0; i < original.joints.size() && keep.size() < palette_limit; ++i)
                if (retained_targets.count(original.joints[i])) keep.push_back(i);
            skin.joints.clear(); skin.joint_names.clear();
            skin.inverse_bind_matrices_gltf.clear(); skin.inverse_bind_matrices_stingray.clear();
            for (const auto i : keep) {
                skin.joints.push_back(original.joints[i]);
                if (i < original.joint_names.size()) skin.joint_names.push_back(original.joint_names[i]);
                if (i < original.inverse_bind_matrices_gltf.size()) skin.inverse_bind_matrices_gltf.push_back(original.inverse_bind_matrices_gltf[i]);
                if (i < original.inverse_bind_matrices_stingray.size()) skin.inverse_bind_matrices_stingray.push_back(original.inverse_bind_matrices_stingray[i]);
            }
        }
    }
    for (std::size_t animation_index = 0; animation_index < scene.animations.size(); ++animation_index) {
        auto& tracks = scene.animations[animation_index].tracks;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const AnimationTrack& track) {
            if (track.target_node >= 0 && static_cast<std::size_t>(track.target_node) < scene.nodes.size()) {
                // A channel that already targets an authored skin joint needs no
                // extra ancestor in the BONES palette. Its non-joint scene parent
                // remains represented by UNIT and must not become a new bone.
                if (std::find(skin.joints.begin(), skin.joints.end(), track.target_node) != skin.joints.end())
                    return false;
                std::vector<int> closure;
                for (int node = track.target_node; node >= 0 && static_cast<std::size_t>(node) < scene.nodes.size();
                     node = scene.nodes[static_cast<std::size_t>(node)].parent) closure.push_back(node);
                std::reverse(closure.begin(), closure.end());
                bool complete = true;
                for (const int node : closure) {
                    if (!append_joint(node)) {
                        complete = std::find(skin.joints.begin(), skin.joints.end(), node) != skin.joints.end();
                        if (!complete) break;
                    }
                }
                if (complete) return false;
            }
            note(scene, report, animation_index, "animation_track_static_drop",
                 "animation " + std::to_string(animation_index) + ": dropped target track that could not fit the retained skin palette");
            return true;
        }), tracks.end());
    }
    const int static_target = skin.joints.empty() ? -1 : skin.joints.front();
    synthesize_empty_clips(scene, report, static_target);
}

bool valid_source_data(const Scene& scene, std::string& error) {
    for (const auto& node : scene.nodes) {
        if (!finite_matrix(node.local_stingray) || !finite_matrix(node.world_stingray)) {
            error = "animation canonicalization encountered non-finite node transform data";
            return false;
        }
    }
    for (const auto& primitive : scene.primitives) {
        for (const auto& channel : primitive.channels) if (!finite_channel(channel)) {
            error = "animation canonicalization encountered malformed or non-finite vertex data";
            return false;
        }
        for (const auto& channel : primitive.source_channels) if (!finite_channel(channel)) {
            error = "animation canonicalization encountered malformed or non-finite source vertex data";
            return false;
        }
    }
    for (const auto& animation : scene.animations) for (const auto& track : animation.tracks) {
        const std::size_t components = track.path == AnimationPath::Rotation ? 4u :
            track.path == AnimationPath::Weights ? track.value_components : 3u;
        const std::size_t multiplier = track.interpolation == AnimationInterpolation::CubicSpline ? 3u : 1u;
        if (track.target_node < 0 || static_cast<std::size_t>(track.target_node) >= scene.nodes.size() ||
            track.value_components == 0 || track.times.empty() ||
            track.values.size() != track.times.size() * components * multiplier ||
            !std::all_of(track.times.begin(), track.times.end(), [](float value) { return std::isfinite(value); }) ||
            !std::all_of(track.values.begin(), track.values.end(), [](float value) { return std::isfinite(value); }) ||
            track.times.front() < 0.0f ||
            std::adjacent_find(track.times.begin(), track.times.end(),
                [](float previous, float current) { return previous >= current; }) != track.times.end()) {
            error = "animation canonicalization encountered malformed or non-finite animation data";
            return false;
        }
    }
    return true;
}

bool malformed_lowering_error(const std::string& error) {
    static constexpr const char* markers[] = {
        "invalid", "malformed", "inconsistent", "cycle", "outside the selected scene",
        "disagrees", "non-finite", "unsupported TRS sampler", "times must be", "contains invalid",
        "requires a finite POSITION channel"
    };
    return std::any_of(std::begin(markers), std::end(markers), [&](const char* marker) {
        return error.find(marker) != std::string::npos;
    });
}

void retain_world_baked_rest_pose(Scene& scene) {
    const Matrix4 identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (auto& node : scene.nodes) {
        node.local_stingray = identity;
        node.world_stingray = identity;
        node.local_gltf = identity;
        node.world_gltf = identity;
        node.local_translation_stingray = {0,0,0};
        node.local_rotation_stingray = {0,0,0,1};
        node.local_scale_stingray = {1,1,1};
        node.has_exact_trs = true;
    }
}

void note(Scene& scene, AnimationCanonicalizationReport& report, std::size_t index,
          const char* code, const std::string& message, bool lossy) {
    report.changed = true;
    report.lossy = report.lossy || lossy;
    report.approximations.emplace_back(code);
    report.diagnostics.push_back({index, code, message});
    scene.notes.push_back(message + " [" + code + "]");
}

} // namespace

bool canonicalize_animations(Scene& scene, AnimationCanonicalizationReport& report,
                             std::string& error) {
    report = {};
    error.clear();
    if (scene.animations.empty()) return true;
    if (!valid_source_data(scene, error)) return false;
    const bool fixed_morph_bake = scene.source_features.fixed_initial_morph_bake_succeeded &&
        scene.source_features.active_morph_target_count == scene.source_features.fixed_initial_morph_bake_count;
    for (std::size_t index = 0; index < scene.animations.size(); ++index) {
        auto& animation = scene.animations[index];
        canonicalize_rotation_keys(animation, scene, report, index);
        const auto before = animation.tracks.size();
        animation.tracks.erase(std::remove_if(animation.tracks.begin(), animation.tracks.end(),
            [](const AnimationTrack& track) { return track.path == AnimationPath::Weights; }), animation.tracks.end());
        if (animation.tracks.size() != before) {
            note(scene, report, index, "morph_weight_animation_dropped",
                 "animation " + std::to_string(index) + (fixed_morph_bake
                    ? ": dropped morph-weight tracks after fixed initial morph bake"
                    : ": retained the static/default morph state because native skeletal animation has no weight channel"));
        }
        canonicalize_clip(animation, scene, report, index);
    }
    synthesize_empty_clips(scene, report);

    select_animation_skin(scene, report);
    synthesize_animation_only_skin(scene, report);

    // The loader's static channels are already world-baked.  Only unskinned
    // geometry needs this automatic rigid-to-skeletal conversion.
    if (scene.animations.empty()) return true;
    if (scene.primitives.empty()) {
        retain_skin_targets(scene, report);
        const auto active = active_skin_indices(scene);
        const int target = active.size() == 1 && !scene.skins[*active.begin()].joints.empty()
            ? scene.skins[*active.begin()].joints.front() : -1;
        synthesize_empty_clips(scene, report, target);
        return true;
    }
    const bool has_skin = !active_skin_indices(scene).empty();
    if (has_skin) {
        retain_skin_targets(scene, report);
        const auto active = active_skin_indices(scene);
        const int target = active.size() == 1 && !scene.skins[*active.begin()].joints.empty()
            ? scene.skins[*active.begin()].joints.front() : -1;
        synthesize_empty_clips(scene, report, target);
        return true;
    }

    // Earlier universal-lowering stages may deliberately detach geometry that
    // cannot fit the single native skin profile. Its current channels are then
    // the authoritative static world/initial-pose representation.
    if (std::any_of(scene.primitives.begin(), scene.primitives.end(), [](const Primitive& primitive) {
            return primitive.source_node < 0;
        })) {
        scene.animations.clear();
        retain_world_baked_rest_pose(scene);
        note(scene, report, 0, "rigid_animation_rest_pose",
             "animation targets geometry canonicalized to a static initial pose; retained world-baked geometry");
        return true;
    }

    Scene lowered;
    std::string lowering_error;
    if (compiler::lower_rigid_animation(scene, lowered, lowering_error)) {
        scene = std::move(lowered);
        note(scene, report, 0, "rigid_animation_lowered",
             "unskinned animated geometry was lowered to the native skeletal animation profile", false);
        if (!scene.asset_definition.node_bodies.empty())
            note(scene, report, 0, "rigid_animation_physics_visual_only",
                 "object clips animate the rendered skin; authored physics bodies keep their own rest-pose or runtime-driven motion");
        return true;
    }

    if (malformed_lowering_error(lowering_error)) {
        error = lowering_error;
        return false;
    }

    // Source data was checked above, so this is a representational limitation
    // (for example sheared transforms or an oversized retained node closure),
    // not malformed loader input. Keep the already baked rest pose.
    scene.animations.clear();
    retain_world_baked_rest_pose(scene);
    note(scene, report, 0, "rigid_animation_rest_pose",
         "unskinned animation could not fit the native rigid profile; retained static rest/world-baked geometry: " + lowering_error);
    return true;
}

} // namespace dtglb::processing
