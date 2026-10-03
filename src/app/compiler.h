#pragma once
#include "stingray/animation/animation_fitter.h"
#include "stingray/physics/physx_cooking.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dtglb::app {
struct AnimationFitOverrides {
    std::optional<double> translation_tolerance;
    std::optional<double> scale_tolerance;
    std::optional<double> rotation_tolerance_radians;
    std::optional<std::size_t> max_controls;
    std::optional<std::size_t> max_refinements;
    std::optional<std::size_t> max_evaluations;
};
enum class OutputKind { All, Model, Animations };
struct StateMachineStateOption {
    std::string name;
    int clip_index = -1;
    bool looping = true;
};
struct StateMachineVariableOption {
    std::string name;
    float initial_value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
};
struct StateMachineTransitionOption {
    std::size_t from_state = 0;
    std::size_t to_state = 0;
    std::string event_name;
    float blend_seconds = 0.0f;
    std::optional<std::size_t> variable_index;
    float lower = 0.0f;
    float upper = 1.0f;
    bool lower_exclusive = false;
    bool upper_exclusive = false;
};
inline const char* output_kind_name(OutputKind kind) {
    switch (kind) {
        case OutputKind::All: return "all";
        case OutputKind::Model: return "model";
        case OutputKind::Animations: return "animations";
    }
    return "invalid";
}
struct CompileOptions {
    std::filesystem::path input;
    std::filesystem::path output_dir;
    std::string material_override;
    std::optional<std::string> material_variant;
    std::filesystem::path settings_path;
    std::optional<std::string> asset_path;
    std::optional<std::filesystem::path> reference_bones_file;
    std::optional<std::string> skeleton_resource;
    bool auto_materials = true;
    bool physics = true;
    bool validate = true;
    // Universal-render mode bakes the authored fixed morph state by default.
    bool allow_fixed_morph_bake = true;
    bool allow_animation_resampling = false;
    bool allow_native_animation_interpolation = false;
    std::optional<float> asset_scale;
    std::optional<stingray::physics::FittedActorOptions> fitted_physics;
    stingray::animation::FitOptions animation_fit;
    AnimationFitOverrides animation_fit_overrides;
    OutputKind output_kind = OutputKind::All;
    std::optional<int> scene_index;
    std::optional<int> animation_index;
    std::optional<int> loop_clip_index;
    std::optional<int> once_clip_index;
    // Clip embedded as the UNIT's simple animation (played by
    // Unit.play_simple_animation without a state machine). Unset picks the
    // first emitted clip whenever no state machine is emitted.
    std::optional<int> simple_clip_index;
    bool simple_animation = true;
    std::vector<StateMachineStateOption> state_machine_states;
    std::vector<StateMachineVariableOption> state_machine_variables;
    std::vector<StateMachineTransitionOption> state_machine_transitions;
    // Event that switches the unit from animation to ragdoll: its dynamic node bodies
    // are not created at spawn; a state-machine ragdoll state creates and releases them.
    std::string ragdoll_event;
    bool in_place = false; // remove root travel from clips (processing/root_motion.h)
};
int compile(const CompileOptions& options);
}
