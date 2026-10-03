#include "stingray/state_machine/state_machine_writer.h"

#include "stingray/binary_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace dtglb::stingray {
namespace {

constexpr std::string_view kTypeName = "state_machine";
constexpr std::string_view kStateName = "loop";
constexpr std::string_view kLengthVariable = "current_animation_length";

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& value) { return read(value); }
    bool u32(std::uint32_t& value) { return read(value); }
    bool u64(std::uint64_t& value) { return read(value); }

    bool raw(std::size_t size, const std::uint8_t*& data) {
        if (size > bytes_.size() - position_) return false;
        data = bytes_.data() + position_;
        position_ += size;
        return true;
    }

    bool done() const { return position_ == bytes_.size(); }

private:
    template<class T> bool read(T& value) {
        if (sizeof(T) > bytes_.size() - position_) return false;
        std::memcpy(&value, bytes_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return true;
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
};

// Root constraint table: u32 count, u32 record offsets from the table start, records.
void write_constraint_table(BinaryWriter& body, const std::vector<BoneConstraint>& constraints) {
    constexpr std::uint32_t kPendulumSize = 0x84, kSpringSize = 0x9c;
    const auto size_of = [](const BoneConstraint& c) {
        return c.kind == BoneConstraint::Kind::Spring ? kSpringSize : kPendulumSize;
    };
    const auto count = static_cast<std::uint32_t>(constraints.size());
    std::uint32_t total = 4 + count * 4;
    for (const auto& c : constraints) total += size_of(c);
    body.u32(total);
    body.u32(count);
    std::uint32_t offset = 4 + count * 4;
    for (const auto& c : constraints) { body.u32(offset); offset += size_of(c); }
    for (const auto& c : constraints) {
        if (c.kind == BoneConstraint::Kind::Spring) {
            body.u32(4);                                // spring
            body.u32(1);                                // as in every retail record
            body.u32(0);                                // no debug draw
            body.u32(c.bone_slot);
            body.u32(c.bone_slot);                      // tracked + written bone
            body.u32(0);                                // free (no ground plane)
            for (std::uint32_t slot = 0; slot < 11; ++slot) body.u32(slot * 2);
            for (float value : {c.mass, c.gravity, c.stiffness, c.damping, c.max_stretch,
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
                body.f32(value);
                body.u32(0x7fa00000u);
            }
            continue;
        }
        body.u32(2);                                    // pendulum
        body.u32(1);                                    // enabled
        body.u32(1);                                    // as in every retail record
        body.u32(c.bone_slot);
        body.u32(c.max_angle_degrees > 0.0f ? 2u : 1u); // limit shape: cone when an angle is given
        body.u32(0);                                    // keep the pull toward the rest axis
        for (std::uint32_t slot = 0; slot < 9; ++slot) body.u32(slot * 2);
        for (float value : {c.angle_offset_degrees, c.mass, c.length, c.gravity, c.damping, 0.0f,
                            c.rest_stiffness, c.max_angle_degrees, c.world_collision}) {
            body.f32(value);
            body.u32(0x7fa00000u);                      // constant expression
        }
    }
}

// The engine appends every constraint a state applies to a fixed per-layer array without a bounds check
// (animation_state_machine.cpp, +0x4b0 count / +0x4b8 entries). Retail never lists more than 4 in a state
// (1724 state machines; up to 47 constraints each): the rest go into extra layers, one state each playing the
// same clip, with a bone row that selects only the constrained bones. More than 4 in one state overran that
// array and corrupted the heap (crash when the unit was destroyed).
constexpr std::size_t kConstraintsPerState = 4;

// Constraint index lists: [0] for the authored states, then one per extra layer.
std::vector<std::vector<std::uint32_t>> constraint_layers(std::size_t count) {
    std::vector<std::vector<std::uint32_t>> layers(1);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (layers.back().size() == kConstraintsPerState) layers.emplace_back();
        layers.back().push_back(i);
    }
    return layers;
}

void write_state_constraint_indices(BinaryWriter& body, const std::vector<std::uint32_t>& indices) {
    body.u32(static_cast<std::uint32_t>(indices.size()));
    for (const auto index : indices) body.u32(index);
}

// One looping clip state; bone_row 0xffffffff = whole body.
void write_clip_state(BinaryWriter& body, std::uint64_t state_id, std::uint64_t animation_hash, bool looping,
                      std::uint32_t bone_row, const std::vector<std::uint32_t>& constraints) {
    body.u64(state_id); body.u64(state_id); body.u64(state_id);
    body.u32(0);                                     // state type: clip
    body.u32(1); body.u64(animation_hash);
    body.u32(1); body.u32(0x3f800000u);              // threshold 1
    body.u32(1); body.u8(looping ? 1 : 0); body.u32(0);
    body.u32(0); body.u32(0); body.u32(0);           // events, transitions, selectors
    body.u32(0); body.u32(2);
    body.u32(0);                                     // triples
    body.u32(0); body.u32(0);
    body.u32(6);                                     // start, end and speed constant expressions
    body.u32(0); body.u32(0x7fa00000u); body.u32(0xbf800000u);
    body.u32(0x7fa00000u); body.u32(0x3f800000u); body.u32(0x7fa00000u);
    body.u32(0);                                     // expression starts
    body.u32(4); body.u32(0); body.u32(3);
    body.u32(bone_row);
    write_state_constraint_indices(body, constraints);
    body.u32(0); body.u32(0);
    body.u32(0xffffffffu);                           // no actor set
}

// Extra layers (after the authored group): one state each, same clip, its bone row selecting its bones.
void write_constraint_layer_groups(BinaryWriter& body, const std::vector<std::vector<std::uint32_t>>& layers,
                                   std::uint64_t animation_hash) {
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        body.u32(1); // states in group
        write_clip_state(body, id64("constraint_layer_" + std::to_string(layer)), animation_hash, true,
                         static_cast<std::uint32_t>(layer - 1), layers[layer]);
        body.u32(0); // group fallback state
    }
}

