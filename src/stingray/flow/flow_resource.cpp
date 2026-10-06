#include "stingray/flow/flow_resource.h"

#include "stingray/murmur_hash.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>

namespace dtglb::stingray::flow {
namespace {

// Layout (all retail units): a 0x5c-byte header of u32 words, then
//   node index (u32 offset per node) | external events {id32, node | in << 16} | extra pairs {u32, u32} |
//   external variables {u32, u32, u32} | query {count, u32...} [pad 8] | preload deps {u32, u32} |
//   subroutines (u32 node index) | 4 u32 lists [pad 8] | nodes (8-aligned, each {type, query offset, payload}).
// Header: 0, nodes, node index offset, events, events offset, extra pairs, variables, variables offset, query
// offset, preload deps, preload offset, subroutines, subroutines offset, 4 list counts, 4 list offsets, size,
// dynamic data size.
constexpr std::size_t kHeaderWords = 23;
constexpr std::size_t kHeaderSize = kHeaderWords * 4;

std::size_t align8(std::size_t value) { return (value + 7) & ~std::size_t{7}; }

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

void pad8(std::vector<std::uint8_t>& out) { out.resize(align8(out.size()), 0); }

std::string hex32(std::uint32_t value) {
    char text[12]; std::snprintf(text, sizeof(text), "#%08x", value); return text;
}

// Event names the game's units use, so descriptions read as names.
const std::map<std::uint32_t, std::string>& event_names() {
    static const std::map<std::uint32_t, std::string> names = [] {
        std::map<std::uint32_t, std::string> out;
        for (const char* name : {"unit_spawned", "unit_object_set_enabled", "unit_object_set_disabled", "create_particle",
                                 "destroy_particle", "enable", "disable", "interactable_enable", "interactable_disable",
                                 "set_barrel_overheat"})
            out.emplace(id32_from_id64(name), name);
        return out;
    }();
    return names;
}

json::Value number(std::uint32_t value) { return json::Value::of(static_cast<double>(value)); }

json::Value words(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count) {
    auto out = json::Value::array();
    for (std::size_t i = 0; i < count; ++i) out.push(number(word(bytes, offset + 4 * i)));
    return out;
}

bool read_u32(const json::Value& value, std::uint32_t& out) {
    if (!value.is_number() || value.number < 0 || value.number > 4294967295.0 ||
        value.number != static_cast<double>(static_cast<std::uint32_t>(value.number))) return false;
    out = static_cast<std::uint32_t>(value.number);
    return true;
}

bool read_words(const json::Value* value, std::size_t group, std::vector<std::uint32_t>& out, const char* what,
                std::string& error) {
    out.clear();
    if (!value) return true;
    if (!value->is_array()) { error = std::string(what) + " must be an array"; return false; }
    for (const auto& item : value->items) {
        const auto& list = group ? item.items : std::vector<json::Value>{item};
        if (group && (!item.is_array() || item.items.size() != group)) {
            error = std::string(what) + " entries must have " + std::to_string(group) + " numbers"; return false;
        }
        for (const auto& entry : list) {
            std::uint32_t v;
            if (!read_u32(entry, v)) { error = std::string(what) + " holds a value that is not a u32"; return false; }
            out.push_back(v);
        }
    }
    return true;
}

bool parse_hex(const std::string& text, std::vector<std::uint8_t>& out) {
    if (text.size() % 2) return false;
    out.clear();
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0;
        if (std::sscanf(text.c_str() + i, "%2x", &value) != 1) return false;
        out.push_back(static_cast<std::uint8_t>(value));
    }
    return true;
}

} // namespace

