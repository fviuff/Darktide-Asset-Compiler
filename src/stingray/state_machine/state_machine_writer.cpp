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
                      std::vector<std::uint8_t>& output, std::string& error) {
    if (states.empty()) { error = "STATE_MACHINE requires at least one authored state"; return false; }
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
        if (state.animation_resource_name.empty()) {
            error = "STATE_MACHINE state animation resource name must be nonempty";
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
    body.u32(1); // one group
    body.u32(static_cast<std::uint32_t>(states.size()));
    for (std::size_t state_index = 0; state_index < states.size(); ++state_index) {
        const auto& state = states[state_index];
        const auto state_id = id64(state.name);
        body.u64(state_id); body.u64(state_id); body.u64(state_id);
        body.u32(0); // state word after identity
        body.u32(1); body.u64(resource_name_hash(state.animation_resource_name));
        body.u32(1); body.u32(0x3f800000u); // one animation, threshold 1
        body.u32(1); body.u8(state.looping ? 1 : 0); body.u32(0);
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
        // Retail transitions reference a constant 0.0 expression (appended after the
        // selector expressions below); 0xffffffff here makes the runtime drop the
        // whole state machine (unit reports no state machine).
        // event graphs still dont work in game even with this, so something else is off too
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
        body.u32(0); // constraint indices
        body.u32(0); body.u32(0); body.u32(0xffffffffu);
    }
    body.u32(0); // initial fallback state
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
    body.u32(0); body.u32(0); body.u32(0);
    body.u32(4); body.u32(0); body.u32(0); body.u32(0);
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
    const std::vector<DirectEventSelector>& selectors) {
    std::vector<std::uint8_t> body;
    std::string error;
    if (!make_direct_body(states, transitions, variables, selectors, body, error))
        throw std::invalid_argument(error);
    auto result = wrap_cooked_resource(kTypeName, resource_name, body);
    if (!validate_direct_event_state_machine(result, resource_name, states, transitions, variables, selectors, error))
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
    std::string& error) {
    std::vector<std::uint8_t> expected_body;
    if (!make_direct_body(states, transitions, variables, selectors, expected_body, error)) return false;
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
    std::uint32_t groups{}, states{};
    if (!reader.u32(groups) || groups != 1 || !reader.u32(states) || states == 0) {
        error = "direct-event STATE_MACHINE must contain one nonempty group";
        return false;
    }
    std::set<std::uint64_t> referenced, expected;
    for (std::uint32_t i = 0; i < states; ++i) {
        std::vector<std::uint64_t> animations;
        if (!skip_direct_state(reader, animations) || animations.size() != 1) {
            error = "direct-event STATE_MACHINE contains an invalid state animation list";
            return false;
        }
        referenced.insert(animations.front());
    }
    std::uint32_t fallback{};
    if (!reader.u32(fallback) || fallback >= states) {
        error = "direct-event STATE_MACHINE initial state is out of range";
        return false;
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
    bool looping) {
    BinaryWriter body;

    body.u32(1); // groups
    body.u32(1); // states in group

    body.u64(id64(kStateName));
    body.u64(id64(kStateName));
    body.u64(id64(kStateName));
    body.u32(0); // state word after identity
    body.u32(1); // animation count
    body.u64(resource_name_hash(animation_resource_name));
    body.u32(1); // threshold count
    body.u32(0x3f800000u); // threshold = 1.0f
    body.u32(1); // policy
    body.u8(looping ? 1 : 0);  // looping
    body.u32(0);
    body.u32(0); // events
    body.u32(0); // transitions
    body.u32(0); // selectors
    body.u32(0);
    body.u32(2);
    body.u32(0); // triples
    body.u32(0);
    body.u32(0);
    body.u32(6); // start, end and speed constant expressions
    body.u32(0); // start time = 0.0f
    body.u32(0x7fa00000u);
    body.u32(0xbf800000u);
    body.u32(0x7fa00000u);
    body.u32(0x3f800000u);
    body.u32(0x7fa00000u);
    body.u32(0); // expression starts
    body.u32(4);
    body.u32(0);
    body.u32(3);
    body.u32(0xffffffffu); // no bone row
    body.u32(0); // constraint indices
    body.u32(0);
    body.u32(0);
    body.u32(0xffffffffu);

    body.u32(0); // group fallback state
    body.u32(0); // root events
    body.u32(1); // variable name count
    body.u32(id32_from_id64(kLengthVariable));
    body.u32(1); // variable initial-value count
    body.u32(0x3f800000u); // current_animation_length = 1.0f
    body.u32(1); // variable min/max count
    body.u32(0xff7fffffu); // -FLT_MAX
    body.u32(0x7f7fffffu);  // FLT_MAX
    body.u32(0); // bone rows
    body.u32(0); // constraint target names
    body.u32(0); // constraint target positions
    body.u32(4); // constraint byte-vector size
    body.u32(0); // empty constraint table
    body.u32(0); // actor sets
    body.u32(0); // base matches
    body.u32(0xbf800000u); // fallback blend time = -1.0f

    auto result = wrap_cooked_resource(kTypeName, resource_name, body.data());
    std::string error;
    std::vector<std::uint8_t> checked_body;
    std::string stream_name;
    if (!parse_cooked_resource_envelope(result, kTypeName, checked_body, stream_name, error) ||
        !stream_name.empty() || !validate_body(checked_body, animation_resource_name, looping)) {
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