// Bone rows: weight per BONES slot as an index into a constant table [0.0, 1.0].
void write_constraint_bone_rows(BinaryWriter& body, const std::vector<std::vector<std::uint32_t>>& layers,
                                const std::vector<BoneConstraint>& constraints, std::uint32_t bone_count) {
    body.u32(static_cast<std::uint32_t>(layers.size() - 1));
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        body.u32(4); body.u32(0); body.u32(0x7fa00000u); body.u32(0x3f800000u); body.u32(0x7fa00000u);
        std::vector<std::uint32_t> weights(bone_count, 0);
        for (const auto index : layers[layer]) weights[constraints[index].bone_slot] = 2;
        body.u32(bone_count);
        for (const auto weight : weights) body.u32(weight);
        body.u8(1);
    }
}

bool valid_constraints(const std::vector<BoneConstraint>& constraints, std::uint32_t bone_count, std::string& error) {
    for (const auto& c : constraints) {
        if (c.bone_slot >= bone_count) {
            error = "STATE_MACHINE bone constraint slot is outside the BONES list";
            return false;
        }
        for (float v : {c.angle_offset_degrees, c.mass, c.length, c.gravity, c.damping, c.rest_stiffness,
                        c.max_angle_degrees, c.world_collision, c.stiffness, c.max_stretch})
            if (!std::isfinite(v)) { error = "STATE_MACHINE bone constraint values must be finite"; return false; }
        if (c.kind == BoneConstraint::Kind::Spring) {
            if (c.mass <= 0.0f || c.stiffness < 0.0f || c.damping < 0.0f || c.max_stretch <= 0.0f) {
                error = "STATE_MACHINE jiggle needs positive mass and max stretch, nonnegative stiffness and damping";
                return false;
            }
            continue;
        }
        if (c.mass <= 0.0f || c.length <= 0.0f || c.damping < 0.0f || c.max_angle_degrees < 0.0f) {
            error = "STATE_MACHINE pendulum needs positive mass and length, nonnegative damping and angle";
            return false;
        }
    }
    return true;
}

bool expect_u8(Reader& reader, std::uint8_t expected) {
    std::uint8_t value{};
    return reader.u8(value) && value == expected;
}

bool expect_u32(Reader& reader, std::uint32_t expected) {
    std::uint32_t value{};
    return reader.u32(value) && value == expected;
}

bool expect_u64(Reader& reader, std::uint64_t expected) {
    std::uint64_t value{};
    return reader.u64(value) && value == expected;
}

bool skip_words(Reader& reader, std::uint32_t count) {
    std::uint32_t value{};
    for (std::uint32_t i = 0; i < count; ++i) if (!reader.u32(value)) return false;
    return true;
}

bool read_u32_vector(Reader& reader, std::uint32_t& count) {
    return reader.u32(count) && skip_words(reader, count);
}

bool read_u64_vector(Reader& reader, std::vector<std::uint64_t>* values = nullptr) {
    std::uint32_t count{};
    if (!reader.u32(count)) return false;
    if (values) values->clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t value{};
        if (!reader.u64(value)) return false;
        if (values) values->push_back(value);
    }
    return true;
}