bool decode(const std::vector<std::uint8_t>& flow, const std::vector<std::uint8_t>& dynamic_data, json::Value& graph,
            std::string& error) {
    if (flow.size() < kHeaderSize) { error = "flow blob is shorter than its header"; return false; }
    std::array<std::uint32_t, kHeaderWords> h{};
    for (std::size_t i = 0; i < kHeaderWords; ++i) h[i] = word(flow, 4 * i);
    const auto inside = [&](std::size_t offset, std::size_t size) { return offset <= flow.size() && size <= flow.size() - offset; };
    if (h[0] != 0 || h[21] != flow.size()) { error = "flow header does not describe this blob"; return false; }
    const std::uint32_t nodes = h[1], events = h[3], extra = h[5], variables = h[6], preload = h[9], subroutines = h[11];
    if (!inside(h[2], 4ull * nodes) || !inside(h[4], 8ull * (events + extra)) || !inside(h[7], 12ull * variables) ||
        !inside(h[8], 4) || !inside(h[10], 8ull * preload) || !inside(h[12], 4ull * subroutines)) {
        error = "flow table lies outside the blob"; return false;
    }
    const std::uint32_t query = word(flow, h[8]);
    if (!inside(h[8] + 4, 4ull * query)) { error = "flow query table lies outside the blob"; return false; }

    graph = json::Value::object();
    graph.set("dynamic_size", number(h[22]));
    auto external = json::Value::array();
    for (std::uint32_t i = 0; i < events; ++i) {
        const auto id = word(flow, h[4] + 8 * i), target = word(flow, h[4] + 8 * i + 4);
        const auto name = event_names().find(id);
        auto event = json::Value::object();
        event.set("event", json::Value::of(name != event_names().end() ? name->second : hex32(id)));
        event.set("node", number(target & 0xffffu));
        event.set("in", number(target >> 16));
        external.push(std::move(event));
    }
    graph.set("external_events", std::move(external));
    auto pairs = json::Value::array();
    for (std::uint32_t i = 0; i < extra; ++i) pairs.push(words(flow, h[4] + 8 * events + 8 * i, 2));
    graph.set("extra", std::move(pairs));
    auto vars = json::Value::array();
    for (std::uint32_t i = 0; i < variables; ++i) vars.push(words(flow, h[7] + 12 * i, 3));
    graph.set("variables", std::move(vars));
    graph.set("query", words(flow, h[8] + 4, query));
    auto deps = json::Value::array();
    for (std::uint32_t i = 0; i < preload; ++i) deps.push(words(flow, h[10] + 8 * i, 2));
    graph.set("preload", std::move(deps));
    graph.set("subroutines", words(flow, h[12], subroutines));
    auto lists = json::Value::array();
    for (int k = 0; k < 4; ++k) {
        if (!inside(h[17 + k], 4ull * h[13 + k])) { error = "flow list lies outside the blob"; return false; }
        lists.push(words(flow, h[17 + k], h[13 + k]));
    }
    graph.set("lists", std::move(lists));
    auto node_list = json::Value::array();
    for (std::uint32_t i = 0; i < nodes; ++i) {
        const std::size_t start = word(flow, h[2] + 4 * i);
        const std::size_t end = i + 1 < nodes ? word(flow, h[2] + 4 * (i + 1)) : flow.size();
        auto node = json::Value::object();
        if (end == start) {   // an index entry without a record of its own (328 in the game's units)
            node.set("empty", json::Value::of(true));
            node_list.push(std::move(node));
            continue;
        }
        if (end < start + 8 || end > flow.size() || (end - start) % 4) { error = "flow node " + std::to_string(i) + " has no valid extent"; return false; }
        node.set("type", number(word(flow, start)));
        node.set("query", number(word(flow, start + 4)));
        node.set("words", words(flow, start + 8, (end - start - 8) / 4));
        node_list.push(std::move(node));
    }
    graph.set("nodes", std::move(node_list));
    std::string hex;
    hex.reserve(dynamic_data.size() * 2);
    for (const auto byte : dynamic_data) { char pair[3]; std::snprintf(pair, sizeof(pair), "%02x", byte); hex += pair; }
    graph.set("dynamic_data", json::Value::of(hex));
    return true;
}

