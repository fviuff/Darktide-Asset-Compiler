#include "compiler/compilation_plan.h"
#include "compiler/capability_registry.h"
#include "compiler/source_queries.h"
#include "compiler/material_builder.h"
#include "stingray/murmur_hash.h"
#include "stingray/animation/animation_writer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace dtglb::compiler {
using app::CompileOptions;
using app::ResolvedPhysicsInventory;
using app::OutputKind;
namespace {
void add_gap(std::vector<std::string>& gaps, bool condition, std::string message) {
    if (condition) gaps.push_back(std::move(message));
}

const VertexChannel* find_channel(const std::vector<VertexChannel>& channels,
                                  VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    const auto it = std::find_if(channels.begin(), channels.end(), [=](const VertexChannel& channel) {
        return channel.semantic == semantic && channel.set == set;
    });
    return it == channels.end() ? nullptr : &*it;
}

bool finite_channel_rows(const VertexChannel& channel, std::size_t rows, std::uint32_t minimum_components) {
    if (channel.components < minimum_components ||
        rows > channel.values.size() / static_cast<std::size_t>(channel.components)) return false;
    const auto end = channel.values.begin() + static_cast<std::ptrdiff_t>(
        rows * static_cast<std::size_t>(channel.components));
    return std::all_of(channel.values.begin(), end, [](float value) { return std::isfinite(value); });
}

bool packed_half_overflow(const VertexChannel& channel, std::size_t rows, std::uint32_t packed_components) {
    constexpr float kBinary16Max = 65504.0f;
    if (!finite_channel_rows(channel, rows, packed_components)) return false;
    for (std::size_t row = 0; row < rows; ++row) {
        const auto base = row * static_cast<std::size_t>(channel.components);
        for (std::uint32_t component = 0; component < packed_components; ++component) {
            if (std::abs(channel.values[base + component]) > kBinary16Max) return true;
        }
    }
    return false;
}



std::set<int> emitted_unit_node_indices(const Scene& scene, const std::vector<std::size_t>& used_skins) {
    auto nodes = active_node_indices(scene);
    for (const auto skin_index : used_skins) {
        for (const int joint : scene.skins[skin_index].joints) {
            int current = joint;
            std::set<int> ancestry;
            while (current >= 0 && static_cast<std::size_t>(current) < scene.nodes.size() &&
                   ancestry.insert(current).second) {
                nodes.insert(current);
                current = scene.nodes[static_cast<std::size_t>(current)].parent;
            }
        }
    }
    return nodes;
}

bool unit_transform_supported(const Matrix4& matrix) {
    if (!std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); })) return false;
    if (std::abs(matrix[3]) > 1e-6f || std::abs(matrix[7]) > 1e-6f ||
        std::abs(matrix[11]) > 1e-6f || std::abs(matrix[15] - 1.0f) > 1e-6f) return false;
    const auto length3 = [&](std::size_t offset) {
        return std::sqrt(matrix[offset] * matrix[offset] + matrix[offset + 1] * matrix[offset + 1] +
                         matrix[offset + 2] * matrix[offset + 2]);
    };
    const std::array<float,3> scales{{length3(0), length3(4), length3(8)}};
    if (scales[0] <= 1e-12f || scales[1] <= 1e-12f || scales[2] <= 1e-12f) return false;
    const auto normalized_dot = [&](std::size_t a, std::size_t b, float denominator) {
        return (matrix[a] * matrix[b] + matrix[a + 1] * matrix[b + 1] +
                matrix[a + 2] * matrix[b + 2]) / denominator;
    };
    return std::max({std::abs(normalized_dot(0, 4, scales[0] * scales[1])),
                     std::abs(normalized_dot(0, 8, scales[0] * scales[2])),
                     std::abs(normalized_dot(4, 8, scales[1] * scales[2]))}) <= 2e-5f;
}