bool skip_direct_state(Reader& reader, std::vector<std::uint64_t>& animation_ids) {
    std::uint32_t count{};
    std::uint32_t id32_dummy{};
    std::uint8_t byte{};
    // identity and animation/threshold/policy/loop fields
    std::uint64_t id{};
    for (int i = 0; i < 3; ++i) if (!reader.u64(id)) return false;
    if (!reader.u32(id32_dummy)) return false;
    if (!read_u64_vector(reader, &animation_ids) || !read_u32_vector(reader, count) ||
        !reader.u32(id32_dummy) || !reader.u8(byte) || !reader.u32(id32_dummy)) return false;
    if (!reader.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i)
        if (!reader.u32(id32_dummy) || !reader.u32(id32_dummy) || !reader.u8(byte)) return false;
    if (!reader.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) if (!skip_words(reader, 7)) return false;
    if (!reader.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t cases{};
        if (!reader.u32(id32_dummy) || !reader.u32(cases)) return false;
        for (std::uint32_t c = 0; c < cases; ++c)
            if (!skip_words(reader, 3) || !reader.u8(byte) || !reader.u8(byte)) return false;
    }
    if (!skip_words(reader, 2) || !reader.u32(count)) return false;
    for (std::uint32_t i = 0; i < count; ++i) if (!skip_words(reader, 3)) return false;
    if (
        !skip_words(reader, 2) || !read_u32_vector(reader, count) ||
        !read_u32_vector(reader, count) || !skip_words(reader, 3) ||
        !reader.u32(id32_dummy) || !read_u32_vector(reader, count) || !skip_words(reader, 3)) return false;
    return true;
}

bool validate_body(
    const std::vector<std::uint8_t>& body,
    std::string_view animation_resource_name,
    bool looping) {
    Reader reader(body);
    const auto state_id = id64(kStateName);
    const auto animation_id = resource_name_hash(animation_resource_name);
    const auto length_id = id32_from_id64(kLengthVariable);

    if (!expect_u32(reader, 1) || !expect_u32(reader, 1)) return false;

    // State identity, animation, threshold and the state-local empty tables.
    if (!expect_u64(reader, state_id) || !expect_u64(reader, state_id) ||
        !expect_u64(reader, state_id) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 1) || !expect_u64(reader, animation_id) ||
        !expect_u32(reader, 1) || !expect_u32(reader, 0x3f800000u) ||
        !expect_u32(reader, 1) || !expect_u8(reader, looping ? 1 : 0) ||
        !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 2) ||
        !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 6) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0x7fa00000u) ||
        !expect_u32(reader, 0xbf800000u) || !expect_u32(reader, 0x7fa00000u) ||
        !expect_u32(reader, 0x3f800000u) || !expect_u32(reader, 0x7fa00000u) ||
        !expect_u32(reader, 0) ||
        !expect_u32(reader, 4) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 3) || !expect_u32(reader, 0xffffffffu) ||
        !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 0xffffffffu) ||
        !expect_u32(reader, 0)) return false; // group fallback

    // Root event and variable tables. The length bounds use finite float maxes.
    if (!expect_u32(reader, 0) || !expect_u32(reader, 1) ||
        !expect_u32(reader, length_id) || !expect_u32(reader, 1) ||
        !expect_u32(reader, 0x3f800000u) || !expect_u32(reader, 1) ||
        !expect_u32(reader, 0xff7fffffu) || !expect_u32(reader, 0x7f7fffffu)) return false;

    // Empty bone/target/actor/base-match tables and the observed zero constraint
    // table (a four-byte payload length followed by an empty u32 count).
    if (!expect_u32(reader, 0) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 4) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0) ||
        !expect_u32(reader, 0) || !expect_u32(reader, 0xbf800000u)) return false;

    return reader.done();
}