bool encode(const json::Value& graph, std::vector<std::uint8_t>& flow, std::vector<std::uint8_t>& dynamic_data,
            std::string& error) {
    if (!graph.is_object()) { error = "flow description must be an object"; return false; }
    const auto* nodes = graph.find("nodes");
    if (!nodes || !nodes->is_array() || nodes->items.size() > 0xffff) { error = "flow description needs a nodes array"; return false; }
    std::uint32_t dynamic_size = 0;
    const auto* size = graph.find("dynamic_size");
    if (!size || !read_u32(*size, dynamic_size)) { error = "flow dynamic_size must be a u32"; return false; }
    const auto* data = graph.find("dynamic_data");
    if (!data || !data->is_string() || !parse_hex(data->string, dynamic_data)) { error = "flow dynamic_data must be hex"; return false; }
    if (dynamic_data.size() < dynamic_size) { error = "flow dynamic_data is shorter than dynamic_size"; return false; }

    std::vector<std::uint32_t> events;
    if (const auto* list = graph.find("external_events")) {
        if (!list->is_array()) { error = "external_events must be an array"; return false; }
        for (const auto& event : list->items) {
            const auto* name = event.find("event");
            const auto* node = event.find("node");
            const auto* in = event.find("in");
            std::uint32_t node_index, in_index, id;
            if (!name || !name->is_string() || name->string.empty() || !node || !read_u32(*node, node_index) ||
                node_index >= nodes->items.size() || !in || !read_u32(*in, in_index) || in_index > 0xffff) {
                error = "external event needs event, node (an existing node) and in"; return false;
            }
            if (name->string[0] == '#') {
                if (name->string.size() != 9 || std::sscanf(name->string.c_str() + 1, "%8x", &id) != 1) { error = "external event id must be #<8 hex>"; return false; }
            } else {
                id = id32_from_id64(name->string);
            }
            events.push_back(id);
            events.push_back(node_index | (in_index << 16));
        }
    }
    std::vector<std::uint32_t> extra, variables, query, preload, subroutines;
    if (!read_words(graph.find("extra"), 2, extra, "extra", error) ||
        !read_words(graph.find("variables"), 3, variables, "variables", error) ||
        !read_words(graph.find("query"), 0, query, "query", error) ||
        !read_words(graph.find("preload"), 2, preload, "preload", error) ||
        !read_words(graph.find("subroutines"), 0, subroutines, "subroutines", error)) return false;
    std::array<std::vector<std::uint32_t>, 4> lists;
    if (const auto* value = graph.find("lists")) {
        if (!value->is_array() || value->items.size() != 4) { error = "lists must hold 4 arrays"; return false; }
        for (int k = 0; k < 4; ++k) if (!read_words(&value->items[k], 0, lists[k], "lists", error)) return false;
    }

    flow.assign(kHeaderSize, 0);
    const auto node_index = flow.size();
    flow.resize(flow.size() + 4 * nodes->items.size(), 0);
    const auto events_offset = flow.size();
    for (const auto v : events) put(flow, v);
    for (const auto v : extra) put(flow, v);
    const auto variables_offset = flow.size();
    for (const auto v : variables) put(flow, v);
    const auto query_offset = flow.size();
    put(flow, static_cast<std::uint32_t>(query.size()));
    for (const auto v : query) put(flow, v);
    pad8(flow);
    const auto preload_offset = flow.size();
    for (const auto v : preload) put(flow, v);
    const auto subroutines_offset = flow.size();
    for (const auto v : subroutines) put(flow, v);
    std::array<std::size_t, 4> list_offsets{};
    for (int k = 0; k < 4; ++k) { list_offsets[k] = flow.size(); for (const auto v : lists[k]) put(flow, v); }
    pad8(flow);
    for (std::size_t i = 0; i < nodes->items.size(); ++i) {
        const auto& node = nodes->items[i];
        if (const auto* empty = node.find("empty"); empty && empty->kind == json::Value::Kind::Bool && empty->boolean) {
            const auto offset = static_cast<std::uint32_t>(flow.size());
            std::memcpy(flow.data() + node_index + 4 * i, &offset, 4);
            continue;
        }
        std::uint32_t type, query_node = 0;
        const auto* type_value = node.find("type");
        const auto* query_value = node.find("query");
        std::vector<std::uint32_t> payload;
        if (!type_value || !read_u32(*type_value, type) || (query_value && !read_u32(*query_value, query_node)) ||
            !read_words(node.find("words"), 0, payload, "node words", error)) {
            if (error.empty()) error = "flow node " + std::to_string(i) + " needs a type";
            return false;
        }
        pad8(flow);   // authored payloads of odd length; the game's carry their padding as words
        const auto offset = static_cast<std::uint32_t>(flow.size());
        std::memcpy(flow.data() + node_index + 4 * i, &offset, 4);
        // authored words holding offsets into the node's own record (Lua node strings); the game stores them as
        // offsets into the whole flow resource
        std::vector<std::uint32_t> relative;
        if (!read_words(node.find("record_offsets"), 0, relative, "record_offsets", error)) return false;
        for (const auto index : relative) {
            if (index >= payload.size()) { error = "flow node " + std::to_string(i) + " record_offsets names a missing word"; return false; }
            payload[index] += offset;
        }
        // authored query lists sit at the end of the node's own payload, their offset given into the record
        if (const auto* in_record = node.find("query_in_record"); in_record && in_record->kind == json::Value::Kind::Bool &&
            in_record->boolean && query_node) query_node += offset;
        put(flow, type);
        put(flow, query_node);
        for (const auto v : payload) put(flow, v);
    }
    const std::array<std::uint32_t, kHeaderWords> header{
        0, static_cast<std::uint32_t>(nodes->items.size()), static_cast<std::uint32_t>(node_index),
        static_cast<std::uint32_t>(events.size() / 2), static_cast<std::uint32_t>(events_offset),
        static_cast<std::uint32_t>(extra.size() / 2), static_cast<std::uint32_t>(variables.size() / 3),
        static_cast<std::uint32_t>(variables_offset), static_cast<std::uint32_t>(query_offset),
        static_cast<std::uint32_t>(preload.size() / 2), static_cast<std::uint32_t>(preload_offset),
        static_cast<std::uint32_t>(subroutines.size()), static_cast<std::uint32_t>(subroutines_offset),
        static_cast<std::uint32_t>(lists[0].size()), static_cast<std::uint32_t>(lists[1].size()),
        static_cast<std::uint32_t>(lists[2].size()), static_cast<std::uint32_t>(lists[3].size()),
        static_cast<std::uint32_t>(list_offsets[0]), static_cast<std::uint32_t>(list_offsets[1]),
        static_cast<std::uint32_t>(list_offsets[2]), static_cast<std::uint32_t>(list_offsets[3]),
        static_cast<std::uint32_t>(flow.size()), dynamic_size};
    std::memcpy(flow.data(), header.data(), kHeaderSize);
    return true;
}

} // namespace dtglb::stingray::flow
