#include "compiler/resource_builder.h"
#include "compiler/material_builder.h"
#include "compiler/particles_builder.h"
#include "compiler/source_queries.h"
#include "stingray/unit/unit_v115.h"
#include "stingray/bones/bones_writer.h"
#include "stingray/murmur_hash.h"
#include <algorithm>
#include <optional>
#include <cmath>
#include <set>

namespace dtglb::compiler {
namespace {
bool remap_joint_channels(std::vector<VertexChannel>& channels,
                          const std::vector<std::uint32_t>& source_to_reference,
                          std::string& error) {
    for (auto& channel : channels) {
        if (channel.semantic != VertexChannel::Semantic::BlendIndices) continue;
        for (float& value : channel.values) {
            if (!std::isfinite(value) || value < 0.0f ||
                value >= static_cast<float>(source_to_reference.size()) || std::floor(value) != value) {
                error = "reference BONES remap found a vertex joint index outside the source skin palette";
                return false;
            }
            value = static_cast<float>(source_to_reference[static_cast<std::size_t>(value)]);
        }
    }
    return true;
}

bool apply_reference_bones(const Scene& source, std::size_t skin_index,
                           const app::CompileOptions& options, Scene& output,
                           ResourceKey& reference_key, bool& subset, std::string& error) {
    subset = false;
    if (!options.reference_bones_file || !options.skeleton_resource) {
        error = "reference BONES requires both a file and explicit skeleton resource identity";
        return false;
    }
    reference_key = {"bones", *options.skeleton_resource};
    if (!reference_key.valid()) { error = "invalid --skeleton-resource identity"; return false; }
    stingray::bones::BonesResource reference;
    if (!stingray::bones::load_cooked_bones(*options.reference_bones_file,
                                            reference_key.name, reference, error)) return false;
    if (skin_index >= source.skins.size()) { error = "reference BONES selected skin index is out of range"; return false; }
    output = source;
    auto& skin = output.skins[skin_index];
    const auto& source_skin = source.skins[skin_index];
    const auto source_names = canonical_bone_names(source_skin);
    if (source_skin.joint_names.size() != source_skin.joints.size() || source_names != source_skin.joint_names) {
        error = "reference BONES requires complete, unique source joint names that are already valid native bone identities";
        return false;
    }
    std::vector<std::uint32_t> source_to_reference;
    std::vector<std::string> target_names = reference.names;
    std::size_t matched = 0;
    for (const auto& name : source_names)
        if (std::find(reference.names.begin(), reference.names.end(), name) != reference.names.end()) ++matched;
    if (!source_names.empty() && matched == 0) {
        error = "no source skin joint matches reference BONES (" + std::to_string(reference.names.size()) +
            " names, e.g. " + reference.names.front() + "); fit the rig to the skeleton or pick another one";
        return false;
    }
    if (!source_names.empty() && (matched < reference.names.size() || matched < source_names.size())) {
        // same as how the game does gloves and such, they all ship their own tiny bones file
        // Retail-gear style: an owned BONES with the matched joints in reference order,
        // followed by the rig's own extra joints (cloth, jiggle, holders) in source order.
        // Matched joints follow the parent's same-named nodes when linked; extras ride along.
        subset = true;
        std::vector<std::uint32_t> matched_reference;
        for (const auto& name : source_names) {
            const auto found = std::find(reference.names.begin(), reference.names.end(), name);
            if (found != reference.names.end())
                matched_reference.push_back(static_cast<std::uint32_t>(found - reference.names.begin()));
        }
        std::vector<std::uint32_t> sorted = matched_reference;
        std::sort(sorted.begin(), sorted.end());
        target_names.clear();
        for (const auto index : sorted) target_names.push_back(reference.names[index]);
        std::uint32_t next_extra = static_cast<std::uint32_t>(sorted.size());
        for (const auto& name : source_names) {
            const auto found = std::find(reference.names.begin(), reference.names.end(), name);
            if (found == reference.names.end()) {
                target_names.push_back(name);
                source_to_reference.push_back(next_extra++);
            } else {
                const auto index = static_cast<std::uint32_t>(found - reference.names.begin());
                source_to_reference.push_back(static_cast<std::uint32_t>(
                    std::lower_bound(sorted.begin(), sorted.end(), index) - sorted.begin()));
            }
        }
    } else if (!stingray::bones::map_bone_names(source_names, reference.names, source_to_reference, error)) {
        return false;
    }

    const auto reorder = [&](const auto& values, auto& mapped) {
        if (values.empty()) { mapped.clear(); return true; }
        if (values.size() != source_to_reference.size()) return false;
        mapped.resize(values.size());
        for (std::size_t old = 0; old < values.size(); ++old) mapped[source_to_reference[old]] = values[old];
        return true;
    };
    std::vector<int> joints;
    std::vector<Matrix4> inverse_gltf, inverse_stingray;
    if (!reorder(skin.joints, joints) ||
        !reorder(skin.inverse_bind_matrices_gltf, inverse_gltf) ||
        !reorder(skin.inverse_bind_matrices_stingray, inverse_stingray)) {
        error = "reference BONES remap requires inverse-bind arrays to match the complete source skin palette";
        return false;
    }
    skin.joints = std::move(joints);
    skin.joint_names = target_names;
    if (canonical_bone_names(skin) != target_names) {
        error = "reference BONES names are not representable as exact UNIT joint identities";
        return false;
    }
    skin.inverse_bind_matrices_gltf = std::move(inverse_gltf);
    skin.inverse_bind_matrices_stingray = std::move(inverse_stingray);
    for (auto& primitive : output.primitives) {
        if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= output.nodes.size() ||
            output.nodes[static_cast<std::size_t>(primitive.source_node)].skin != static_cast<int>(skin_index)) continue;
        if (!remap_joint_channels(primitive.source_channels, source_to_reference, error) ||
            !remap_joint_channels(primitive.channels, source_to_reference, error)) return false;
    }
    return true;
}

Scene with_placeholder_geometry(const Scene& source) {
    Scene lowered = source;
    Primitive primitive;
    primitive.name = "__empty_scene_placeholder__";
    primitive.material_name = "__no_material__";
    primitive.source_node = -1;
    primitive.mode = 4;
    primitive.indices = {0, 1, 2};
    primitive.channels = {
        {VertexChannel::Semantic::Position, 0, 3, {0,0,0, 0,0,0, 0,0,0}},
        {VertexChannel::Semantic::Normal, 0, 3, {0,0,1, 0,0,1, 0,0,1}},
        {VertexChannel::Semantic::Tangent, 0, 4, {1,0,0,1, 1,0,0,1, 1,0,0,1}},
        {VertexChannel::Semantic::Texcoord, 0, 2, {0,0, 0,0, 0,0}},
    };
    primitive.source_channels = primitive.channels;
    lowered.primitives.push_back(std::move(primitive));
    return lowered;
}

void set_feature_status(std::vector<std::string>& statuses, const std::string& feature,
                        const std::string& value) {
    for (std::size_t i = 0; i + 1 < statuses.size(); i += 2) {
        if (statuses[i] == feature) {
            statuses[i + 1] = value;
            return;
        }
    }
}
} // namespace