bool animation_base_transform_supported(const NodeInfo& node) {
    if (node.has_exact_trs) {
        const bool finite = std::all_of(node.local_translation_stingray.begin(), node.local_translation_stingray.end(),
            [](float value) { return std::isfinite(value); }) &&
            std::all_of(node.local_rotation_stingray.begin(), node.local_rotation_stingray.end(),
            [](float value) { return std::isfinite(value); }) &&
            std::all_of(node.local_scale_stingray.begin(), node.local_scale_stingray.end(),
            [](float value) { return std::isfinite(value); });
        float quaternion_length2 = 0.0f;
        for (const float value : node.local_rotation_stingray) quaternion_length2 += value * value;
        return finite && std::isfinite(quaternion_length2) && quaternion_length2 >= 1e-20f;
    }
    const auto& matrix = node.local_stingray;
    if (!std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); })) return false;
    const auto length3 = [&](std::size_t offset) {
        return std::sqrt(matrix[offset] * matrix[offset] + matrix[offset + 1] * matrix[offset + 1] +
                         matrix[offset + 2] * matrix[offset + 2]);
    };
    const std::array<float,3> scales{{length3(0), length3(4), length3(8)}};
    if (scales[0] < 1e-20f || scales[1] < 1e-20f || scales[2] < 1e-20f) return false;
    const auto normalized_dot = [&](std::size_t a, std::size_t b, float denominator) {
        return (matrix[a] * matrix[b] + matrix[a + 1] * matrix[b + 1] +
                matrix[a + 2] * matrix[b + 2]) / denominator;
    };
    return std::max({std::abs(normalized_dot(0, 4, scales[0] * scales[1])),
                     std::abs(normalized_dot(0, 8, scales[0] * scales[2])),
                     std::abs(normalized_dot(4, 8, scales[1] * scales[2]))}) <= 1e-3f;
}


