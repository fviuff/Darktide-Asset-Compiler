#pragma once

#include "app/asset_settings.h"
#include "app/compiler.h"
#include "scene/scene.h"
#include "stingray/state_machine/state_machine_writer.h"

#include <optional>
#include <string>
#include <vector>

namespace dtglb::compiler {

struct PlannedStateMachineState {
    std::string name;
    std::size_t animation_index = 0;
    bool looping = true;
};

struct CompilationPlan {
    bool emit_unit = false;
    bool placeholder_unit = false;
    bool emit_materials = false;
    bool emit_animations = false;
    std::optional<std::size_t> skin_index;
    std::optional<std::size_t> state_machine_animation_index;
    bool state_machine_looping = true;
    // Scene animation embedded as the UNIT simple animation.
    std::optional<std::size_t> simple_animation_index;
    std::vector<PlannedStateMachineState> state_machine_states;
    std::vector<stingray::DirectEventTransition> state_machine_transitions;
    std::vector<stingray::DirectEventVariable> state_machine_variables;
    std::vector<stingray::DirectEventSelector> state_machine_selectors;
    // Dangling bones or a ragdoll event without an authored state machine: emit a
    // state over a generated rest-pose clip so the machine has something to run.
    bool rest_state = false;
    // Adds a ragdoll state entered on this event from every other state.
    std::string ragdoll_event;
    // Indices into the unchanged scene animation array, preserving native identities.
    std::vector<std::size_t> animation_indices;
    std::vector<std::string> gaps;
    std::vector<std::string> approximations;
};

CompilationPlan plan_compilation(const Scene& scene, const app::CompileOptions& options,
                                 const app::ResolvedPhysicsInventory& physics);

} // namespace dtglb::compiler