bool build_glb_resources(const Scene& source, const app::CompileOptions& options,
                         const CompilationContext& context, const CompilationPlan& plan, ResourceGraph& graph,
                         std::vector<std::string>& feature_status, std::string& error) {
    graph = {};
    if (!plan.gaps.empty()) { error = "cannot build resources from an unsupported compilation plan"; return false; }
    const auto& base = context.file_base();
    const auto no_skin = static_cast<std::size_t>(-1);
    const auto selected = plan.skin_index.value_or(no_skin);
    Scene reference_scene;
    bool reference_subset = false;
    const Scene* emitted_source = &source;
    const auto generated_bones = context.generated_key("bones", base + "_bones");
    ResourceKey bones = generated_bones;
    if (options.reference_bones_file || options.skeleton_resource) {
        if (selected == no_skin) { error = "reference BONES requires a compilation plan with one selected skin"; return false; }
        if (!apply_reference_bones(source, selected, options, reference_scene, bones, reference_subset, error)) return false;
        emitted_source = &reference_scene;
        if (reference_subset) bones = generated_bones;
        else graph.external.push_back({bones, true});
    }
    if (selected != no_skin) {
        if (!options.reference_bones_file || reference_subset) graph.owned.push_back({bones, base + "_bones.bones",
            stingray::bones::BonesResource{bones.name, canonical_bone_names(emitted_source->skins[selected]), {}}, {}});
        set_feature_status(feature_status, "bones", reference_subset ? "emitted_reference_subset" :
            options.reference_bones_file ? "external_reference" : "emitted");
    }
    if (plan.emit_unit) {
        const Scene placeholder = plan.placeholder_unit ? with_placeholder_geometry(*emitted_source) : Scene{};
        const Scene& unit_scene = plan.placeholder_unit ? placeholder : *emitted_source;
        const auto unit_key = context.generated_key("unit", base);
        stingray::unit::WriteOptions unit_options;
        unit_options.resource_name = unit_key.name;
        if (plan.state_machine_animation_index || !plan.state_machine_states.empty() || plan.rest_state)
            unit_options.animation_state_machine_resource = unit_key.name;
        unit_options.material_override = options.material_override;
        unit_options.authored_physics = options.physics;
        unit_options.ragdoll_handoff = !plan.ragdoll_event.empty();
        if (options.physics && options.fitted_physics)
            unit_options.fitted_physics = options.fitted_physics;
        std::vector<int> material_indices;
        material_indices.reserve(unit_scene.primitives.size());
        for (const auto& primitive : unit_scene.primitives) material_indices.push_back(primitive.material);
        std::vector<std::string> material_names;
        material_names.reserve(source.materials.size());
        for (const auto& material : source.materials) material_names.push_back(material.name);
        const auto material_slots = stingray::unit::lower_material_slots(material_names, material_indices);
        unit_options.primitive_material_slots = material_slots.primitive_slots;
        // The selected skin is also used for animation-only scenes.  UNIT's
        // skeleton field, however, is cardinality-coupled to the skins that
        // its own primitives reference; an animation-only synthetic skin must
        // not make a static/placeholder UNIT claim a BONES resource.
        const bool unit_uses_selected_skin = selected != no_skin && std::any_of(
            unit_scene.primitives.begin(), unit_scene.primitives.end(), [&](const Primitive& primitive) {
                return primitive.source_node >= 0 &&
                    static_cast<std::size_t>(primitive.source_node) < unit_scene.nodes.size() &&
                    unit_scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin ==
                        static_cast<int>(selected);
            });
        if (unit_uses_selected_skin) unit_options.skeleton_resource = bones.name;
        if (plan.simple_animation_index && unit_uses_selected_skin && !plan.placeholder_unit) {
            stingray::animation::BuiltAnimation simple;
            if (!stingray::animation::build_skeletal_animation(*emitted_source, emitted_source->skins[selected],
                    emitted_source->animations[*plan.simple_animation_index], unit_key.name, simple, error,
                    &options.animation_fit)) return false;
            constexpr std::size_t kEnvelopeSize = 0x26;
            if (simple.cooked.size() <= kEnvelopeSize) { error = "simple animation produced no ANIMATION body"; return false; }
            unit_options.simple_animation.assign(simple.cooked.begin() + kEnvelopeSize, simple.cooked.end());
            unit_options.simple_animation_skin = selected;
            set_feature_status(feature_status, "simple_animation", "emitted");
        }
        // The command-line override intentionally replaces every per-slot
        // material choice, including authored external and generated intents.
        if (plan.emit_materials && options.material_override.empty()) {
            if (!build_core_materials(source, context, graph, unit_options.material_bindings, error, &material_slots)) return false;
            if (!unit_options.material_bindings.empty()) {
                const auto used = used_material_indices(source);
                const bool has_external = std::any_of(used.begin(), used.end(), [&](std::size_t index) {
                    return source.materials[index].intent == MaterialInfo::Intent::External;
                });
                set_feature_status(feature_status, "materials", has_external ? "emitted_per_slot_materials" : "emitted_core_pbr");
            }
        }
        std::set<ResourceKey> dependencies;
        if (unit_uses_selected_skin) dependencies.insert(bones);
        if (plan.state_machine_animation_index || !plan.state_machine_states.empty() || plan.rest_state)
            dependencies.insert({"state_machine", unit_key.name});
        for (const auto& binding : unit_options.material_bindings) dependencies.insert({"material", binding.second});
        if (!options.material_override.empty()) {
            ResourceKey external{"material", options.material_override};
            graph.external.push_back({external, true}); dependencies.insert(external);
        }
        stingray::unit::UnitResource unit;
        if (!stingray::unit::build_unit_v115(unit_scene, unit, error, unit_options)) return false;
        graph.owned.push_back({unit_key, base + ".unit", std::move(unit), {dependencies.begin(), dependencies.end()}});
        graph.roots.push_back(unit_key);
        set_feature_status(feature_status, "unit", "emitted");
    }
    if (plan.emit_animations && selected != no_skin) {
        for (const auto i : plan.animation_indices) {
            if (i >= source.animations.size()) { error = "compilation plan animation index is out of range"; return false; }
            const auto stem = base + "_animation_" + std::to_string(i);
            const auto key = context.generated_key("animation", stem);
            stingray::animation::BuiltAnimation animation;
            if (!stingray::animation::build_skeletal_animation(*emitted_source, emitted_source->skins[selected], emitted_source->animations[i], key.name, animation, error, &options.animation_fit)) return false;
            stingray::animation::SampledAnimationError fidelity;
            std::string fidelity_error;
            if (!stingray::animation::measure_animation_error(animation.cooked, emitted_source->skins[selected], emitted_source->animations[i], fidelity, fidelity_error)) {
                error="post-serialization animation evaluation failed: "+fidelity_error;return false;
            }
            bool best_effort = false;
            for (const auto& fit : animation.fits) best_effort = best_effort || fit.best_effort;
            if(!best_effort && (fidelity.translation>options.animation_fit.translation_tolerance || fidelity.scale>options.animation_fit.scale_tolerance ||
               fidelity.rotation_radians>options.animation_fit.rotation_tolerance_radians)) {
                error="post-serialization animation samples exceed configured fit tolerance";return false;
            }
            animation.fidelity = std::move(fidelity);
            graph.owned.push_back({key, stem + ".animation", std::move(animation), {bones}});
            graph.roots.push_back(key);
            set_feature_status(feature_status, "animation", "emitted");
        }
    }
    // Dangling bones: one pendulum per bone, addressed by its slot in the BONES list.
    std::vector<stingray::BoneConstraint> pendulums;
    const auto bone_count = selected == no_skin ? 0u :
        static_cast<std::uint32_t>(canonical_bone_names(emitted_source->skins[selected]).size());
    if (!source.asset_definition.dangles.empty() && selected != no_skin) {
        const auto& source_skin = source.skins[selected];
        const auto source_names = canonical_bone_names(source_skin);
        const auto emitted_names = canonical_bone_names(emitted_source->skins[selected]);
        for (const auto& dangle : source.asset_definition.dangles) {
            const auto joint = std::find(source_skin.joints.begin(), source_skin.joints.end(), dangle.source_node);
            const auto bone_name = joint == source_skin.joints.end() ? std::string() :
                source_names[static_cast<std::size_t>(joint - source_skin.joints.begin())];
            const auto slot = std::find(emitted_names.begin(), emitted_names.end(), bone_name);
            if (bone_name.empty() || slot == emitted_names.end()) {
                error = "dangling bone '" + source.nodes[static_cast<std::size_t>(dangle.source_node)].name +
                    "' is not a bone of the emitted skeleton";
                return false;
            }
            stingray::BoneConstraint pendulum;
            pendulum.bone_slot = static_cast<std::uint32_t>(slot - emitted_names.begin());
            pendulum.length = dangle.length;
            pendulum.mass = dangle.mass;
            pendulum.gravity = dangle.gravity;
            pendulum.damping = dangle.damping;
            pendulum.rest_stiffness = dangle.stiffness;
            pendulum.max_angle_degrees = dangle.max_angle_degrees;
            if (dangle.jiggle) {
                pendulum.kind = stingray::BoneConstraint::Kind::Spring;
                pendulum.stiffness = dangle.stiffness;
                pendulum.max_stretch = dangle.max_stretch;
            }
            pendulums.push_back(pendulum);
        }
        set_feature_status(feature_status, "dangling_bones", "emitted");
    }
    // Ragdoll handoff: the dynamic node bodies (named after their nodes, see
    // authored_scene.cpp) form the actor set the ragdoll state creates.
    std::vector<std::uint32_t> ragdoll_actors;
    if (!plan.ragdoll_event.empty()) {
        for (const auto& body : source.asset_definition.node_bodies) {
            if (body.actor != "dynamic") continue;
            const auto& name = source.nodes[static_cast<std::size_t>(body.source_node)].name;
            ragdoll_actors.push_back(stingray::id32_from_id64(name.empty() ? body.id : name));
        }
        set_feature_status(feature_status, "ragdoll", "emitted_event_" + plan.ragdoll_event);
    }
    std::optional<ResourceKey> rest_animation;
    if (plan.rest_state) {
        // A constant key on the root joint; every other channel stays at rest.
        const auto& skin = emitted_source->skins[selected];
        if (skin.joints.empty()) { error = "dangling bones need a skin with joints"; return false; }
        const auto& root = emitted_source->nodes[static_cast<std::size_t>(skin.joints.front())];
        AnimationInfo rest;
        rest.name = "rest";
        AnimationTrack track;
        track.target_node = skin.joints.front();
        track.path = AnimationPath::Translation;
        track.times = {0.0f, 1.0f};
        track.value_components = 3;
        for (int key = 0; key < 2; ++key)
            for (int axis = 0; axis < 3; ++axis) track.values.push_back(root.local_stingray[12 + axis]);
        rest.tracks.push_back(std::move(track));
        const auto animation_key = context.generated_key("animation", base + "_rest");
        stingray::animation::BuiltAnimation animation;
        if (!stingray::animation::build_skeletal_animation(*emitted_source, skin, rest, animation_key.name,
                animation, error, &options.animation_fit)) return false;
        graph.owned.push_back({animation_key, base + "_rest.animation", std::move(animation), {bones}});
        rest_animation = animation_key;
        if (plan.ragdoll_event.empty()) {
            const auto machine_key = context.generated_key("state_machine", base);
            auto machine = stingray::write_minimal_single_clip_state_machine(machine_key.name, animation_key.name, true, pendulums, bone_count);
            graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine), {animation_key}});
        }
    }
    if (plan.state_machine_animation_index) {
        const auto animation_index = *plan.state_machine_animation_index;
        if (selected == no_skin || animation_index >= source.animations.size()) {
            error = "state machine plan has no valid selected animation skin";
            return false;
        }
        const auto animation_key = context.generated_key(
            "animation", base + "_animation_" + std::to_string(animation_index));
        const auto machine_key = context.generated_key("state_machine", base);
        auto machine = stingray::write_minimal_single_clip_state_machine(
            machine_key.name, animation_key.name, plan.state_machine_looping, pendulums, bone_count);
        graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine), {animation_key}});
    }
    if (!plan.state_machine_states.empty() || (rest_animation && !plan.ragdoll_event.empty())) {
        if (selected == no_skin) {
            error = "direct-event state machine plan has no valid selected animation skin";
            return false;
        }
        std::vector<stingray::DirectEventState> states;
        std::set<ResourceKey> dependencies;
        states.reserve(plan.state_machine_states.size() + 2);
        if (rest_animation) {
            states.push_back({"rest", rest_animation->name, true});
            dependencies.insert(*rest_animation);
        }
        for (const auto& planned : plan.state_machine_states) {
            if (planned.animation_index >= source.animations.size()) {
                error = "direct-event state machine plan references an invalid animation";
                return false;
            }
            const auto animation_key = context.generated_key("animation",
                base + "_animation_" + std::to_string(planned.animation_index));
            states.push_back({planned.name, animation_key.name, planned.looping});
            dependencies.insert(animation_key);
        }
        auto transitions = plan.state_machine_transitions;
        if (!plan.ragdoll_event.empty()) {
            const auto ragdoll_state = states.size();
            for (std::size_t from = 0; from < ragdoll_state; ++from)
                transitions.push_back({from, ragdoll_state, plan.ragdoll_event, 0.0f});
            stingray::DirectEventState ragdoll;
            ragdoll.name = "ragdoll";
            ragdoll.ragdoll = true;
            states.push_back(ragdoll);
        }
        const auto machine_key = context.generated_key("state_machine", base);
        auto machine = stingray::write_direct_event_state_machine(
            machine_key.name, states, transitions,
            plan.state_machine_variables, plan.state_machine_selectors, pendulums, bone_count, ragdoll_actors);
        graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine),
            {dependencies.begin(), dependencies.end()}});
    }
    std::vector<ResourceKey> effects;
    if (!build_particle_effects(source, context, graph, effects, error)) return false;
    graph.roots.insert(graph.roots.end(), effects.begin(), effects.end());
    return true;
}
}
