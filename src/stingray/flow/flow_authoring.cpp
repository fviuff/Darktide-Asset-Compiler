#include "stingray/flow/flow_authoring.h"

#include "stingray/murmur_hash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

namespace dtglb::stingray::flow {
namespace {

// Payload layouts of the game's flow nodes (from the node handlers in the game exe and the retail graphs). Every word is a byte offset into the dynamic data, 0xffffffff when unset, except
// the inline output-event lists {count, node | in_event << 16 ...}.
enum class Kind { Unit, Bool, U32, Float, Vector3, Quaternion, String, Name, Mesh, Material, Light };
enum class Role { Input, Output, State, Events, Unset, Zero };

struct Slot {
    Role role;
    const char* name;   // input/output name, output event name for Events
    Kind kind;
    bool required = false;
};

struct NodeSpec {
    const char* name;
    std::uint32_t type;
    std::vector<Slot> layout;
    std::vector<const char*> in_events;
    bool source = false;   // fired by the engine (unit_spawned) or an external event, has no input events
    // computes values only, no events: a node reading its output lists it in its query list (at the end of its
    // payload, {count, node | 0xffff << 16}) and the engine runs it, and its own query list, first
    bool getter = false;
};

const std::vector<NodeSpec>& specs() {
    static const std::vector<NodeSpec> table = {
        {"unit_spawned", 0x07, {{Role::Output, "unit", Kind::Unit}, {Role::Events, "out", Kind::U32}}, {}, true},
        // fired by the engine's external event unit_unspawned when the unit is destroyed (605 retail units)
        {"unit_unspawned", 0xed, {{Role::Output, "unit", Kind::Unit}, {Role::Events, "out", Kind::U32}}, {}, true},
        {"external_event", 0x00, {{Role::State, "", Kind::Bool}, {Role::Events, "out", Kind::U32}}, {}, true},
        {"particle_effect", 0x0b,
         {{Role::Input, "effect", Kind::String, true}, {Role::Input, "object", Kind::Name},
          {Role::Input, "linked", Kind::Bool}, {Role::Input, "orphaned_policy", Kind::U32},
          {Role::Input, "offset", Kind::Vector3}, {Role::Input, "rotation_offset", Kind::Quaternion},
          {Role::Input, "unit", Kind::Unit}, {Role::Unset, "", Kind::Float}, {Role::Output, "effect_id", Kind::U32}},
         {"create", "stop", "kill"}},
        {"set_unit_visibility", 0x1b,
         {{Role::Input, "unit", Kind::Unit}, {Role::Input, "group", Kind::Name}, {Role::Input, "visible", Kind::Bool, true},
          {Role::Unset, "", Kind::Bool}, {Role::Events, "out", Kind::U32}},
         {"in"}},
        {"animation_event", 0x09,
         {{Role::Input, "event", Kind::String, true}, {Role::Input, "unit", Kind::Unit}, {Role::Events, "out", Kind::U32}},
         {"in"}},
        {"delay", 0x06, {{Role::Input, "time", Kind::Float, true}, {Role::Events, "out", Kind::U32}}, {"in"}},
        {"once", 0x13, {{Role::State, "", Kind::U32}, {Role::Events, "out", Kind::U32}}, {"in"}},
        {"gate", 0x14, {{Role::State, "start_open", Kind::Bool}, {Role::Events, "out", Kind::U32}},
         {"pass", "open", "close", "toggle"}},
        // logic (handlers 0x1403e5fb0, 0x1403e6400, 0x1403e5ce0, 0x1403d29a0)
        {"branch", 0x34, {{Role::Input, "condition", Kind::Bool}, {Role::Events, "true", Kind::U32},
                          {Role::Events, "false", Kind::U32}}, {"in"}},
        {"compare", 0x1f, {{Role::Input, "a", Kind::Float}, {Role::Input, "b", Kind::Float},
                           {Role::Events, "less", Kind::U32}, {Role::Events, "less_or_equal", Kind::U32},
                           {Role::Events, "equal", Kind::U32}, {Role::Events, "greater_or_equal", Kind::U32},
                           {Role::Events, "greater", Kind::U32}}, {"in"}},
        {"fork", 0x29, {{Role::Events, "out_1", Kind::U32}, {Role::Events, "out_2", Kind::U32},
                        {Role::Events, "out_3", Kind::U32}, {Role::Events, "out_4", Kind::U32},
                        {Role::Events, "out_5", Kind::U32}, {Role::Events, "out_6", Kind::U32},
                        {Role::Events, "out_7", Kind::U32}, {Role::Events, "out_8", Kind::U32}}, {"in"}},
        // first run: value = start; add / subtract step; reset to start
        {"counter", 0x1e, {{Role::Input, "start", Kind::Float}, {Role::Input, "step", Kind::Float},
                           {Role::Output, "value", Kind::Float}, {Role::State, "", Kind::U32},
                           {Role::Events, "out", Kind::U32}}, {"add", "subtract", "reset"}},
        // values (handlers 0x1403d2a30, 0x1403d2e80)
        {"random_float", 0x27, {{Role::Input, "min", Kind::Float}, {Role::Input, "max", Kind::Float},
                                {Role::Output, "value", Kind::Float}}, {}, false, true},
        {"vector3", 0x3b, {{Role::Input, "x", Kind::Float}, {Role::Input, "y", Kind::Float},
                           {Role::Input, "z", Kind::Float}, {Role::Output, "vector", Kind::Vector3}}, {}, false, true},
        // materials: a mesh of the unit by name, one of its materials by name, then set a variable (handlers
        // 0x1403de550, 0x1403d28b0, 0x1403d2680)
        {"get_mesh", 0xf1, {{Role::Input, "unit", Kind::Unit}, {Role::Input, "name", Kind::String, true},
                            {Role::Output, "mesh", Kind::Mesh}}, {}, false, true},
        {"get_material", 0xf2, {{Role::Input, "mesh", Kind::Mesh, true}, {Role::Input, "name", Kind::String, true},
                                {Role::Output, "material", Kind::Material}}, {}, false, true},
        {"set_material_variable", 0xf3,
         {{Role::Input, "material", Kind::Material, true}, {Role::Input, "variable", Kind::String, true},
          {Role::Input, "value", Kind::Float}, {Role::Input, "vector", Kind::Vector3}, {Role::Events, "out", Kind::U32}},
         {"in"}},
        // lights: one of the unit's lights by name, then set its intensity or color (handlers in the 10-06 exe
        // 0x1403de1d0, setters write the light and call the engine's light-changed function like Light.set_*;
        // the setters never fire their (empty) out list)
        {"get_light", 0xbb, {{Role::Input, "unit", Kind::Unit}, {Role::Input, "name", Kind::String, true},
                             {Role::Output, "light", Kind::Light}}, {}, false, true},
        {"set_light_intensity", 0xbc, {{Role::Input, "unit", Kind::Unit}, {Role::Input, "light", Kind::Light, true},
                                       {Role::Input, "intensity", Kind::Float, true}, {Role::Zero, "", Kind::U32}},
         {"in"}},
        {"set_light_color", 0xcf, {{Role::Input, "unit", Kind::Unit}, {Role::Input, "light", Kind::Light, true},
                                   {Role::Input, "color", Kind::Vector3, true}, {Role::Zero, "", Kind::U32}},
         {"in"}},
        // unit data (Unit.get_data values) by key (handlers 0x1403dd660, 0x1403dcff0)
        {"get_data_bool", 0x9d, {{Role::Input, "unit", Kind::Unit}, {Role::Unset, "", Kind::U32},
                                 {Role::Unset, "", Kind::U32}, {Role::Input, "key", Kind::String, true},
                                 {Role::Output, "value", Kind::Bool}}, {}, false, true},
        {"get_data_number", 0x9c, {{Role::Input, "unit", Kind::Unit}, {Role::Unset, "", Kind::U32},
                                   {Role::Unset, "", Kind::U32}, {Role::Input, "key", Kind::String, true},
                                   {Role::Output, "value", Kind::Float}}, {}, false, true},
    };
    return table;
}

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Unit: return "unit";
        case Kind::Bool: return "bool";
        case Kind::U32: return "uint";
        case Kind::Float: return "float";
        case Kind::Vector3: return "vector3";
        case Kind::Quaternion: return "quaternion";
        case Kind::String: return "string";
        case Kind::Name: return "name";
        case Kind::Mesh: return "mesh";
        case Kind::Material: return "material";
        case Kind::Light: return "light";
    }
    return "";
}