bool make_direct_body(const std::vector<DirectEventState>& states,
                      const std::vector<DirectEventTransition>& transitions,
                      const std::vector<DirectEventVariable>& variables,
                      const std::vector<DirectEventSelector>& selectors,
                      const std::vector<BoneConstraint>& constraints,
                      std::uint32_t bone_count,
                      const std::vector<std::uint32_t>& ragdoll_actor_names,
                      std::vector<std::uint8_t>& output, std::string& error) {
    if (states.empty()) { error = "STATE_MACHINE requires at least one authored state"; return false; }
    if (!valid_constraints(constraints, bone_count, error)) return false;
    const auto layers = constraint_layers(constraints.size());
    const auto clip_state = std::find_if(states.begin(), states.end(), [](const auto& s) { return !s.ragdoll; });
    const bool has_ragdoll_state = std::any_of(states.begin(), states.end(), [](const auto& s) { return s.ragdoll; });
    if (has_ragdoll_state != !ragdoll_actor_names.empty()) {
        error = "STATE_MACHINE ragdoll states need a ragdoll actor set and vice versa";
        return false;
    }
    std::set<std::string> names;
    std::map<std::uint32_t, std::string> event_names;
    std::vector<std::uint32_t> event_hashes;
    std::vector<std::vector<std::size_t>> state_transitions(states.size());
    std::vector<std::vector<std::size_t>> state_selectors(states.size());
    std::vector<std::size_t> local_transition_indices(transitions.size());
    std::set<std::size_t> selected_transitions;
    std::set<std::pair<std::size_t, std::uint32_t>> selector_events;
    for (const auto& state : states) {
        if (state.name.empty() || !names.insert(state.name).second) {
            error = "STATE_MACHINE state names must be unique and nonempty";
            return false;
        }
        if (state.animation_resource_name.empty() != state.ragdoll) {
            error = "STATE_MACHINE clip states need an animation and ragdoll states none";
            return false;
        }
    }
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const auto& transition = transitions[i];
        if (transition.from_state >= states.size() || transition.to_state >= states.size()) {
            error = "STATE_MACHINE transition endpoint is outside the authored state list";
            return false;
        }
        if (transition.event_name.empty()) {
            error = "STATE_MACHINE transition event name must be nonempty";
            return false;
        }
        if (!std::isfinite(transition.blend_seconds) || transition.blend_seconds < 0.0f) {
            error = "STATE_MACHINE transition blend time must be finite and nonnegative";
            return false;
        }
        const auto event_hash = id32_from_id64(transition.event_name);
        const auto [event, inserted] = event_names.emplace(event_hash, transition.event_name);
        if (inserted) {
            event_hashes.push_back(event_hash);
        } else if (event->second != transition.event_name) {
            error = "STATE_MACHINE event names collide in the native IdString32 domain";
            return false;
        }
        local_transition_indices[i] = state_transitions[transition.from_state].size();
        state_transitions[transition.from_state].push_back(i);
    }

    std::set<std::uint32_t> variable_ids{id32_from_id64(kLengthVariable)};
    for (const auto& variable : variables) {
        if (variable.name.empty() || variable.name == kLengthVariable) {
            error = "STATE_MACHINE variable names must be nonempty and cannot replace current_animation_length";
            return false;
        }
        if (!variable_ids.insert(id32_from_id64(variable.name)).second) {
            error = "STATE_MACHINE variable names collide in the native IdString32 domain";
            return false;
        }
        if (!std::isfinite(variable.minimum) || !std::isfinite(variable.maximum) ||
            !std::isfinite(variable.default_value) || variable.minimum > variable.maximum ||
            variable.default_value < variable.minimum || variable.default_value > variable.maximum) {
            error = "STATE_MACHINE variable defaults and bounds must be finite and the default must be in range";
            return false;
        }
    }

    for (std::size_t selector_index = 0; selector_index < selectors.size(); ++selector_index) {
        const auto& selector = selectors[selector_index];
        if (selector.from_state >= states.size()) {
            error = "STATE_MACHINE selector state is outside the authored state list";
            return false;
        }
        if (selector.event_name.empty()) {
            error = "STATE_MACHINE selector event name must be nonempty";
            return false;
        }
        if (selector.variable_index >= variables.size()) {
            error = "STATE_MACHINE selector variable index is outside the authored variable list";
            return false;
        }
        if (selector.variable_index >= 0x000fffffu) {
            error = "STATE_MACHINE selector variable index does not fit the expression word";
            return false;
        }
        if (selector.cases.empty()) {
            error = "STATE_MACHINE selectors must contain at least one case";
            return false;
        }
        const auto event_hash = id32_from_id64(selector.event_name);
        const auto [event, inserted] = event_names.emplace(event_hash, selector.event_name);
        if (inserted) event_hashes.push_back(event_hash);
        else if (event->second != selector.event_name) {
            error = "STATE_MACHINE event names collide in the native IdString32 domain";
            return false;
        }
        if (!selector_events.emplace(selector.from_state, event_hash).second) {
            error = "STATE_MACHINE has more than one selector for the same state event";
            return false;
        }
        state_selectors[selector.from_state].push_back(selector_index);
        for (const auto& selector_case : selector.cases) {
            if (selector_case.transition_index >= transitions.size()) {
                error = "STATE_MACHINE selector case transition index is outside the authored transition list";
                return false;
            }
            const auto& transition = transitions[selector_case.transition_index];
            if (transition.from_state != selector.from_state || transition.event_name != selector.event_name) {
                error = "STATE_MACHINE selector case must reference a transition with the same state and event";
                return false;
            }
            if (!std::isfinite(selector_case.lower) || !std::isfinite(selector_case.upper) ||
                selector_case.lower > selector_case.upper) {
                error = "STATE_MACHINE selector case bounds must be finite and ordered";
                return false;
            }
            if (selector_case.lower == selector_case.upper &&
                (selector_case.lower_exclusive || selector_case.upper_exclusive)) {
                error = "STATE_MACHINE selector case cannot exclude either edge of a zero-width range";
                return false;
            }
            if (!selected_transitions.insert(selector_case.transition_index).second) {
                error = "STATE_MACHINE transition cannot belong to more than one selector case";
                return false;
            }
        }
    }
    for (std::size_t transition_index = 0; transition_index < transitions.size(); ++transition_index) {
        const auto& transition = transitions[transition_index];
        const auto key = std::make_pair(transition.from_state, id32_from_id64(transition.event_name));
        if (selector_events.count(key) && !selected_transitions.count(transition_index)) {
            error = "STATE_MACHINE selector event cannot also have an unconditional transition";
            return false;
        }
    }
    std::sort(event_hashes.begin(), event_hashes.end());

    BinaryWriter body;
    body.u32(static_cast<std::uint32_t>(layers.size())); // authored group + constraint layers
    body.u32(static_cast<std::uint32_t>(states.size()));
    for (std::size_t state_index = 0; state_index < states.size(); ++state_index) {
        const auto& state = states[state_index];
        const auto state_id = id64(state.name);
        body.u64(state_id); body.u64(state_id); body.u64(state_id);
        if (state.ragdoll) {
            body.u32(4);         // state type: ragdoll
            body.u32(0);         // no animations
            body.u32(0);         // no thresholds
            body.u32(1); body.u8(1); body.u32(0);
        } else {
            body.u32(0); // state type: clip
            body.u32(1); body.u64(resource_name_hash(state.animation_resource_name));
            body.u32(1); body.u32(0x3f800000u); // one animation, threshold 1
            body.u32(1); body.u8(state.looping ? 1 : 0); body.u32(0);
        }
        body.u32(static_cast<std::uint32_t>(state_transitions[state_index].size() -
            std::count_if(state_transitions[state_index].begin(), state_transitions[state_index].end(),
                [&selected_transitions](std::size_t index) { return selected_transitions.count(index) != 0; }) +
            state_selectors[state_index].size()));
        std::uint32_t local_transition_index = 0;
        for (const auto transition_index : state_transitions[state_index]) {
            const auto& transition = transitions[transition_index];
            if (!selected_transitions.count(transition_index)) {
                body.u32(id32_from_id64(transition.event_name));
                body.u32(local_transition_index);
                body.u8(0); // unconditional direct event binding
            }
            ++local_transition_index;
        }
        for (std::size_t local_selector_index = 0;
             local_selector_index < state_selectors[state_index].size(); ++local_selector_index) {
            const auto& selector = selectors[state_selectors[state_index][local_selector_index]];
            body.u32(id32_from_id64(selector.event_name));
            body.u32(static_cast<std::uint32_t>(local_selector_index));
            body.u8(1); // selector event binding
        }
        // Retail transitions reference a constant 0.0 expression, appended after the
        // selector expressions below.
        const bool has_transitions = !state_transitions[state_index].empty();
        const auto transition_expression = static_cast<std::uint32_t>(6 + state_selectors[state_index].size() * 2);
        body.u32(static_cast<std::uint32_t>(state_transitions[state_index].size()));
        for (const auto transition_index : state_transitions[state_index]) {
            const auto& transition = transitions[transition_index];
            std::uint32_t blend_bits = 0;
            std::memcpy(&blend_bits, &transition.blend_seconds, sizeof(blend_bits));
            body.u32(static_cast<std::uint32_t>(transition.to_state));
            body.u32(blend_bits);
            body.u32(0); // DIRECT
            body.u32(0); body.u32(transition_expression); body.u32(1); body.u32(0);
        }
        body.u32(static_cast<std::uint32_t>(state_selectors[state_index].size()));
        for (std::size_t local_selector_index = 0;
             local_selector_index < state_selectors[state_index].size(); ++local_selector_index) {
            const auto& selector = selectors[state_selectors[state_index][local_selector_index]];
            body.u32(static_cast<std::uint32_t>(6 + local_selector_index * 2));
            body.u32(static_cast<std::uint32_t>(selector.cases.size()));
            for (const auto& selector_case : selector.cases) {
                std::uint32_t lower_bits{}, upper_bits{};
                std::memcpy(&lower_bits, &selector_case.lower, sizeof(lower_bits));
                std::memcpy(&upper_bits, &selector_case.upper, sizeof(upper_bits));
                body.u32(static_cast<std::uint32_t>(local_transition_indices[selector_case.transition_index]));
                body.u32(lower_bits); body.u32(upper_bits);
                body.u8(selector_case.lower_exclusive ? 1 : 0);
                body.u8(selector_case.upper_exclusive ? 1 : 0);
            }
        }
        body.u32(0); body.u32(2); // words b0/b4
        body.u32(0); // triples
        body.u32(0); body.u32(0); // words c0/c4
        body.u32(transition_expression + (has_transitions ? 2u : 0u));
        body.u32(0); body.u32(0x7fa00000u); body.u32(0xbf800000u);
        body.u32(0x7fa00000u); body.u32(0x3f800000u); body.u32(0x7fa00000u);
        for (const auto selector_index : state_selectors[state_index]) {
            const auto root_variable_index = selectors[selector_index].variable_index + 1;
            body.u32(0x7f900000u | static_cast<std::uint32_t>(root_variable_index));
            body.u32(0x7fa00000u);
        }
        if (has_transitions) { body.u32(0); body.u32(0x7fa00000u); } // transition expression: 0.0
        body.u32(0); // expression starts
        body.u32(4); body.u32(0); body.u32(3);
        body.u32(0xffffffffu); // no bone row
        write_state_constraint_indices(body, state.ragdoll ? std::vector<std::uint32_t>{} : layers[0]);
        body.u32(0); body.u32(0);
        body.u32(state.ragdoll ? 0u : 0xffffffffu); // actor set 0 for the ragdoll state
    }
    body.u32(0); // initial fallback state
    if (layers.size() > 1 && clip_state == states.end()) {
        error = "STATE_MACHINE constraint layers need a clip state";
        return false;
    }
    if (layers.size() > 1) write_constraint_layer_groups(body, layers, resource_name_hash(clip_state->animation_resource_name));
    body.u32(static_cast<std::uint32_t>(event_hashes.size()));
    for (const auto event_hash : event_hashes) body.u32(event_hash);
    body.u32(static_cast<std::uint32_t>(1 + variables.size()));
    body.u32(id32_from_id64(kLengthVariable));
    for (const auto& variable : variables) body.u32(id32_from_id64(variable.name));
    body.u32(static_cast<std::uint32_t>(1 + variables.size())); body.u32(0x3f800000u);
    for (const auto& variable : variables) {
        std::uint32_t bits{}; std::memcpy(&bits, &variable.default_value, sizeof(bits)); body.u32(bits);
    }
    body.u32(static_cast<std::uint32_t>(1 + variables.size()));
    body.u32(0xff7fffffu); body.u32(0x7f7fffffu);
    for (const auto& variable : variables) {
        std::uint32_t minimum_bits{}, maximum_bits{};
        std::memcpy(&minimum_bits, &variable.minimum, sizeof(minimum_bits));
        std::memcpy(&maximum_bits, &variable.maximum, sizeof(maximum_bits));
        body.u32(minimum_bits); body.u32(maximum_bits);
    }
    write_constraint_bone_rows(body, layers, constraints, bone_count);
    body.u32(0); body.u32(0); // constraint target names and positions
    write_constraint_table(body, constraints);
    // Actor sets: list A is created and set simulating when the ragdoll state is entered.
    if (ragdoll_actor_names.empty()) {
        body.u32(0);
    } else {
        body.u32(1);
        body.u32(static_cast<std::uint32_t>(ragdoll_actor_names.size()));
        for (const auto name : ragdoll_actor_names) body.u32(name);
        body.u32(0);
    }
    body.u32(0); // base matches
    body.u32(0xbf800000u);
    output = body.data();
    error.clear();
    return true;
}

} // namespace

