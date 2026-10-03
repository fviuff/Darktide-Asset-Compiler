#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dtglb::stingray {

struct DirectEventState {
    std::string name;
    std::string animation_resource_name;
    bool looping = true;
    // Ragdoll state (type 4, no clip): entering it creates the actors in the
    // machine's ragdoll actor set and makes them simulate (set_kinematic false).
    bool ragdoll = false;
};

struct DirectEventTransition {
    std::size_t from_state = 0;
    std::size_t to_state = 0;
    std::string event_name;
    float blend_seconds = 0.0f;
};

// Additional root float variables. Variable index 0 in selectors refers to
// the first authored variable here; serialized root index 0 is reserved for
// current_animation_length.
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
struct BoneConstraint {
    enum class Kind { Pendulum, Spring };
    Kind kind = Kind::Pendulum;
    std::uint32_t bone_slot = 0;
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