bool parse_kind(const std::string& text, Kind& kind) {
    for (const Kind candidate : {Kind::Unit, Kind::Bool, Kind::U32, Kind::Float, Kind::Vector3, Kind::Quaternion,
                                 Kind::String, Kind::Name})
        if (text == kind_name(candidate)) { kind = candidate; return true; }
    return false;
}

// The Lua flow node's value types (engine push_c_variable_to_lua): 1 unit, 4 vector3, 5 number, 6 bool, 7 string,
// 8 id64 (passed as its name), 9 quaternion, 10 integer.
std::uint32_t lua_type(Kind kind) {
    switch (kind) {
        case Kind::Unit: return 1;
        case Kind::Vector3: return 4;
        case Kind::Float: return 5;
        case Kind::Bool: return 6;
        case Kind::String: return 7;
        case Kind::Name: return 8;
        case Kind::Quaternion: return 9;
        case Kind::U32: return 10;
        case Kind::Mesh:
        case Kind::Material:
        case Kind::Light: return 0;   // not passed to Lua (parse_kind does not offer them)
    }
    return 0;
}

struct Dynamic {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(32, 0);   // the game's graphs keep 32 zero bytes first

    std::uint32_t allocate(std::size_t size, std::size_t alignment) {
        bytes.resize((bytes.size() + alignment - 1) / alignment * alignment, 0);
        const auto offset = static_cast<std::uint32_t>(bytes.size());
        bytes.resize(bytes.size() + size, 0);
        return offset;
    }
    void write(std::uint32_t offset, const void* data, std::size_t size) { std::memcpy(bytes.data() + offset, data, size); }
};