std::vector<std::uint8_t> write_direct_event_state_machine(
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions) {
    return write_direct_event_state_machine(resource_name, states, transitions, {}, {});
}

std::vector<std::uint8_t> write_direct_event_state_machine(
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    const std::vector<DirectEventVariable>& variables,
    const std::vector<DirectEventSelector>& selectors,
    const std::vector<BoneConstraint>& constraints,
    std::uint32_t bone_count,
    const std::vector<std::uint32_t>& ragdoll_actor_names) {
    std::vector<std::uint8_t> body;
    std::string error;
    if (!make_direct_body(states, transitions, variables, selectors, constraints, bone_count, ragdoll_actor_names, body, error))
        throw std::invalid_argument(error);
    auto result = wrap_cooked_resource(kTypeName, resource_name, body);
    if (!validate_direct_event_state_machine(result, resource_name, states, transitions, variables, selectors, error,
                                             constraints, bone_count, ragdoll_actor_names))
        throw std::logic_error("generated direct-event STATE_MACHINE failed self-validation: " + error);
    return result;
}

bool validate_direct_event_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    std::string& error) {
    return validate_direct_event_state_machine(bytes, resource_name, states, transitions, {}, {}, error);
}

bool validate_direct_event_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<DirectEventState>& states,
    const std::vector<DirectEventTransition>& transitions,
    const std::vector<DirectEventVariable>& variables,
    const std::vector<DirectEventSelector>& selectors,
    std::string& error,
    const std::vector<BoneConstraint>& constraints,
    std::uint32_t bone_count,
    const std::vector<std::uint32_t>& ragdoll_actor_names) {
    std::vector<std::uint8_t> expected_body;
    if (!make_direct_body(states, transitions, variables, selectors, constraints, bone_count, ragdoll_actor_names, expected_body, error))
        return false;
    std::vector<std::uint8_t> actual_body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(bytes, kTypeName, actual_body, stream_name, error)) return false;
    if (!stream_name.empty()) { error = "direct-event STATE_MACHINE unexpectedly has a stream name"; return false; }
    if (bytes.size() < 16) { error = "cooked STATE_MACHINE is shorter than its identity fields"; return false; }
    std::uint64_t name_hash{};
    std::memcpy(&name_hash, bytes.data() + 8, sizeof(name_hash));
    if (name_hash != resource_name_hash(resource_name)) { error = "cooked STATE_MACHINE name hash mismatch"; return false; }
    if (actual_body != expected_body) { error = "STATE_MACHINE body differs from the requested direct-event shape"; return false; }
    error.clear();
    return true;
}

