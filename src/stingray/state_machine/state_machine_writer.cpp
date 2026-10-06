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
// Aim records (16 bytes, 236 retail): {1, bone row of the turning bones, target index, aim bone}.
void write_constraint_table(BinaryWriter& body, const std::vector<BoneConstraint>& constraints,
                            const std::vector<std::uint32_t>& aim_rows, const std::vector<std::uint32_t>& aim_targets) {
    constexpr std::uint32_t kPendulumSize = 0x84, kSpringSize = 0x9c, kAimSize = 0x10;
    const auto size_of = [](const BoneConstraint& c) {
        return c.kind == BoneConstraint::Kind::Spring ? kSpringSize : c.kind == BoneConstraint::Kind::Aim ? kAimSize : kPendulumSize;
    };
    const auto count = static_cast<std::uint32_t>(constraints.size());
    std::uint32_t total = 4 + count * 4;
    for (const auto& c : constraints) total += size_of(c);
    body.u32(total);
    body.u32(count);
    std::uint32_t offset = 4 + count * 4;
    for (const auto& c : constraints) { body.u32(offset); offset += size_of(c); }
    for (std::size_t i = 0; i < constraints.size(); ++i) {
        const auto& c = constraints[i];
        if (c.kind == BoneConstraint::Kind::Aim) {
            body.u32(1); body.u32(aim_rows[i]); body.u32(aim_targets[i]); body.u32(c.bone_slot);
            continue;
        }
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
                                   std::uint64_t animation_hash, std::uint32_t first_bone_row = 0) {
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        body.u32(1); // states in group
        write_clip_state(body, id64("constraint_layer_" + std::to_string(layer)), animation_hash, true,
                         first_bone_row + static_cast<std::uint32_t>(layer - 1), layers[layer]);
        body.u32(0); // group fallback state
    }
}

using BoneMask = std::vector<std::pair<std::uint32_t, float>>;

// Bone rows: per BONES slot an index into the row's expressions ("value END" pairs; index 0 = 0.0). The
// authored masks come first, then one row per constraint layer, then one per aim constraint (its turning bones).
void write_constraint_bone_rows(BinaryWriter& body, const std::vector<std::vector<std::uint32_t>>& layers,
                                const std::vector<BoneConstraint>& constraints, std::uint32_t bone_count,
                                const std::vector<BoneMask>& masks, const std::vector<BoneMask>& aim_rows) {
    body.u32(static_cast<std::uint32_t>(masks.size() + layers.size() - 1 + aim_rows.size()));
    const auto write_mask = [&](const BoneMask& mask) {
        std::vector<float> values{0.0f};
        std::vector<std::uint32_t> slots(bone_count, 0);
        for (const auto& [bone, weight] : mask) {
            auto it = std::find(values.begin(), values.end(), weight);
            if (it == values.end()) it = values.insert(values.end(), weight);
            slots[bone] = static_cast<std::uint32_t>(2 * (it - values.begin()));
        }
        body.u32(static_cast<std::uint32_t>(2 * values.size()));
        for (const float value : values) {
            std::uint32_t bits{}; std::memcpy(&bits, &value, sizeof(bits));
            body.u32(bits); body.u32(0x7fa00000u);
        }
        body.u32(bone_count);
        for (const auto slot : slots) body.u32(slot);
        body.u8(1);
    };
    for (const auto& mask : masks) write_mask(mask);
    for (std::size_t layer = 1; layer < layers.size(); ++layer) {
        body.u32(4); body.u32(0); body.u32(0x7fa00000u); body.u32(0x3f800000u); body.u32(0x7fa00000u);
        std::vector<std::uint32_t> weights(bone_count, 0);
        for (const auto index : layers[layer]) {
            weights[constraints[index].bone_slot] = 2;
            for (const auto& [bone, weight] : constraints[index].turn) weights[bone] = 2;   // an aim's turning bones
        }
        body.u32(bone_count);
        for (const auto weight : weights) body.u32(weight);
        body.u8(1);
    }
    for (const auto& row : aim_rows) write_mask(row);
}