std::size_t kind_size(Kind kind) {
    switch (kind) {
        case Kind::Vector3: return 12;
        case Kind::Quaternion: return 16;
        case Kind::Name:
        case Kind::Mesh:
        case Kind::Material:
        case Kind::Light: return 8;   // id64, or an engine handle
        default: return 4;
    }
}

std::uint32_t new_slot(Dynamic& dynamic, Kind kind) {
    const auto offset = dynamic.allocate(kind_size(kind), kind_size(kind) == 8 ? 8 : 4);
    if (kind == Kind::Unit) { const std::uint32_t null_unit = 0x0001ffffu; dynamic.write(offset, &null_unit, 4); }
    return offset;
}

bool numbers(const json::Value& value, std::size_t count, std::array<float, 4>& out) {
    if (count == 1) {
        if (!value.is_number()) return false;
        out[0] = static_cast<float>(value.number);
        return std::isfinite(out[0]);
    }
    if (!value.is_array() || value.items.size() != count) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!value.items[i].is_number()) return false;
        out[i] = static_cast<float>(value.items[i].number);
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

// A constant input: its own slot holding the value.
bool constant_slot(Dynamic& dynamic, Kind kind, const json::Value& value, std::uint32_t& offset, std::string& error) {
    std::array<float, 4> v{};
    switch (kind) {
        case Kind::Bool: {
            if (value.kind != json::Value::Kind::Bool) { error = "expects true or false"; return false; }
            offset = new_slot(dynamic, kind);
            const std::uint32_t flag = value.boolean ? 1u : 0u;
            dynamic.write(offset, &flag, 4);
            return true;
        }
        case Kind::U32: {
            std::uint32_t number = 0;
            if (value.is_string() && value.string == "destroy") number = 0;
            else if (value.is_string() && value.string == "stop") number = 1;
            else if (value.is_string() && value.string == "unlink") number = 2;
            else if (value.is_number() && value.number >= 0 && value.number <= 4294967295.0 && value.number == std::floor(value.number))
                number = static_cast<std::uint32_t>(value.number);
            else { error = "expects a whole number"; return false; }
            offset = new_slot(dynamic, kind);
            dynamic.write(offset, &number, 4);
            return true;
        }
        case Kind::Float:
        case Kind::Vector3:
        case Kind::Quaternion: {
            const std::size_t count = kind == Kind::Float ? 1 : kind == Kind::Vector3 ? 3 : 4;
            if (!numbers(value, count, v)) { error = "expects " + std::string(count == 1 ? "a number" : count == 3 ? "3 numbers" : "4 numbers (x, y, z, w)"); return false; }
            offset = new_slot(dynamic, kind);
            dynamic.write(offset, v.data(), 4 * count);
            return true;
        }
        case Kind::String: {
            if (!value.is_string() || value.string.empty() || value.string.find('\0') != std::string::npos) { error = "expects a text"; return false; }
            offset = dynamic.allocate(value.string.size() + 1, 4);
            dynamic.write(offset, value.string.data(), value.string.size());
            return true;
        }
        case Kind::Name: {
            if (!value.is_string() || value.string.empty()) { error = "expects a name"; return false; }
            offset = new_slot(dynamic, kind);
            const std::uint64_t id = id64(value.string);
            dynamic.write(offset, &id, 8);
            return true;
        }
        case Kind::Unit:
        case Kind::Mesh:
        case Kind::Material:
        case Kind::Light:
            error = std::string("a ") + kind_name(kind) + " input takes a connection, not a value";
            return false;
    }
    return false;
}