bool validate_direct_event_state_machine_dependencies(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    const std::vector<std::string>& animation_resource_names,
    std::string& error) {
    if (animation_resource_names.empty()) {
        error = "direct-event STATE_MACHINE has no animation dependencies";
        return false;
    }
    std::vector<std::uint8_t> body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(bytes, kTypeName, body, stream_name, error)) return false;
    if (!stream_name.empty()) { error = "direct-event STATE_MACHINE unexpectedly has a stream name"; return false; }
    if (bytes.size() < 16) { error = "cooked STATE_MACHINE is shorter than its identity fields"; return false; }
    std::uint64_t name_hash{};
    std::memcpy(&name_hash, bytes.data() + 8, sizeof(name_hash));
    if (name_hash != resource_name_hash(resource_name)) { error = "cooked STATE_MACHINE name hash mismatch"; return false; }

    Reader reader(body);
    std::uint32_t groups{};
    if (!reader.u32(groups) || groups == 0) {
        error = "direct-event STATE_MACHINE must contain a group";
        return false;
    }
    std::set<std::uint64_t> referenced, expected;
    // the authored group, then single-state constraint layers
    for (std::uint32_t group = 0; group < groups; ++group) {
        std::uint32_t states{};
        if (!reader.u32(states) || states == 0 || (group > 0 && states != 1)) {
            error = "direct-event STATE_MACHINE has an empty group or a constraint layer with more than one state";
            return false;
        }
        for (std::uint32_t i = 0; i < states; ++i) {
            std::vector<std::uint64_t> animations;
            // Clip states carry one animation; ragdoll states none.
            if (!skip_direct_state(reader, animations) || animations.size() > 1) {
                error = "direct-event STATE_MACHINE contains an invalid state animation list";
                return false;
            }
            if (!animations.empty()) referenced.insert(animations.front());
        }
        std::uint32_t fallback{};
        if (!reader.u32(fallback) || fallback >= states) {
            error = "direct-event STATE_MACHINE initial state is out of range";
            return false;
        }
    }
    // Consume the root tables emitted by this bounded writer and require exact EOF.
    std::uint32_t count{}, ignored{};
    if (!read_u32_vector(reader, count) || !read_u32_vector(reader, count) ||
        !read_u32_vector(reader, count) || !reader.u32(count)) {
        error = "truncated direct-event STATE_MACHINE root tables";
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) if (!skip_words(reader, 2)) {
        error = "truncated direct-event STATE_MACHINE variable bounds"; return false;
    }
    if (!reader.u32(count)) { error = "truncated direct-event STATE_MACHINE bone rows"; return false; }
    std::uint8_t byte{};
    for (std::uint32_t i = 0; i < count; ++i)
        if (!read_u32_vector(reader, ignored) || !read_u32_vector(reader, ignored) || !reader.u8(byte)) {
            error = "truncated direct-event STATE_MACHINE bone rows"; return false;
        }
    if (!read_u32_vector(reader, count) || !reader.u32(count)) {
        error = "truncated direct-event STATE_MACHINE constraint tables"; return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) if (!skip_words(reader, 3)) {
        error = "truncated direct-event STATE_MACHINE constraint positions"; return false;
    }
    std::uint32_t blob_size{};
    const std::uint8_t* blob{};
    if (!reader.u32(blob_size) || !reader.raw(blob_size, blob) || !reader.u32(count)) {
        error = "truncated direct-event STATE_MACHINE constraint or actor tables"; return false;
    }
    for (std::uint32_t i = 0; i < count; ++i)
        if (!read_u32_vector(reader, ignored) || !read_u32_vector(reader, ignored)) {
            error = "truncated direct-event STATE_MACHINE actor set"; return false;
        }
    if (!reader.u32(count)) { error = "truncated direct-event STATE_MACHINE base matches"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t value64{};
        if (!reader.u64(value64) || !reader.u64(value64) || !reader.u64(value64) || !reader.u32(ignored)) {
            error = "truncated direct-event STATE_MACHINE base match"; return false;
        }
    }
    if (!reader.u32(ignored) || !reader.done()) {
        error = "direct-event STATE_MACHINE has invalid trailing data";
        return false;
    }
    for (const auto& animation : animation_resource_names)
        expected.insert(resource_name_hash(animation));
    if (referenced != expected) {
        error = "STATE_MACHINE animation references do not match graph dependencies";
        return false;
    }
    error.clear();
    return true;
}