std::vector<std::string> current_compiler_gaps(const Scene& s, const CompileOptions& o,
                                               const ResolvedPhysicsInventory&,
                                               const std::vector<std::size_t>& animation_indices) {
    const auto& f = s.source_features;
    std::vector<std::string> gaps;
    const bool model = o.output_kind != OutputKind::Animations;
    const bool animations = o.output_kind != OutputKind::Model;
    auto animation_count = animation_indices.size();

    add_gap(gaps, model && f.decoded_source_primitive_count < f.active_source_primitive_count,
            "one or more active glTF primitives were not decoded by the current frontend");
    add_gap(gaps, model && f.active_morph_target_count != f.fixed_initial_morph_bake_count,
            "one or more active morph targets could not be baked at their fixed initial weights");
    add_gap(gaps, f.skin_count > s.skins.size(),
            "one or more source skins were not decoded into the intermediate scene");
    const auto used_skins = used_skin_indices(s);
    bool unsupported_skin_channel_set = false;
    bool active_custom_attribute = false;
    bool active_color_attribute = false;
    bool unsupported_texcoord_set = false;
    bool unsupported_skinned_texcoord_set = false;
    bool invalid_skin_channels = false;
    bool packed_half_range_exceeded = false;
    if (model) for (const auto& primitive : s.primitives) {
        const bool skinned = primitive.source_node >= 0 &&
            static_cast<std::size_t>(primitive.source_node) < s.nodes.size() &&
            s.nodes[static_cast<std::size_t>(primitive.source_node)].skin >= 0 &&
            static_cast<std::size_t>(s.nodes[static_cast<std::size_t>(primitive.source_node)].skin) < s.skins.size();
        const auto& channels = skinned && !primitive.source_channels.empty()
            ? primitive.source_channels : primitive.channels;
        const auto* position = find_channel(primitive.channels, VertexChannel::Semantic::Position);
        const std::size_t vertices = position && position->components
            ? position->values.size() / position->components : 0;
        for (const auto& channel : channels) {
            if ((channel.semantic == VertexChannel::Semantic::BlendIndices ||
                 channel.semantic == VertexChannel::Semantic::BlendWeights) && channel.set > 1u)
                unsupported_skin_channel_set = true;
            if (channel.semantic == VertexChannel::Semantic::Color) active_color_attribute = true;
            if (channel.semantic == VertexChannel::Semantic::Texcoord) {
                if (channel.set > 6u) unsupported_texcoord_set = true;
                if (skinned && channel.set > 1u) unsupported_skinned_texcoord_set = true;
                packed_half_range_exceeded = packed_half_range_exceeded ||
                    packed_half_overflow(channel, vertices, 2);
            }
        }
        const auto* packed_position = find_channel(channels, VertexChannel::Semantic::Position);
        if (packed_position)
            packed_half_range_exceeded = packed_half_range_exceeded ||
                packed_half_overflow(*packed_position, vertices, 3);
        if (skinned) {
            for (std::uint32_t set = 0; set < 2; ++set) {
                const auto* joints = find_channel(channels, VertexChannel::Semantic::BlendIndices, set);
                const auto* weights = find_channel(channels, VertexChannel::Semantic::BlendWeights, set);
                if (set == 0 && (!joints || !weights)) invalid_skin_channels = true;
                if (!joints && !weights) continue;
                if (!joints || !weights || !finite_channel_rows(*joints, vertices, 4) ||
                    !finite_channel_rows(*weights, vertices, 4)) {
                    invalid_skin_channels = true;
                    continue;
                }
                if (std::any_of(joints->values.begin(), joints->values.end(), [](float value) {
                        return value < 0.0f || value > 255.0f || std::floor(value) != value;
                    }) || std::any_of(weights->values.begin(), weights->values.end(), [](float value) {
                        return value < 0.0f || value > 1.0f;
                    })) invalid_skin_channels = true;
            }
            const auto skin_index = static_cast<std::size_t>(
                s.nodes[static_cast<std::size_t>(primitive.source_node)].skin);
            const auto palette_size = s.skins[skin_index].joints.size();
            for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
                bool has_positive_influence = false;
                for (std::uint32_t set = 0; set < 2; ++set) {
                    const auto* joints = find_channel(channels, VertexChannel::Semantic::BlendIndices, set);
                    const auto* weights = find_channel(channels, VertexChannel::Semantic::BlendWeights, set);
                    if (!joints || !weights || !finite_channel_rows(*joints, vertices, 4) ||
                        !finite_channel_rows(*weights, vertices, 4)) continue;
                    const auto joint_base = vertex * static_cast<std::size_t>(joints->components);
                    const auto weight_base = vertex * static_cast<std::size_t>(weights->components);
                    for (std::uint32_t component = 0; component < 4; ++component) {
                        if (weights->values[weight_base + component] <= 0.0f) continue;
                        has_positive_influence = true;
                        const float joint = joints->values[joint_base + component];
                        if (joint < 0.0f || joint >= static_cast<float>(palette_size) ||
                            std::floor(joint) != joint) invalid_skin_channels = true;
                    }
                }
                if (!has_positive_influence) invalid_skin_channels = true;
            }
        }
        active_custom_attribute = active_custom_attribute || !primitive.custom_attributes.empty();
    }
    add_gap(gaps, unsupported_skin_channel_set,
            "more than one JOINTS/WEIGHTS set is not supported");
    add_gap(gaps, unsupported_skinned_texcoord_set,
            "skinned meshes support at most two TEXCOORD sets");
    add_gap(gaps, invalid_skin_channels,
            "skin JOINTS/WEIGHTS must fit four 8-bit joint indices with normalized weights");
    add_gap(gaps, packed_half_range_exceeded,
            "POSITION/TEXCOORD values exceed the half-float range the game stores them in");
    bool invalid_used_skin_palette = false;
    if (model) for (const auto skin_index : used_skins) {
        const auto& skin = s.skins[skin_index];
        const auto& matrices = skin.inverse_bind_matrices_stingray.empty()
            ? skin.inverse_bind_matrices_gltf : skin.inverse_bind_matrices_stingray;
        std::set<int> unique_joints;
        const bool invalid_joint_nodes = std::any_of(skin.joints.begin(), skin.joints.end(), [&](int joint) {
            return joint < 0 || static_cast<std::size_t>(joint) >= s.nodes.size() || !unique_joints.insert(joint).second;
        });
        if (skin.joints.empty() || skin.joints.size() > 256u || invalid_joint_nodes ||
            matrices.size() != skin.joints.size() ||
            !std::all_of(matrices.begin(), matrices.end(), [](const Matrix4& matrix) {
                return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
            })) invalid_used_skin_palette = true;
    }
    add_gap(gaps, invalid_used_skin_palette,
            "active SkinDT skin requires 1..256 finite inverse-bind matrices matching its joints");
    add_gap(gaps, active_custom_attribute,
            "custom vertex attributes are not supported");
    bool custom_gpu_instance_attribute = false;
    const auto active_nodes = active_node_indices(s);
    for (const int node_index : active_nodes) {
        const auto& node = s.nodes[static_cast<std::size_t>(node_index)];
        for (const auto& attribute : node.instance_attributes) {
            if (!attribute.name.empty() && attribute.name.front() == '_') custom_gpu_instance_attribute = true;
        }
    }
    add_gap(gaps, model && custom_gpu_instance_attribute,
            "custom EXT_mesh_gpu_instancing attributes are not supported");
    if (model && !s.primitives.empty()) {
        bool invalid_node_identity = false;
        if (f.node_count > 1 || f.parent_edge_count != 0 || !used_skins.empty()) {
            const auto unit_nodes = emitted_unit_node_indices(s, used_skins);
            bool invalid_node_transform = false;
            for (const int node_index : unit_nodes) {
                const auto& node = s.nodes[static_cast<std::size_t>(node_index)];
                if (node.name.find('\0') != std::string::npos)
                    invalid_node_identity = true;
                if (!unit_transform_supported(node.local_stingray)) invalid_node_transform = true;
            }
            add_gap(gaps, invalid_node_transform,
                    "active UNIT scene node transform is non-affine, singular, or contains unsupported shear");
        }
        add_gap(gaps, invalid_node_identity,
                "active UNIT authored node names must be NUL-free");
    }
    add_gap(gaps, used_skins.size() > 1,
            "multiple skins are referenced by active primitives but the current resource closure supports one skeleton");
    const auto active_node_skins = active_node_skin_indices(s);
    add_gap(gaps, animations && animation_count != 0 && used_skins.empty() && active_node_skins.size() > 1,
            "multiple skins are referenced by active animation-only nodes but the current resource closure supports one skeleton");
    const bool animation_only_skin = animation_count != 0 && used_skins.empty() && active_node_skins.size() == 1;
    if (animations) for (const auto index : animation_indices) {
        const auto& animation = s.animations[index];
        add_gap(gaps, animation.tracks.empty() && animation.dropped_pointer_channel_count == 0,
                "animation has no decoded channels that can be lowered");
    }
    const std::size_t animation_skin = used_skins.size() == 1 ? used_skins.front() :
        (animation_only_skin ? active_node_skins.front() : static_cast<std::size_t>(-1));
    if (animation_skin != static_cast<std::size_t>(-1)) {
        const auto& skin = s.skins[animation_skin];
        std::set<int> unique_joints;
        bool invalid_joint_nodes = skin.joints.empty();
        for (const int joint : skin.joints) {
            if (joint < 0 || static_cast<std::size_t>(joint) >= s.nodes.size() || !unique_joints.insert(joint).second)
                invalid_joint_nodes = true;
        }
        add_gap(gaps, invalid_joint_nodes,
                "selected skin requires distinct in-range joint nodes for coherent UNIT/BONES/ANIMATION identity");
        std::set<std::uint32_t> bone_hashes;
        bool colliding_bone_hash = false;
        for (const auto& name : canonical_bone_names(skin)) {
            if (!bone_hashes.insert(stingray::id32_from_id64(name)).second) colliding_bone_hash = true;
        }
        add_gap(gaps, colliding_bone_hash,
                "selected skin bone names collide in the native BONES hash domain");
    }
    if (animations && animation_count != 0 && animation_skin != static_cast<std::size_t>(-1)) {
        add_gap(gaps, s.skins[animation_skin].joints.size() > 4096u,
                "the animated skin has more than 4096 bones");
        std::set<int> selected_joints(s.skins[animation_skin].joints.begin(), s.skins[animation_skin].joints.end());
        if (animations) for (const auto index : animation_indices) {
            const auto& animation = s.animations[index];
            std::set<std::pair<int,AnimationPath>> animation_channels;
            for (const auto& track : animation.tracks) {
                add_gap(gaps, track.path == AnimationPath::Weights,
                        "animation weights channels cannot be represented by the current skeletal ANIMATION writer");
                add_gap(gaps, !selected_joints.count(track.target_node),
                        "animation channel targets a node outside the selected skin");
                add_gap(gaps, !animation_channels.insert({track.target_node, track.path}).second,
                        "animation contains duplicate channels for one bone/path");
            }
        }
        bool invalid_animation_base = false;
        for (const int joint : s.skins[animation_skin].joints) {
            if (joint < 0 || static_cast<std::size_t>(joint) >= s.nodes.size() ||
                !animation_base_transform_supported(s.nodes[static_cast<std::size_t>(joint)]))
                invalid_animation_base = true;
        }
        add_gap(gaps, invalid_animation_base,
                "the animated skin has a joint transform that is not a finite translation/rotation/scale");
    }
    add_gap(gaps, active_color_attribute,
            "vertex colors (COLOR_n) are not supported on this mesh");
    add_gap(gaps, unsupported_texcoord_set,
            "TEXCOORD sets above 6 are not supported");
    if (model && o.auto_materials && o.material_override.empty()) {
        for (const auto material_index : used_material_indices(s)) {
            const auto gap = core_material_gap(s, s.materials[material_index]);
            if (!gap.empty()) gaps.push_back("material " + s.materials[material_index].name + ": " + gap);
        }
    }

    if (o.output_kind == OutputKind::Animations && animation_count == 0) {
        gaps.push_back("input has no active runtime content that the current compiler can emit");
    }

    return gaps;
}

} // namespace

