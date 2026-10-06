#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dtglb::stingray {

struct DirectEventState {
    std::string name;
    std::string animation_resource_name;
    bool looping = true;
    // Ragdoll state (type 4, no clip): entering it creates the actors in the
    // machine's ragdoll actor set and makes them simulate (set_kinematic false).
    bool ragdoll = false;
    // Blend state (type 2, used instead of animation_resource_name): each clip plays fully at its value and fades
    // into its neighbours (match_range); with a second variable the clips sit on a 2D grid (value, value2).
    struct BlendClip { std::string animation_resource_name; float value = 0.0f; float value2 = 0.0f; };
    std::vector<BlendClip> blend;
    std::size_t blend_variable = 0;   // index into the authored DirectEventVariable list
    int blend_variable2 = -1;         // second variable of a 2D blend, -1 = 1D
    // Playback speed: a constant, or the value of a variable (index into the authored list, -1 = none) that scripts
    // set; the game's states use both (an expression like the blend weights).
    float speed = 1.0f;
    int speed_variable = -1;
    // Random state (type 0 with several clips): blend holds the clips with their weights (value); one is picked
    // on entry (randomization 0), every loop (1) or every loop without repeating the last (2).
    bool random = false;
    std::uint32_t randomization = 1;
    // Layers: layer 0 is the base; each further layer plays on top of the ones below, starting in its first
    // state. Transitions stay inside a layer; an event reaches every layer's current state.
    std::size_t layer = 0;
    // Empty state (type 1, no clip): the layers below show through.
    bool empty = false;
    // Bone mask (BONES slot, weight 0..1); bones not listed get 0. Empty = every bone at full weight.
    std::vector<std::pair<std::uint32_t, float>> bone_weights;
    // Additive (blend_type BT_OFFSET): the clip is added on top of the layers below instead of replacing them.
    bool additive = false;
    // Timeline: state machine events sent when the clip reaches a time (seconds), like Unit.animation_event.
    std::vector<std::pair<float, std::string>> events_at;
    // Exit event: sent once when exit_blend seconds of the clip are left (e.g. back to idle after a one-shot).
    std::string exit_event;
    float exit_blend = 0.2f;
};

struct DirectEventTransition {
    std::size_t from_state = 0;
    std::size_t to_state = 0;
    std::string event_name;
    float blend_seconds = 0.0f;
};

// Root float variables; expressions refer to them by their index here (the clip-length variable the engine
// needs goes after them).
struct DirectEventVariable {
    std::string name;
    float default_value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct SelectorCase {
    // Global index into the DirectEventTransition list passed to the writer.
    std::size_t transition_index = 0;
    float lower = 0.0f;
    float upper = 0.0f;
    bool lower_exclusive = false;
    bool upper_exclusive = false;
};

struct DirectEventSelector {
    std::size_t from_state = 0;
    std::string event_name;
    // Zero-based index into the authored DirectEventVariable list.
    std::size_t variable_index = 0;
    std::vector<SelectorCase> cases;
};

// Procedural bone constraints; bone_slot indexes the unit's BONES list. At most 4 are applied per state (engine
// limit, see state_machine_writer.cpp); more are written into extra single-state layers with bone rows.
// Pendulum (type 2, engine FUN_14063b220): swings the bone, whose local +X is
// the hanging axis. Spring (type 4, FUN_14063f860): the bone's translation
// lags its animated position (jiggle); stiffness/damping/max_stretch apply.
// Aim (type 1, 16 bytes): every bone in `turn` rotates by its weight so that the direction from it to the aim
// bone (bone_slot) points at the named constraint target, which scripts move with
// Unit.animation_set_constraint_target (world position); target_position is its start value.
struct BoneConstraint {
    enum class Kind { Pendulum, Spring, Aim };
    Kind kind = Kind::Pendulum;
    std::uint32_t bone_slot = 0;
    std::vector<std::pair<std::uint32_t, float>> turn;   // aim: BONES slot -> weight
    std::string target;                                  // aim: constraint target name
    float target_position[3] = {0.0f, 5.0f, 1.0f};
    float angle_offset_degrees = 0.0f;
    float mass = 1.0f;
    float length = 0.5f;
    float gravity = 9.82f;
    float damping = 3.0f;
    float rest_stiffness = 0.0f;
    float max_angle_degrees = 0.0f; // 0 = unlimited
    float world_collision = 0.0f;
    float stiffness = 1000.0f;      // spring
    float max_stretch = 0.1f;       // spring
};

std::vector<std::uint8_t> write_direct_event_state_machine(
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions);

std::vector<std::uint8_t> write_direct_event_state_machine(
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    const std::vector<DirectEventVariable>& variables,
    const std::vector<DirectEventSelector>& selectors,
    const std::vector<BoneConstraint>& constraints = {},
    std::uint32_t bone_count = 0, // BONES list size; required with constraints (bone rows of constraint layers)
    const std::vector<std::uint32_t>& ragdoll_actor_names = {});

bool validate_direct_event_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    std::string& error);

bool validate_direct_event_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    const std::vector<DirectEventVariable>& variables,
    const std::vector<DirectEventSelector>& selectors,
    std::string& error,
    const std::vector<BoneConstraint>& constraints = {},
    std::uint32_t bone_count = 0, // BONES list size; required with constraints (bone rows of constraint layers)
    const std::vector<std::uint32_t>& ragdoll_actor_names = {});

bool validate_direct_event_state_machine_dependencies(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<std::string>& animation_resource_names,
    std::string& error);

// Writes the smallest observed Darktide looping state-machine shape: one
// group, one state named "loop", one compiled animation, and no constraints.
std::vector<std::uint8_t> write_minimal_looping_state_machine(
    std::string_view resource_name,
    std::string_view animation_resource_name);

std::vector<std::uint8_t> write_minimal_single_clip_state_machine(
    std::string_view resource_name,
    std::string_view animation_resource_name,
    bool looping,
    const std::vector<BoneConstraint>& constraints = {},
    std::uint32_t bone_count = 0);

// Validates both the cooked-resource envelope and the complete minimal body.
bool validate_minimal_looping_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    std::string_view animation_resource_name,
    std::string& error);

bool validate_minimal_single_clip_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    std::string_view animation_resource_name,
    std::string& error);

} // namespace dtglb::stingray