// Everything after the variables: bone rows, the constraint targets (names, default positions) and the
// constraint table. Aims get their bone rows after the others and share targets by name.
void write_constraint_tail(BinaryWriter& body, const std::vector<std::vector<std::uint32_t>>& layers,
                           const std::vector<BoneConstraint>& constraints, std::uint32_t bone_count,
                           const std::vector<BoneMask>& masks = {}) {
    std::vector<BoneMask> aim_rows;
    std::vector<const BoneConstraint*> targets;
    std::vector<std::uint32_t> aim_row(constraints.size(), 0), aim_target(constraints.size(), 0);
    for (std::size_t i = 0; i < constraints.size(); ++i) {
        const auto& c = constraints[i];
        if (c.kind != BoneConstraint::Kind::Aim) continue;
        aim_row[i] = static_cast<std::uint32_t>(masks.size() + layers.size() - 1 + aim_rows.size());
        aim_rows.push_back(c.turn);
        auto target = std::find_if(targets.begin(), targets.end(), [&](const auto* t) { return t->target == c.target; });
        if (target == targets.end()) target = targets.insert(targets.end(), &c);
        aim_target[i] = static_cast<std::uint32_t>(target - targets.begin());
    }
    write_constraint_bone_rows(body, layers, constraints, bone_count, masks, aim_rows);
    body.u32(static_cast<std::uint32_t>(targets.size()));
    for (const auto* t : targets) body.u32(id32_from_id64(t->target));
    body.u32(static_cast<std::uint32_t>(targets.size()));
    for (const auto* t : targets) for (const float value : t->target_position) body.f32(value);
    write_constraint_table(body, constraints, aim_row, aim_target);
}