const NodeSpec* find_spec(const std::string& name) {
    for (const auto& spec : specs()) if (name == spec.name) return &spec;
    return nullptr;
}

bool split(const json::Value& value, std::string& node, std::string& port) {
    if (!value.is_string()) return false;
    const auto dot = value.string.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == value.string.size()) return false;
    node = value.string.substr(0, dot);
    port = value.string.substr(dot + 1);
    return true;
}

json::Value number(std::uint32_t value) { return json::Value::of(static_cast<double>(value)); }

} // namespace

json::Value authoring_schema() {
    auto out = json::Value::object();
    for (const auto& spec : specs()) {
        auto node = json::Value::object();
        auto inputs = json::Value::object(), outputs = json::Value::object(), out_events = json::Value::array();
        for (const auto& slot : spec.layout) {
            if (slot.role == Role::Input || (slot.role == Role::State && *slot.name))
                inputs.set(slot.name, json::Value::of(std::string(kind_name(slot.kind))));
            if (slot.role == Role::Output) outputs.set(slot.name, json::Value::of(std::string(kind_name(slot.kind))));
            if (slot.role == Role::Events) out_events.push(json::Value::of(std::string(slot.name)));
        }
        auto in_events = json::Value::array();
        for (const auto* event : spec.in_events) in_events.push(json::Value::of(std::string(event)));
        node.set("inputs", std::move(inputs));
        node.set("outputs", std::move(outputs));
        node.set("in_events", std::move(in_events));
        node.set("out_events", std::move(out_events));
        out.set(spec.name, std::move(node));
    }
    // Lua call: inputs, outputs and output events are the node's own (see build_graph)
    auto lua = json::Value::object();
    lua.set("inputs", json::Value::object());
    lua.set("outputs", json::Value::object());
    auto in_events = json::Value::array();
    in_events.push(json::Value::of(std::string("in")));
    lua.set("in_events", std::move(in_events));
    auto out_events = json::Value::array();
    out_events.push(json::Value::of(std::string("out")));
    lua.set("out_events", std::move(out_events));
    lua.set("dynamic", json::Value::of(true));
    out.set("lua", std::move(lua));
    return out;
}