CompilationPlan plan_compilation(const Scene& scene, const CompileOptions& options,
                                 const ResolvedPhysicsInventory& physics) {
    CompilationPlan plan;
    if (options.output_kind != OutputKind::All && options.output_kind != OutputKind::Model &&
        options.output_kind != OutputKind::Animations) {
        plan.gaps.push_back("invalid requested output kind");
        return plan;
    }
    if (options.animation_index && options.output_kind == OutputKind::Model) {
        plan.gaps.push_back("explicit clip selection conflicts with model-only output");
        return plan;
    }
    if (options.output_kind != OutputKind::Model) {
        for (std::size_t i = 0; i < scene.animations.size(); ++i) {
            if (!options.animation_index || scene.animations[i].source_index == *options.animation_index)
                plan.animation_indices.push_back(i);
        }
        if (options.animation_index && (*options.animation_index < 0 || plan.animation_indices.size() != 1)) {
            plan.gaps.push_back("requested clip index is not a decoded active animation in the selected scene");
            return plan;
        }
    }
    plan.emit_animations = options.output_kind != OutputKind::Model && !plan.animation_indices.empty();
    // a scene holding only particle effects ships just those
    plan.placeholder_unit = options.output_kind != OutputKind::Animations && scene.primitives.empty() &&
        scene.particle_effects.empty() &&
        (options.output_kind == OutputKind::Model || !plan.emit_animations);
    plan.emit_unit = options.output_kind != OutputKind::Animations &&
        (!scene.primitives.empty() || plan.placeholder_unit);
    plan.emit_materials = plan.emit_unit && !plan.placeholder_unit &&
        options.auto_materials && options.material_override.empty();
    const auto used = used_skin_indices(scene);
    const auto active = active_node_skin_indices(scene);
    if (!plan.placeholder_unit && plan.emit_unit && used.size() == 1) plan.skin_index = used.front();
    else if (plan.emit_animations && used.size() == 1) plan.skin_index = used.front();
    else if (used.empty() && active.size() == 1 && plan.emit_animations)
        plan.skin_index = active.front();
    plan.gaps = current_compiler_gaps(scene, options, physics, plan.animation_indices);
    const bool has_direct_state_machine = !options.state_machine_states.empty() ||
        !options.state_machine_variables.empty() ||
        !options.state_machine_transitions.empty();
    if (has_direct_state_machine && (options.loop_clip_index || options.once_clip_index)) {
        plan.gaps.push_back("--sm-state/--sm-transition cannot be combined with --loop-clip or --once-clip");
    }
    if (options.loop_clip_index && options.once_clip_index) {
        plan.gaps.push_back("--loop-clip and --once-clip are mutually exclusive");
    }
    const auto selected_state_machine_clip = options.loop_clip_index
        ? options.loop_clip_index : options.once_clip_index;
    plan.state_machine_looping = !options.once_clip_index.has_value();
    if (selected_state_machine_clip) {
        if (options.output_kind != OutputKind::All)
            plan.gaps.push_back("state-machine clip selection requires --output-kind all");
        if (!plan.emit_unit || plan.placeholder_unit)
            plan.gaps.push_back("state-machine clip selection requires a generated UNIT");
        if (!plan.skin_index)
            plan.gaps.push_back("state-machine clip selection requires one selected skin");
        for (const auto index : plan.animation_indices) {
            if (scene.animations[index].source_index == *selected_state_machine_clip) {
                plan.state_machine_animation_index = index;
                break;
            }
        }
        if (!plan.state_machine_animation_index)
            plan.gaps.push_back("state-machine clip index must identify an emitted clip");
    }
    if (has_direct_state_machine) {
        if (options.output_kind != OutputKind::All)
            plan.gaps.push_back("state-machine definitions require --output-kind all");
        if (!plan.emit_unit || plan.placeholder_unit)
            plan.gaps.push_back("state-machine definitions require a generated UNIT");
        if (!plan.skin_index)
            plan.gaps.push_back("state-machine definitions require one selected skin");
        if (options.state_machine_states.empty() &&
            (!options.state_machine_variables.empty() || !options.state_machine_transitions.empty()))
            plan.gaps.push_back("state-machine variables and transitions require at least one --sm-state");
        std::set<std::string> state_names;
        for (const auto& authored : options.state_machine_states) {
            if (authored.name.empty() || !state_names.insert(authored.name).second) {
                plan.gaps.push_back("state-machine state names must be unique and nonempty");
                continue;
            }
            auto animation = std::find_if(plan.animation_indices.begin(), plan.animation_indices.end(),
                [&](std::size_t index) {
                    return scene.animations[index].source_index == authored.clip_index;
                });
            if (animation == plan.animation_indices.end()) {
                plan.gaps.push_back("state-machine state clip index must identify an emitted clip");
                continue;
            }
            plan.state_machine_states.push_back({authored.name, *animation, authored.looping});
        }
        std::set<std::string> variable_names;
        for (const auto& variable : options.state_machine_variables) {
            if (variable.name.empty() || !variable_names.insert(variable.name).second)
                plan.gaps.push_back("state-machine variable names must be unique and nonempty");
            if (!std::isfinite(variable.initial_value) || !std::isfinite(variable.minimum) ||
                !std::isfinite(variable.maximum) || variable.minimum > variable.maximum ||
                variable.initial_value < variable.minimum || variable.initial_value > variable.maximum)
                plan.gaps.push_back("state-machine variable initial value must be within finite ordered bounds");
            plan.state_machine_variables.push_back({variable.name, variable.initial_value,
                variable.minimum, variable.maximum});
        }
        using EventKey = std::pair<std::size_t, std::string>;
        std::map<EventKey, std::optional<std::size_t>> event_kinds;
        std::map<EventKey, std::size_t> selector_indices;
        for (const auto& transition : options.state_machine_transitions) {
            if (transition.from_state >= options.state_machine_states.size() ||
                transition.to_state >= options.state_machine_states.size())
                plan.gaps.push_back("state-machine transition endpoint is outside the authored state list");
            if (transition.event_name.empty())
                plan.gaps.push_back("state-machine transition event name must be nonempty");
            if (!std::isfinite(transition.blend_seconds) || transition.blend_seconds < 0.0f)
                plan.gaps.push_back("state-machine transition blend time must be finite and nonnegative");
            const EventKey event_key{transition.from_state, transition.event_name};
            const auto [kind, inserted] = event_kinds.emplace(event_key, transition.variable_index);
            if (!inserted && kind->second != transition.variable_index)
                plan.gaps.push_back("one state/event binding cannot mix direct and different variable selectors");
            if (!inserted && !transition.variable_index)
                plan.gaps.push_back("one state/event binding has more than one direct transition");
            if (transition.variable_index) {
                if (*transition.variable_index >= plan.state_machine_variables.size())
                    plan.gaps.push_back("state-machine range transition refers to a missing variable");
                if (!std::isfinite(transition.lower) || !std::isfinite(transition.upper) ||
                    transition.lower > transition.upper ||
                    (transition.lower == transition.upper &&
                     (transition.lower_exclusive || transition.upper_exclusive)))
                    plan.gaps.push_back("state-machine range transition needs a nonempty finite interval");
                const auto [selector, new_selector] = selector_indices.emplace(event_key,
                    plan.state_machine_selectors.size());
                if (new_selector)
                    plan.state_machine_selectors.push_back({transition.from_state,
                        transition.event_name, *transition.variable_index, {}});
                if (plan.state_machine_selectors[selector->second].variable_index !=
                    *transition.variable_index)
                    plan.gaps.push_back("one state/event selector must use one variable");
                plan.state_machine_selectors[selector->second].cases.push_back({
                    plan.state_machine_transitions.size(), transition.lower, transition.upper,
                    transition.lower_exclusive, transition.upper_exclusive});
            }
            plan.state_machine_transitions.push_back({transition.from_state, transition.to_state,
                transition.event_name, transition.blend_seconds});
        }
    }
    for (const auto& decision : analyze_gltf_extension_capabilities(scene)) {
        const bool unsupported_required = decision.required &&
            (decision.status == CapabilityStatus::UnknownFormat ||
             decision.status == CapabilityStatus::KnownFormatNotImplemented);
        const bool active_lod = decision.feature == "MSFT_lod" &&
            decision.status == CapabilityStatus::KnownFormatNotImplemented;
        if (unsupported_required || active_lod) {
            plan.gaps.push_back("glTF extension '" + decision.feature +
                "' has active or required semantics that the compiler does not implement");
        }
    }
    if (!options.ragdoll_event.empty()) {
        const bool has_dynamic_body = std::any_of(scene.asset_definition.node_bodies.begin(),
            scene.asset_definition.node_bodies.end(), [](const BodyDefinition& body) { return body.actor == "dynamic"; });
        if (!has_dynamic_body || !options.physics)
            plan.gaps.push_back("--ragdoll-event needs dynamic node bodies (bodies on the rig's bones)");
        plan.ragdoll_event = options.ragdoll_event;
        if (plan.state_machine_animation_index) {
            // A single-clip machine becomes a named state so the ragdoll state can join it.
            plan.state_machine_states.push_back({"default", *plan.state_machine_animation_index, plan.state_machine_looping});
            plan.state_machine_animation_index.reset();
        }
    }
    if (!scene.asset_definition.dangles.empty() || !plan.ragdoll_event.empty()) {
        if (!plan.emit_unit || !plan.skin_index)
            plan.gaps.push_back("dangling bones and ragdolls need a unit with a skinned mesh");
        else if (!plan.state_machine_animation_index && plan.state_machine_states.empty())
            plan.rest_state = true;
    }
    const bool emits_state_machine = plan.state_machine_animation_index || !plan.state_machine_states.empty() ||
        plan.rest_state;
    if (options.simple_clip_index && !options.simple_animation)
        plan.gaps.push_back("--simple-clip cannot be combined with --no-simple-animation");
    if (options.simple_clip_index && emits_state_machine)
        plan.gaps.push_back("--simple-clip cannot be combined with a state machine");
    if (options.simple_clip_index && (!plan.emit_unit || !plan.emit_animations))
        plan.gaps.push_back("--simple-clip requires --output-kind all");
    if (options.simple_animation && !emits_state_machine && plan.emit_unit && plan.emit_animations &&
            plan.skin_index && !plan.animation_indices.empty()) {
        if (options.simple_clip_index) {
            for (const auto index : plan.animation_indices) {
                if (scene.animations[index].source_index == *options.simple_clip_index) {
                    plan.simple_animation_index = index;
                    break;
                }
            }
            if (!plan.simple_animation_index)
                plan.gaps.push_back("--simple-clip index must identify an emitted clip");
        } else {
            plan.simple_animation_index = plan.animation_indices.front();
        }
    }
    if (!scene.source_features.approximation_locations.empty())
        plan.approximations.push_back("frontend_source_approximation");
    if (plan.emit_unit && scene.source_features.active_morph_target_count != 0 &&
            scene.source_features.active_morph_target_count == scene.source_features.fixed_initial_morph_bake_count)
        plan.approximations.push_back("fixed_morph_bake");
    if (plan.emit_unit && !options.material_override.empty())
        plan.approximations.push_back("material_override");
    if (plan.placeholder_unit)
        plan.approximations.push_back("empty_scene_placeholder_geometry");
    if (plan.emit_unit && options.physics && physics.effective != 0 && !options.fitted_physics && !scene.asset_definition.body && scene.asset_definition.node_bodies.empty())
        plan.approximations.push_back("physics_omitted");
    for (const auto& decision : analyze_gltf_extension_capabilities(scene)) {
        if (decision.status != CapabilityStatus::UnknownFormat || decision.required) continue;
        plan.approximations.push_back("ignored_optional_extension:" + decision.feature);
    }
    return plan;
}
} // namespace dtglb::compiler