bool valid_constraints(const std::vector<BoneConstraint>& constraints, std::uint32_t bone_count, std::string& error) {
    for (const auto& c : constraints) {
        if (c.bone_slot >= bone_count) {
            error = "STATE_MACHINE bone constraint slot is outside the BONES list";
            return false;
        }
        if (c.kind == BoneConstraint::Kind::Aim) {
            if (c.target.empty() || c.turn.empty()) { error = "STATE_MACHINE aim needs a target name and bones to turn"; return false; }
            for (const auto& [bone, weight] : c.turn)
                if (bone >= bone_count || bone == c.bone_slot || !std::isfinite(weight) || weight < 0.0f || weight > 1.0f) {
                    // the engine stops at a zero-length direction, so the aiming bone cannot turn itself
                    error = "STATE_MACHINE aim bones must be in the BONES list, not the aiming bone, with weights from 0 to 1";
                    return false;
                }
            for (const float value : c.target_position)
                if (!std::isfinite(value)) { error = "STATE_MACHINE aim target position must be finite"; return false; }
            continue;
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

// Blend weights as the Stingray blend-space editor writes them: per axis match_range(variable, lower, value, upper)
// with the neighbouring clip values on that axis (the first and last clip keep full weight beyond the ends, as
// lower == value or upper == value counts as 1); 2D blends weigh both axes at once (match_range_2d). The engine
// normalizes the weights, so the clips can sit on any grid.
std::vector<std::vector<std::uint32_t>> blend_weight_programs(const DirectEventState& state,
                                                              const std::vector<DirectEventState::BlendClip>& clips) {
    const auto axis = [&](bool second) {
        std::vector<float> values;
        for (const auto& clip : clips) values.push_back(second ? clip.value2 : clip.value);
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    };
    const auto bits = [](float value) { std::uint32_t word{}; std::memcpy(&word, &value, sizeof(word)); return word; };
    const auto range = [&](const std::vector<float>& values, float value, std::vector<std::uint32_t>& out) {
        const auto at = static_cast<std::size_t>(std::lower_bound(values.begin(), values.end(), value) - values.begin());
        out.push_back(bits(at > 0 ? values[at - 1] : value));
        out.push_back(bits(value));
        out.push_back(bits(at + 1 < values.size() ? values[at + 1] : value));
    };
    const bool two_d = state.blend_variable2 >= 0;
    const auto xs = axis(false), ys = axis(true);
    std::vector<std::vector<std::uint32_t>> programs;
    for (const auto& clip : clips) {
        std::vector<std::uint32_t> program{0x7f900000u | static_cast<std::uint32_t>(state.blend_variable)};
        range(xs, clip.value, program);
        if (two_d) {
            program.push_back(0x7f900000u | static_cast<std::uint32_t>(state.blend_variable2));
            range(ys, clip.value2, program);
        }
        program.push_back(0x7f800000u | (two_d ? 12u : 11u));   // OP_MATCH_RANGE_2D / OP_MATCH_RANGE
        program.push_back(0x7fa00000u);
        programs.push_back(std::move(program));
    }
    return programs;
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
    const auto clip_state = std::find_if(states.begin(), states.end(), [](const auto& s) { return !s.ragdoll && !s.empty; });
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
        if (!std::isfinite(state.speed) || (state.speed_variable >= 0 && static_cast<std::size_t>(state.speed_variable) >= variables.size())) {
            error = "STATE_MACHINE state speed must be finite and its variable must exist";
            return false;
        }
        if (state.random && state.blend.empty()) {
            error = "STATE_MACHINE random states need clips";
            return false;
        }
        if (!state.blend.empty()) {
            if (state.ragdoll || !state.animation_resource_name.empty()) {
                error = "STATE_MACHINE blend states take their clips from the blend list only";
                return false;
            }
            if (!state.random && state.blend_variable >= variables.size()) {
                error = "STATE_MACHINE blend state variable index is outside the authored variable list";
                return false;
            }
            if (state.random && state.randomization > 2u) {
                error = "STATE_MACHINE random state randomization must be 0, 1 or 2";
                return false;
            }
            if (!state.random && state.blend_variable2 >= 0 && static_cast<std::size_t>(state.blend_variable2) >= variables.size()) {
                error = "STATE_MACHINE 2D blend state second variable index is outside the authored variable list";
                return false;
            }
            for (const auto& clip : state.blend)
                if (clip.animation_resource_name.empty() || !std::isfinite(clip.value) || !std::isfinite(clip.value2) ||
                    (state.random && clip.value <= 0.0f)) {
                    error = "STATE_MACHINE blend clips need an animation and a finite value (random weights above 0)";
                    return false;
                }
        } else if (state.empty) {
            if (state.ragdoll || !state.animation_resource_name.empty()) {
                error = "STATE_MACHINE empty states play no clip";
                return false;
            }
        } else if (state.animation_resource_name.empty() != state.ragdoll) {
            error = "STATE_MACHINE clip states need an animation and ragdoll states none";
            return false;
        }
        for (const auto& [bone, weight] : state.bone_weights)
            if (bone >= bone_count || !std::isfinite(weight) || weight < 0.0f || weight > 1.0f) {
                error = "STATE_MACHINE bone mask bones must be in the BONES list with weights from 0 to 1";
                return false;
            }
        if (state.layer >= states.size()) {
            error = "STATE_MACHINE layer numbers must count up from 0";
            return false;
        }
        if (!std::isfinite(state.exit_blend) || state.exit_blend < 0.0f) {
            error = "STATE_MACHINE exit event time must be finite and nonnegative";
            return false;
        }
        for (const auto& [time, event] : state.events_at)
            if (!std::isfinite(time) || time < 0.0f || event.empty()) {
                error = "STATE_MACHINE timed events need a time from 0 and a name";
                return false;
            }
    }
    // events the clips send themselves (timeline, exit) are listed with the others
    for (const auto& state : states) {
        auto named = state.events_at;
        if (!state.exit_event.empty()) named.push_back({0.0f, state.exit_event});
        for (const auto& [time, event] : named) {
            const auto event_hash = id32_from_id64(event);
            const auto [known, inserted] = event_names.emplace(event_hash, event);
            if (inserted) event_hashes.push_back(event_hash);
            else if (known->second != event) {
                error = "STATE_MACHINE event names collide in the native IdString32 domain";
                return false;
            }
        }
    }
    // Layers in order, each listing its states; a state's index inside its layer is what transitions use.
    std::size_t layer_count = 0;
    for (const auto& state : states) layer_count = std::max(layer_count, state.layer + 1);
    std::vector<std::vector<std::size_t>> layer_states(layer_count);
    std::vector<std::uint32_t> local_state_index(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        local_state_index[i] = static_cast<std::uint32_t>(layer_states[states[i].layer].size());
        layer_states[states[i].layer].push_back(i);
    }
    for (const auto& layer : layer_states)
        if (layer.empty()) { error = "STATE_MACHINE layer numbers must count up from 0 without gaps"; return false; }
    // One bone row per distinct mask.
    std::vector<BoneMask> masks;
    std::vector<std::uint32_t> state_bone_row(states.size(), 0xffffffffu);
    for (std::size_t i = 0; i < states.size(); ++i) {
        if (states[i].bone_weights.empty()) continue;
        auto mask = states[i].bone_weights;
        std::sort(mask.begin(), mask.end());
        auto it = std::find(masks.begin(), masks.end(), mask);
        if (it == masks.end()) it = masks.insert(masks.end(), mask);
        state_bone_row[i] = static_cast<std::uint32_t>(it - masks.begin());
    }
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const auto& transition = transitions[i];
        if (transition.from_state >= states.size() || transition.to_state >= states.size()) {
            error = "STATE_MACHINE transition endpoint is outside the authored state list";
            return false;
        }
        if (states[transition.from_state].layer != states[transition.to_state].layer) {
            error = "STATE_MACHINE transitions stay inside one layer";
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
    body.u32(static_cast<std::uint32_t>(layer_count + layers.size() - 1)); // authored layers + constraint layers
    for (const auto& layer : layer_states) {
        body.u32(static_cast<std::uint32_t>(layer.size()));
        for (const auto state_index : layer) {
            const auto& state = states[state_index];
            const auto state_id = id64(state.name);
            body.u64(state_id); body.u64(state_id); body.u64(state_id);
            if (state.ragdoll) {
                body.u32(4);         // state type: ragdoll
                body.u32(0);         // no animations
                body.u32(0);         // no thresholds
                body.u32(1); body.u8(1); body.u32(0);
            } else if (state.empty) {
                body.u32(1);         // state type: empty (the layers below show through)
                body.u32(0);         // no animations
                body.u32(0);         // no thresholds
                body.u32(1); body.u8(state.looping ? 1 : 0); body.u32(0);
            } else if (state.random) {
                // state type 0 with several clips: cumulative probabilities, the last 1 (pick_animation)
                body.u32(0);
                body.u32(static_cast<std::uint32_t>(state.blend.size()));
                for (const auto& clip : state.blend) body.u64(resource_name_hash(clip.animation_resource_name));
                float total = 0.0f;
                for (const auto& clip : state.blend) total += clip.value;
                body.u32(static_cast<std::uint32_t>(state.blend.size()));
                float running = 0.0f;
                for (std::size_t i = 0; i < state.blend.size(); ++i) {
                    running += state.blend[i].value;
                    const float threshold = i + 1 == state.blend.size() ? 1.0f : running / total;
                    std::uint32_t bits{}; std::memcpy(&bits, &threshold, sizeof(bits)); body.u32(bits);
                }
                body.u32(state.randomization); body.u8(state.looping ? 1 : 0); body.u32(state.additive ? 1 : 0);
            } else if (!state.blend.empty()) {
                body.u32(2); // state type: blend (clips weighted by expressions, as the game's 2271 blend states)
                body.u32(static_cast<std::uint32_t>(state.blend.size()));
                for (const auto& clip : state.blend) body.u64(resource_name_hash(clip.animation_resource_name));
                body.u32(0); // no thresholds
                body.u32(1); body.u8(state.looping ? 1 : 0); body.u32(state.additive ? 1 : 0); // blend_type
            } else {
                body.u32(0); // state type: clip
                body.u32(1); body.u64(resource_name_hash(state.animation_resource_name));
                body.u32(1); body.u32(0x3f800000u); // one animation, threshold 1
                body.u32(1); body.u8(state.looping ? 1 : 0); body.u32(state.additive ? 1 : 0); // blend_type
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
                body.u32(local_state_index[transition.to_state]);
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
            // timeline: {time in seconds, event id32, 1 = send the state machine event}, in time order
            auto markers = state.events_at;
            std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            body.u32(static_cast<std::uint32_t>(markers.size()));
            for (const auto& [time, event] : markers) {
                std::uint32_t time_bits{}; std::memcpy(&time_bits, &time, sizeof(time_bits));
                body.u32(time_bits); body.u32(id32_from_id64(event)); body.u32(1);
            }
            // exit event: sent once when the clip has this many seconds left
            std::uint32_t exit_bits{}; std::memcpy(&exit_bits, &state.exit_blend, sizeof(exit_bits));
            body.u32(state.exit_event.empty() ? 0u : id32_from_id64(state.exit_event));
            body.u32(state.exit_event.empty() ? 0u : exit_bits);
            // blend weights after the other expressions (see blend_weight_programs); then the speed
            // when it is not the constant 1 at word 4 ("value END" or "variable END")
            const auto blend_expressions = transition_expression + (has_transitions ? 2u : 0u);
            const bool own_speed = state.speed_variable >= 0 || state.speed != 1.0f;
            const std::vector<DirectEventState::BlendClip> no_weights;
            const auto& weighted = state.random ? no_weights : state.blend;   // random states pick, they carry no weights
            const auto programs = blend_weight_programs(state, weighted);
            std::vector<std::uint32_t> weight_starts;
            std::uint32_t weight_words = 0;
            for (const auto& program : programs) {
                weight_starts.push_back(blend_expressions + weight_words);
                weight_words += static_cast<std::uint32_t>(program.size());
            }
            const auto speed_expression = blend_expressions + weight_words;
            body.u32(speed_expression + (own_speed ? 2u : 0u));
            body.u32(0); body.u32(0x7fa00000u); body.u32(0xbf800000u);
            body.u32(0x7fa00000u); body.u32(0x3f800000u); body.u32(0x7fa00000u);
            for (const auto selector_index : state_selectors[state_index]) {
                const auto root_variable_index = selectors[selector_index].variable_index;
                body.u32(0x7f900000u | static_cast<std::uint32_t>(root_variable_index));
                body.u32(0x7fa00000u);
            }
            if (has_transitions) { body.u32(0); body.u32(0x7fa00000u); } // transition expression: 0.0
            for (const auto& program : programs)
                for (const auto word : program) body.u32(word);
            if (own_speed) {
                std::uint32_t speed_bits{}; std::memcpy(&speed_bits, &state.speed, sizeof(speed_bits));
                body.u32(state.speed_variable >= 0 ? 0x7f900000u | static_cast<std::uint32_t>(state.speed_variable) : speed_bits);
                body.u32(0x7fa00000u);
            }
            body.u32(static_cast<std::uint32_t>(weighted.size())); // expression starts: each clip's weight
            for (const auto start : weight_starts) body.u32(start);
            body.u32(own_speed ? speed_expression : 4u); // speed expression
            body.u32(0); body.u32(3);
            body.u32(state_bone_row[state_index]); // bone mask row, 0xffffffff = every bone
            // the constraints run in the base layer
            write_state_constraint_indices(body, state.ragdoll || state.layer != 0 ? std::vector<std::uint32_t>{} : layers[0]);
            body.u32(0); body.u32(0);
            body.u32(state.ragdoll ? 0u : 0xffffffffu); // actor set 0 for the ragdoll state
        }
        body.u32(0); // the layer starts in its first state
    }
    if (layers.size() > 1 && clip_state == states.end()) {
        error = "STATE_MACHINE constraint layers need a clip state";
        return false;
    }
    if (layers.size() > 1)
        write_constraint_layer_groups(body, layers, resource_name_hash(clip_state->blend.empty()
            ? clip_state->animation_resource_name : clip_state->blend.front().animation_resource_name),
            static_cast<std::uint32_t>(masks.size()));
    body.u32(static_cast<std::uint32_t>(event_hashes.size()));
    for (const auto event_hash : event_hashes) body.u32(event_hash);
    // The length variable goes last: every frame the engine overwrites the last variable with the current
    // clip's length before it evaluates the speed expressions (retail lists it last too).
    body.u32(static_cast<std::uint32_t>(1 + variables.size()));
    for (const auto& variable : variables) body.u32(id32_from_id64(variable.name));
    body.u32(id32_from_id64(kLengthVariable));
    body.u32(static_cast<std::uint32_t>(1 + variables.size()));
    for (const auto& variable : variables) {
        std::uint32_t bits{}; std::memcpy(&bits, &variable.default_value, sizeof(bits)); body.u32(bits);
    }
    body.u32(0x3f800000u);
    body.u32(static_cast<std::uint32_t>(1 + variables.size()));
    for (const auto& variable : variables) {
        std::uint32_t minimum_bits{}, maximum_bits{};
        std::memcpy(&minimum_bits, &variable.minimum, sizeof(minimum_bits));
        std::memcpy(&maximum_bits, &variable.maximum, sizeof(maximum_bits));
        body.u32(minimum_bits); body.u32(maximum_bits);
    }
    body.u32(0xff7fffffu); body.u32(0x7f7fffffu);
    write_constraint_tail(body, layers, constraints, bone_count, masks);
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
        if (!reader.u32(states) || states == 0) {
            error = "direct-event STATE_MACHINE has an empty group";
            return false;
        }
        for (std::uint32_t i = 0; i < states; ++i) {
            std::vector<std::uint64_t> animations;
            // Clip states carry one animation, blend states several, ragdoll states none.
            if (!skip_direct_state(reader, animations)) {
                error = "direct-event STATE_MACHINE contains an invalid state animation list";
                return false;
            }
            referenced.insert(animations.begin(), animations.end());
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
    write_constraint_tail(body, layers, constraints, bone_count);
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