std::vector<std::uint8_t> write_minimal_looping_state_machine(
    std::string_view resource_name,
    std::string_view animation_resource_name) {
    return write_minimal_single_clip_state_machine(resource_name, animation_resource_name, true);
}

std::vector<std::uint8_t> write_minimal_single_clip_state_machine(
    std::string_view resource_name,
    std::string_view animation_resource_name,
    bool looping,
    const std::vector<BoneConstraint>& constraints,
    std::uint32_t bone_count) {
    {
        std::string error;
        if (!valid_constraints(constraints, bone_count, error)) throw std::invalid_argument(error);
    }
    const auto layers = constraint_layers(constraints.size());
    const auto animation_hash = resource_name_hash(animation_resource_name);
    BinaryWriter body;

    body.u32(static_cast<std::uint32_t>(layers.size())); // groups: the clip + constraint layers
    body.u32(1); // states in group
    write_clip_state(body, id64(kStateName), animation_hash, looping, 0xffffffffu, layers[0]);
    body.u32(0); // group fallback state
    write_constraint_layer_groups(body, layers, animation_hash);
    body.u32(0); // root events
    body.u32(1); // variable name count
    body.u32(id32_from_id64(kLengthVariable));
    body.u32(1); // variable initial-value count
    body.u32(0x3f800000u); // current_animation_length = 1.0f
    body.u32(1); // variable min/max count
    body.u32(0xff7fffffu); // -FLT_MAX
    body.u32(0x7f7fffffu);  // FLT_MAX
    write_constraint_bone_rows(body, layers, constraints, bone_count);
    body.u32(0); // constraint target names
    body.u32(0); // constraint target positions
    write_constraint_table(body, constraints);
    body.u32(0); // actor sets
    body.u32(0); // base matches
    body.u32(0xbf800000u); // fallback blend time = -1.0f

    auto result = wrap_cooked_resource(kTypeName, resource_name, body.data());
    std::string error;
    std::vector<std::uint8_t> checked_body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(result, kTypeName, checked_body, stream_name, error) ||
        !stream_name.empty() ||
        !(constraints.empty()
              ? validate_body(checked_body, animation_resource_name, looping)
              : validate_direct_event_state_machine_dependencies(
                    result, resource_name, {std::string(animation_resource_name)}, error))) {
        if (error.empty()) error = "STATE_MACHINE body does not match the requested single-clip shape";
        throw std::logic_error("generated STATE_MACHINE failed self-validation: " + error);
    }
    return result;
}