bool build_graph(const json::Value& authored, json::Value& graph, std::string& error) {
    const auto* node_list = authored.find("nodes");
    if (!authored.is_object() || !node_list || !node_list->is_array()) { error = "flow needs a nodes array"; return false; }

    struct Node {
        std::string id;
        const NodeSpec* spec = nullptr;
        const json::Value* source = nullptr;
        std::map<std::string, std::uint32_t> slots;   // input/output name -> dynamic data offset
        std::map<std::string, std::vector<std::uint32_t>> links;   // output event -> links
        std::vector<std::uint32_t> queries;   // getter nodes whose outputs this node reads
    };
    std::vector<Node> nodes;
    std::map<std::string, std::size_t> by_id;
    std::deque<NodeSpec> lua_specs;     // per Lua node: its own inputs, outputs and output events
    std::deque<std::string> lua_names;  // the names those specs point at
    for (const auto& item : node_list->items) {
        const auto* id = item.find("id");
        const auto* type = item.find("type");
        if (!id || !id->is_string() || id->string.empty() || id->string.find('.') != std::string::npos) { error = "every flow node needs an id (no dots)"; return false; }
        const bool lua = type && type->is_string() && type->string == "lua";
        if (!type || !type->is_string() || (!lua && !find_spec(type->string))) { error = "flow node '" + id->string + "' has an unknown type"; return false; }
        if (!by_id.emplace(id->string, nodes.size()).second) { error = "flow node id '" + id->string + "' is used twice"; return false; }
        if (!lua) { nodes.push_back({id->string, find_spec(type->string), &item, {}, {}}); continue; }
        // {"type": "lua", "function": "name", "class": "FlowCallbacks", "params": {"name": "kind"}, "returns": {...},
        //  "events": ["out", ...]}: calls Class.function(params); an output event fires when the returned table sets it
        //  true ("out" fires unless it is set false)
        const auto where = "Lua flow node '" + id->string + "'";
        const auto* function = item.find("function");
        if (!function || !function->is_string() || function->string.empty() || function->string.find('\0') != std::string::npos) { error = where + " needs a function name"; return false; }
        if (const auto* klass = item.find("class"); klass && (!klass->is_string() || klass->string.find('\0') != std::string::npos)) { error = where + " class must be a name"; return false; }
        NodeSpec spec{"lua", 0x11, {}, {"in"}};
        std::set<std::string> names;
        const auto add_slots = [&](const char* key, Role role) {
            const auto* list = item.find(key);
            if (!list) return true;
            if (!list->is_object()) { error = where + " " + key + " must map names to kinds"; return false; }
            for (const auto& [name, kind_text] : list->members) {
                Kind kind;
                if (name.empty() || name == "node_id" || name.find('\0') != std::string::npos || name.find('.') != std::string::npos) { error = where + " has an invalid name '" + name + "' (node_id is set by the game)"; return false; }
                if (!kind_text.is_string() || !parse_kind(kind_text.string, kind)) { error = where + " " + name + ": kind must be unit, bool, uint, float, vector3, quaternion, string or name"; return false; }
                if (!names.insert(name).second) { error = where + " uses '" + name + "' twice"; return false; }
                spec.layout.push_back({role, lua_names.emplace_back(name).c_str(), kind});
            }
            return true;
        };
        if (!add_slots("params", Role::Input) || !add_slots("returns", Role::Output)) return false;
        const auto* events = item.find("events");
        if (events && (!events->is_array() || events->items.empty())) { error = where + " events must be a list of names"; return false; }
        for (const auto& event : events ? events->items : std::vector<json::Value>{json::Value::of(std::string("out"))}) {
            if (!event.is_string() || event.string.empty() || event.string.find('\0') != std::string::npos || !names.insert(event.string).second) { error = where + " has an invalid or repeated event name"; return false; }
            spec.layout.push_back({Role::Events, lua_names.emplace_back(event.string).c_str(), Kind::U32});
        }
        nodes.push_back({id->string, &lua_specs.emplace_back(std::move(spec)), &item, {}, {}});
    }
    // the unit itself: the unit_spawned node's output, which every open unit input reads
    std::size_t spawned = nodes.size();
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (std::string(nodes[i].spec->name) == "unit_spawned") {
            if (spawned != nodes.size()) { error = "a flow graph has one unit_spawned node"; return false; }
            spawned = i;
        }
    static const json::Value no_inputs = json::Value::object();
    if (spawned == nodes.size()) {
        by_id.emplace("unit_spawned", nodes.size());
        nodes.push_back({"unit_spawned", find_spec("unit_spawned"), &no_inputs, {}, {}});
    }

    Dynamic dynamic;
    // outputs and internal state first, so connections can point at them
    for (auto& node : nodes) {
        for (const auto& slot : node.spec->layout) {
            if (slot.role == Role::Output) node.slots[slot.name] = new_slot(dynamic, slot.kind);
            if (slot.role == Role::State) {
                const auto offset = new_slot(dynamic, slot.kind);
                node.slots[std::string("#state") + slot.name] = offset;
                if (*slot.name) {   // gate: initial open state
                    const auto* inputs = node.source->find("inputs");
                    const auto* value = inputs ? inputs->find(slot.name) : nullptr;
                    if (value) {
                        if (value->kind != json::Value::Kind::Bool) { error = "flow node '" + node.id + "' " + slot.name + " expects true or false"; return false; }
                        const std::uint32_t flag = value->boolean ? 1u : 0u;
                        dynamic.write(offset, &flag, 4);
                    }
                }
            }
        }
    }
    // Lua nodes: a 4-byte slot the call gets as its second argument and the cached function reference per input
    // event (Lua's LUA_NOREF -2 until the first call), as in the game's graphs
    for (auto& node : nodes) {
        if (node.spec->type != 0x11) continue;
        node.slots["#lua_data"] = new_slot(dynamic, Kind::U32);
        const auto reference = dynamic.allocate(8, 4);
        const std::uint32_t no_reference = 0xfffffffeu;
        dynamic.write(reference, &no_reference, 4);
        node.slots["#lua_reference"] = reference;
    }
    const std::uint32_t own_unit = nodes[spawned].slots["unit"];

    // data connections: an input shares its source output's slot
    if (const auto* connections = authored.find("connections")) {
        if (!connections->is_array()) { error = "flow connections must be an array"; return false; }
        for (const auto& pair : connections->items) {
            std::string from_node, from_port, to_node, to_port;
            if (!pair.is_array() || pair.items.size() != 2 || !split(pair.items[0], from_node, from_port) || !split(pair.items[1], to_node, to_port) ||
                !by_id.count(from_node) || !by_id.count(to_node)) {
                error = "flow connection entries are [\"node.output\", \"node.input\"] between existing nodes"; return false;
            }
            auto& from = nodes[by_id[from_node]];
            auto& to = nodes[by_id[to_node]];
            const Slot* output = nullptr; const Slot* input = nullptr;
            for (const auto& slot : from.spec->layout) if (slot.role == Role::Output && from_port == slot.name) output = &slot;
            for (const auto& slot : to.spec->layout) if (slot.role == Role::Input && to_port == slot.name) input = &slot;
            if (!output || !input) { error = "flow connection " + from_node + "." + from_port + " -> " + to_node + "." + to_port + " names a missing output or input"; return false; }
            if (output->kind != input->kind) { error = "flow connection " + from_node + "." + from_port + " -> " + to_node + "." + to_port + " joins a " + kind_name(output->kind) + " to a " + kind_name(input->kind); return false; }
            if (to.slots.count(to_port)) { error = "flow input " + to_node + "." + to_port + " is connected twice"; return false; }
            to.slots[to_port] = from.slots[from_port];
            const auto from_index = static_cast<std::uint32_t>(by_id[from_node]);
            if (from.spec->getter && std::find(to.queries.begin(), to.queries.end(), from_index) == to.queries.end())
                to.queries.push_back(from_index);
        }
    }
    // constants and open unit inputs
    for (auto& node : nodes) {
        const auto* inputs = node.source->find("inputs");
        if (inputs && !inputs->is_object()) { error = "flow node '" + node.id + "' inputs must be an object"; return false; }
        if (inputs)
            for (const auto& [name, value] : inputs->members) {
                const Slot* slot = nullptr;
                for (const auto& candidate : node.spec->layout)
                    if ((candidate.role == Role::Input || candidate.role == Role::State) && name == candidate.name) slot = &candidate;
                if (!slot) { error = "flow node '" + node.id + "' (" + node.spec->name + ") has no input '" + name + "'"; return false; }
                if (slot->role == Role::State) continue;
                if (node.slots.count(name)) { error = "flow input " + node.id + "." + name + " has both a value and a connection"; return false; }
                std::uint32_t offset;
                if (!constant_slot(dynamic, slot->kind, value, offset, error)) { error = "flow input " + node.id + "." + name + " " + error; return false; }
                node.slots[name] = offset;
            }
        for (const auto& slot : node.spec->layout) {
            if (slot.role != Role::Input || node.slots.count(slot.name)) continue;
            if (slot.kind == Kind::Unit) node.slots[slot.name] = own_unit;
            else if (slot.required && slot.kind == Kind::Bool) {
                std::uint32_t offset; constant_slot(dynamic, Kind::Bool, json::Value::of(true), offset, error);
                node.slots[slot.name] = offset;
            } else if (slot.required) {
                error = "flow node '" + node.id + "' (" + node.spec->name + ") needs '" + slot.name + "'"; return false;
            }
        }
    }
    // event links
    if (const auto* links = authored.find("links")) {
        if (!links->is_array()) { error = "flow links must be an array"; return false; }
        for (const auto& pair : links->items) {
            std::string from_node, from_event, to_node, to_event;
            if (!pair.is_array() || pair.items.size() != 2 || !split(pair.items[0], from_node, from_event) || !split(pair.items[1], to_node, to_event) ||
                !by_id.count(from_node) || !by_id.count(to_node)) {
                error = "flow link entries are [\"node.event\", \"node.event\"] between existing nodes"; return false;
            }
            auto& from = nodes[by_id[from_node]];
            const auto& to = nodes[by_id[to_node]];
            bool has_out = false;
            for (const auto& slot : from.spec->layout) if (slot.role == Role::Events && from_event == slot.name) has_out = true;
            const auto in = std::find_if(to.spec->in_events.begin(), to.spec->in_events.end(), [&](const char* e) { return to_event == e; });
            if (!has_out || in == to.spec->in_events.end()) { error = "flow link " + from_node + "." + from_event + " -> " + to_node + "." + to_event + " names a missing event"; return false; }
            from.links[from_event].push_back(static_cast<std::uint32_t>(by_id[to_node]) |
                                            (static_cast<std::uint32_t>(in - to.spec->in_events.begin()) << 16));
        }
    }

    graph = json::Value::object();
    auto events = std::vector<std::pair<std::uint32_t, std::uint32_t>>{{id32_from_id64("unit_spawned"), static_cast<std::uint32_t>(spawned)}};
    auto node_out = json::Value::array();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (std::string(node.spec->name) == "unit_unspawned") {
            if (std::any_of(events.begin(), events.end(), [](const auto& e) { return e.first == id32_from_id64("unit_unspawned"); })) {
                error = "a flow graph has one unit_unspawned node";
                return false;
            }
            events.push_back({id32_from_id64("unit_unspawned"), static_cast<std::uint32_t>(i)});
        }
        if (std::string(node.spec->name) == "external_event") {
            const auto* name = node.source->find("event");
            if (!name || !name->is_string() || name->string.empty()) { error = "external_event node '" + node.id + "' needs an event name"; return false; }
            events.push_back({id32_from_id64(name->string), static_cast<std::uint32_t>(i)});
        }
        auto words = json::Value::array();
        auto record_offsets = json::Value::array();
        if (node.spec->type == 0x11) {
            // [node id, 0, data slot, reference slot, input events 1, class/function, inputs (count, offset),
            //  outputs (count, offset), output events (count, offset), the events' link lists] then the strings
            //  (offsets into the record here, rebased by the codec): 1 class\0 function\0 | inputs {type, slot,
            //  name\0} each padded to 4 | outputs {type, slot, name\0} unpadded | event names\0
            std::vector<std::uint8_t> text;
            const auto pad4 = [&] { text.resize((text.size() + 3) / 4 * 4, 0); };
            const auto put_u32 = [&](std::uint32_t v) { const auto* b = reinterpret_cast<const std::uint8_t*>(&v); text.insert(text.end(), b, b + 4); };
            const auto put_text = [&](const std::string& s) { text.insert(text.end(), s.begin(), s.end()); text.push_back(0); };
            const auto* klass = node.source->find("class");
            text.push_back(1);
            put_text(klass && !klass->string.empty() ? klass->string : "FlowCallbacks");
            put_text(node.source->find("function")->string);
            pad4();
            std::uint32_t inputs = 0, outputs = 0, events = 0;
            const auto inputs_at = static_cast<std::uint32_t>(text.size());
            for (const auto& slot : node.spec->layout) {
                if (slot.role != Role::Input) continue;
                const auto found = node.slots.find(slot.name);
                put_u32(lua_type(slot.kind));
                put_u32(found == node.slots.end() ? 0xffffffffu : found->second);
                put_text(slot.name);
                pad4();
                ++inputs;
            }
            const auto outputs_at = static_cast<std::uint32_t>(text.size());
            for (const auto& slot : node.spec->layout) {
                if (slot.role != Role::Output) continue;
                put_u32(lua_type(slot.kind));
                put_u32(node.slots.at(slot.name));
                put_text(slot.name);
                ++outputs;
            }
            pad4();
            const auto events_at = static_cast<std::uint32_t>(text.size());
            std::vector<std::uint32_t> lists;
            for (const auto& slot : node.spec->layout) {
                if (slot.role != Role::Events) continue;
                put_text(slot.name);
                ++events;
                const auto found = node.links.find(slot.name);
                const auto& list = found == node.links.end() ? std::vector<std::uint32_t>{} : found->second;
                lists.push_back(static_cast<std::uint32_t>(list.size()));
                lists.insert(lists.end(), list.begin(), list.end());
            }
            pad4();
            const auto strings = static_cast<std::uint32_t>(8 + 4 * (12 + lists.size()));
            for (const std::uint32_t word : {id32_from_id64(node.id), 0u, node.slots.at("#lua_data"), node.slots.at("#lua_reference"),
                                             1u, strings, inputs, strings + inputs_at, outputs, strings + outputs_at, events,
                                             strings + events_at})
                words.push(number(word));
            for (const std::uint32_t index : {5u, 7u, 9u, 11u}) record_offsets.push(number(index));
            for (const auto word : lists) words.push(number(word));
            for (std::size_t k = 0; k < text.size(); k += 4) {
                std::uint32_t word; std::memcpy(&word, text.data() + k, 4); words.push(number(word));
            }
        }
        for (const auto& slot : node.spec->type == 0x11 ? std::vector<Slot>{} : node.spec->layout) {
            switch (slot.role) {
                case Role::Input:
                case Role::Output: {
                    const auto found = node.slots.find(slot.name);
                    words.push(number(found == node.slots.end() ? 0xffffffffu : found->second));
                    break;
                }
                case Role::State: words.push(number(node.slots.at(std::string("#state") + slot.name))); break;
                case Role::Unset: words.push(number(0xffffffffu)); break;
                case Role::Zero: words.push(number(0)); break;
                case Role::Events: {
                    const auto found = node.links.find(slot.name);
                    const auto& list = found == node.links.end() ? std::vector<std::uint32_t>{} : found->second;
                    words.push(number(static_cast<std::uint32_t>(list.size())));
                    for (const auto link : list) words.push(number(link));
                    break;
                }
            }
        }
        // query list after the node's own words, its offset into the record (the codec rebases it)
        std::uint32_t query = 0;
        if (!node.queries.empty()) {
            query = static_cast<std::uint32_t>(8 + 4 * words.items.size());
            words.push(number(static_cast<std::uint32_t>(node.queries.size())));
            for (const auto getter : node.queries) words.push(number(getter | 0xffff0000u));
        }
        if (words.items.size() % 2) words.push(number(0));   // records stay 8-byte aligned, as in the game's graphs
        auto record = json::Value::object();
        record.set("type", number(node.spec->type));
        record.set("query", number(query));
        if (query) record.set("query_in_record", json::Value::of(true));
        record.set("words", std::move(words));
        if (!record_offsets.items.empty()) record.set("record_offsets", std::move(record_offsets));
        node_out.push(std::move(record));
    }
    // the engine binary-searches the external events by id
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto external = json::Value::array();
    for (const auto& [id, node] : events) {
        char text[12]; std::snprintf(text, sizeof(text), "#%08x", id);
        auto event = json::Value::object();
        event.set("event", json::Value::of(std::string(text)));
        event.set("node", number(node));
        event.set("in", number(0));
        external.push(std::move(event));
    }
    dynamic.bytes.resize((dynamic.bytes.size() + 7) / 8 * 8, 0);
    graph.set("dynamic_size", number(static_cast<std::uint32_t>(dynamic.bytes.size())));
    graph.set("external_events", std::move(external));
    auto empty_lists = json::Value::array();
    for (int k = 0; k < 4; ++k) empty_lists.push(json::Value::array());
    graph.set("lists", std::move(empty_lists));
    graph.set("nodes", std::move(node_out));
    std::string hex;
    for (const auto byte : dynamic.bytes) { char pair[3]; std::snprintf(pair, sizeof(pair), "%02x", byte); hex += pair; }
    graph.set("dynamic_data", json::Value::of(hex));
    return true;
}

} // namespace dtglb::stingray::flow