bool validate_minimal_looping_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    std::string_view animation_resource_name,
    std::string& error) {
    std::vector<std::uint8_t> body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(bytes, kTypeName, body, stream_name, error)) return false;
    if (!stream_name.empty()) {
        error = "minimal STATE_MACHINE unexpectedly has a stream name";
        return false;
    }
    if (bytes.size() < 16) {
        error = "cooked STATE_MACHINE is shorter than its identity fields";
        return false;
    }
    std::uint64_t name_hash{};
    std::memcpy(&name_hash, bytes.data() + 8, sizeof(name_hash));
    if (name_hash != resource_name_hash(resource_name)) {
        error = "cooked STATE_MACHINE name hash mismatch";
        return false;
    }
    if (!validate_body(body, animation_resource_name, true)) {
        error = "STATE_MACHINE body does not match the validated minimal looping shape";
        return false;
    }
    error.clear();
    return true;
}

bool validate_minimal_single_clip_state_machine(
    const std::vector<std::uint8_t>& bytes,
    std::string_view resource_name,
    std::string_view animation_resource_name,
    std::string& error) {
    std::vector<std::uint8_t> body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(bytes, kTypeName, body, stream_name, error)) return false;
    if (!stream_name.empty()) {
        error = "minimal STATE_MACHINE unexpectedly has a stream name";
        return false;
    }
    if (bytes.size() < 16) {
        error = "cooked STATE_MACHINE is shorter than its identity fields";
        return false;
    }
    std::uint64_t name_hash{};
    std::memcpy(&name_hash, bytes.data() + 8, sizeof(name_hash));
    if (name_hash != resource_name_hash(resource_name)) {
        error = "cooked STATE_MACHINE name hash mismatch";
        return false;
    }
    if (!validate_body(body, animation_resource_name, true) &&
        !validate_body(body, animation_resource_name, false)) {
        error = "STATE_MACHINE body does not match the validated minimal single-clip shape";
        return false;
    }
    error.clear();
    return true;
}

} // namespace dtglb::stingray
