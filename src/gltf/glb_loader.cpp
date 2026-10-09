#include "gltf/glb_loader.h"
#include "gltf/gltf_images.h"
#include "gltf/gltf_meshes.h"
#include "compiler/asset_definition_resolver.h"
#include "compiler/resource_key.h"
#include "stingray/texture/texture_writer.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dtglb::gltf {
namespace {

constexpr float kSqrtHalf = 0.70710678118654752440f;

// Small JSON object scanner used only for the deliberately narrow physics
// inventory.  cgltf owns the full JSON validation; this scanner preserves
// exact key/path semantics without substring matching.
struct JsonSlice { std::string_view text; std::size_t begin = 0, end = 0; };
bool json_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::size_t json_string_end(std::string_view s, std::size_t p) {
    if (p >= s.size() || s[p] != '"') return std::string_view::npos;
    for (++p; p < s.size(); ++p) { if (s[p] == '\\') { ++p; continue; } if (s[p] == '"') return p + 1; }
    return std::string_view::npos;
}
int json_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool json_key_equals(std::string_view encoded, std::string_view wanted) {
    std::size_t output = 0;
    for (std::size_t i = 0; i < encoded.size();) {
        unsigned value = static_cast<unsigned char>(encoded[i++]);
        if (value == '\\') {
            if (i >= encoded.size()) return false;
            const char escape = encoded[i++];
            switch (escape) {
                case '"': value = '"'; break; case '\\': value = '\\'; break; case '/': value = '/'; break;
                case 'b': value = '\b'; break; case 'f': value = '\f'; break; case 'n': value = '\n'; break;
                case 'r': value = '\r'; break; case 't': value = '\t'; break;
                case 'u': {
                    if (i + 4 > encoded.size()) return false;
                    value = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const int hex = json_hex(encoded[i++]);
                        if (hex < 0) return false;
                        value = value * 16u + static_cast<unsigned>(hex);
                    }
                    break;
                }
                default: return false;
            }
        }
        if (value > 0x7fu || output >= wanted.size() || wanted[output++] != static_cast<char>(value)) return false;
    }
    return output == wanted.size();
}
std::size_t json_value_end(std::string_view s, std::size_t p) {
    while (p < s.size() && json_ws(s[p])) ++p;
    if (p >= s.size()) return std::string_view::npos;
    if (s[p] == '"') return json_string_end(s, p);
    if (s[p] == '{' || s[p] == '[') {
        const char open = s[p], close = open == '{' ? '}' : ']'; int depth = 0; bool str = false;
        for (std::size_t i = p; i < s.size(); ++i) { const char c = s[i];
            if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
            if (c == '"') str = true; else if (c == open) ++depth; else if (c == close && --depth == 0) return i + 1;
        }
        return std::string_view::npos;
    }
    std::size_t e = p; while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' && !json_ws(s[e])) ++e; return e;
}
bool json_unescape_string(std::string_view encoded, std::string& output) {
    output.clear();
    auto append_utf8 = [&output](unsigned value) {
        if (value <= 0x7f) output.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) { output.push_back(static_cast<char>(0xc0 | (value >> 6))); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
        else if (value <= 0xffff) { output.push_back(static_cast<char>(0xe0 | (value >> 12))); output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
        else { output.push_back(static_cast<char>(0xf0 | (value >> 18))); output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f))); output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
    };
    for (std::size_t i = 0; i < encoded.size();) {
        const unsigned char value = static_cast<unsigned char>(encoded[i++]);
        if (value != '\\') { if (value < 0x20) return false; output.push_back(static_cast<char>(value)); continue; }
        if (i >= encoded.size()) return false;
        const char escape = encoded[i++];
        switch (escape) {
            case '"': output.push_back('"'); break; case '\\': output.push_back('\\'); break; case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break; case 'f': output.push_back('\f'); break; case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break; case 't': output.push_back('\t'); break;
            case 'u': {
                if (i + 4 > encoded.size()) return false;
                unsigned value16 = 0;
                for (int digit = 0; digit < 4; ++digit) { const int hex = json_hex(encoded[i++]); if (hex < 0) return false; value16 = value16 * 16u + static_cast<unsigned>(hex); }
                unsigned codepoint = value16;
                if (value16 >= 0xd800 && value16 <= 0xdbff) {
                    if (i + 6 > encoded.size() || encoded[i++] != '\\' || encoded[i++] != 'u') return false;
                    unsigned low = 0;
                    for (int digit = 0; digit < 4; ++digit) { const int hex = json_hex(encoded[i++]); if (hex < 0) return false; low = low * 16u + static_cast<unsigned>(hex); }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    codepoint = 0x10000u + ((value16 - 0xd800u) << 10) + (low - 0xdc00u);
                } else if (value16 >= 0xdc00 && value16 <= 0xdfff) return false;
                append_utf8(codepoint);
                break;
            }
            default: return false;
        }
    }
    return true;
}
bool json_object_key(std::string_view object, std::string_view wanted, JsonSlice& value) {
    std::size_t p = 0; while (p < object.size() && json_ws(object[p])) ++p;
    if (p >= object.size() || object[p] != '{') return false; ++p;
    while (p < object.size()) {
        while (p < object.size() && (json_ws(object[p]) || object[p] == ',')) ++p;
        if (p < object.size() && object[p] == '}') return false;
        const auto ke = json_string_end(object, p); if (ke == std::string_view::npos) return false;
        const auto key = object.substr(p + 1, ke - p - 2); p = ke;
        while (p < object.size() && json_ws(object[p])) ++p; if (p >= object.size() || object[p++] != ':') return false;
        const auto ve = json_value_end(object, p); if (ve == std::string_view::npos) return false;
        if (json_key_equals(key, wanted)) { value = {object, p, ve}; return true; } p = ve;
    }
    return false;
}
bool json_object_enabled_false(std::string_view object) {
    JsonSlice value; if (!json_object_key(object, "enabled", value)) return false;
    std::size_t p = value.begin; while (p < value.end && json_ws(object[p])) ++p;
    return object.substr(p, value.end - p) == "false";
}

using AuthoredMembers = std::map<std::string, std::string_view>;
std::string_view authored_trim(std::string_view text) {
    while (!text.empty() && json_ws(text.front())) text.remove_prefix(1);
    while (!text.empty() && json_ws(text.back())) text.remove_suffix(1);
    return text;
}
AuthoredMembers authored_members(std::string_view text, const std::string& location,
                                std::initializer_list<const char*> allowed) {
    text = authored_trim(text);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        throw std::runtime_error(location + " must be an object");
    AuthoredMembers result;
    std::size_t p = 1;
    while (p < text.size() - 1) {
        while (p < text.size() && (json_ws(text[p]) || text[p] == ',')) ++p;
        if (p == text.size() - 1) break;
        const auto end = json_string_end(text, p);
        std::string key;
        if (end == std::string_view::npos || !json_unescape_string(text.substr(p + 1, end - p - 2), key))
            throw std::runtime_error(location + " has an invalid key");
        p = end;
        while (p < text.size() && json_ws(text[p])) ++p;
        if (p >= text.size() || text[p++] != ':') throw std::runtime_error(location + " has an invalid member");
        const auto value_end = json_value_end(text, p);
        if (value_end == std::string_view::npos) throw std::runtime_error(location + " has an invalid value");
        // An empty allow-list accepts any key (free-form name -> value maps).
        if (allowed.size() != 0 && std::none_of(allowed.begin(), allowed.end(), [&](const char* name) { return key == name; }))
            throw std::runtime_error(location + ": unsupported field '" + key + "'");
        if (!result.emplace(key, authored_trim(text.substr(p, value_end - p))).second)
            throw std::runtime_error(location + ": duplicate field '" + key + "'");
        p = value_end;
    }
    return result;
}
std::string authored_string(const AuthoredMembers& members, const char* key,
                            const std::string& location, const char* fallback = nullptr) {
    const auto found = members.find(key);
    if (found == members.end()) {
        if (fallback) return fallback;
        throw std::runtime_error(location + ": missing '" + key + "'");
    }
    const auto text = found->second;
    std::string value;
    if (text.size() < 2 || text.front() != '"' || text.back() != '"' ||
        !json_unescape_string(text.substr(1, text.size() - 2), value) || value.empty() ||
        value.find('\0') != std::string::npos)
        throw std::runtime_error(location + ": '" + key + "' must be a nonempty string");
    return value;
}
float authored_number(std::string_view text, const std::string& location) {
    float value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
        throw std::runtime_error(location + " must be a finite number");
    return value;
}
// A number or an array of 1..4 numbers (shader variable values).
std::vector<float> authored_floats(std::string_view value, const std::string& location) {
    std::vector<float> floats;
    auto text = authored_trim(value);
    if (!text.empty() && text.front() == '[') {
        if (text.back() != ']') throw std::runtime_error(location + ": malformed array");
        text = text.substr(1, text.size() - 2);
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto comma = text.find(',', start);
            const auto part = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (!authored_trim(part).empty()) floats.push_back(authored_number(authored_trim(part), location));
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
    } else {
        floats.push_back(authored_number(value, location));
    }
    if (floats.empty() || floats.size() > 4) throw std::runtime_error(location + ": needs 1 to 4 numbers");
    return floats;
}
std::optional<std::string_view> authored_payload(const cgltf_data* data, const cgltf_extras& extras,
                                                 std::string_view key = "darktide_asset") {
    std::string_view text;
    if (extras.data) text = extras.data;
    else if (data->json && extras.end_offset > extras.start_offset && extras.end_offset <= data->json_size)
        text = std::string_view(data->json + extras.start_offset, extras.end_offset - extras.start_offset);
    JsonSlice slice;
    if (!json_object_key(text, key, slice)) return std::nullopt;
    return authored_trim(text.substr(slice.begin, slice.end - slice.begin));
}
void authored_version(const AuthoredMembers& members, const std::string& location,
                      std::string_view kind = "darktide_asset") {
    const auto version = members.find("version");
    if (version == members.end() || authored_number(version->second, location + ".version") != 1.0f)
        throw std::runtime_error(location + ": supported " + std::string(kind) + " version is 1");
}
int authored_version_value(const AuthoredMembers& members, const std::string& location) {
    const auto version = members.find("version");
    if (version == members.end()) throw std::runtime_error(location + ": missing 'version'");
    const float value = authored_number(version->second, location + ".version");
    if (value != 1.0f && value != 2.0f) throw std::runtime_error(location + ": supported darktide_asset versions are 1 and 2");
    return static_cast<int>(value);
}
BodyDefinition parse_node_body(const AuthoredMembers& members, const std::string& location, int source_node, std::string id) {
    BodyDefinition result;
    result.id = std::move(id); result.source_node = source_node;
    result.actor = authored_string(members, "actor", location, "static");
    result.material = authored_string(members, "material", location, "default");
    const auto mass = members.find("mass");
    result.mass = mass == members.end() ? (result.actor == "dynamic" ? 1.0f : 0.0f) : authored_number(mass->second, location + ".mass");
    return result;
}
ColliderDefinition parse_node_collider(const AuthoredMembers& members, const std::string& location, int source_node, std::string id) {
    ColliderDefinition result; result.id = std::move(id); result.source_node = source_node;
    result.body = authored_string(members, "body", location);
    const auto shape = authored_string(members, "shape", location, "geometry");
    if (shape == "geometry") result.shape = ColliderShape::Geometry;
    else if (shape == "convex") result.shape = ColliderShape::Convex;
    else if (shape == "box") result.shape = ColliderShape::Box;
    else if (shape == "sphere") result.shape = ColliderShape::Sphere;
    else if (shape == "capsule") result.shape = ColliderShape::Capsule;
    else throw std::runtime_error(location + ": unsupported collider shape '" + shape + "'");
    const auto role = authored_string(members, "role", location, "collision");
    if (role == "collision") result.role = ColliderRole::Collision;
    else if (role == "both") result.role = ColliderRole::Both;
    else throw std::runtime_error(location + ": collider role must be collision or both");
    return result;
}
void populate_asset_definition(const cgltf_data* data, Scene& out) {
    bool has_v2 = false;
    if (out.selected_scene >= 0) {
        if (const auto payload = authored_payload(data, data->scenes[out.selected_scene].extras)) {
            const auto members = authored_members(*payload, "scene.darktide_asset", {"version", "body"});
            authored_version(members, "scene.darktide_asset");
            const auto record = members.find("body");
            if (record == members.end()) throw std::runtime_error("scene.darktide_asset requires a body");
            const auto body = authored_members(record->second, "body", {"id", "actor", "mass", "material"});
            BodyDefinition definition;
            definition.id = authored_string(body, "id", "body");
            definition.actor = authored_string(body, "actor", "body", "static");
            definition.material = authored_string(body, "material", "body", "default");
            const auto mass = body.find("mass");
            definition.mass = mass == body.end() ? (definition.actor == "dynamic" ? 1.0f : 0.0f)
                                                 : authored_number(mass->second, "body.mass");
            out.asset_definition.body = std::move(definition);
            ++out.source_features.physics_requested_count;
            ++out.source_features.physics_effective_count;
            out.source_features.physics_locations.push_back("scene[" + std::to_string(out.selected_scene) + "].extras.darktide_asset.body");
        }
    }
    std::set<int> visited;
    std::set<std::string> ids;
    std::vector<int> pending(out.scene_roots);
    while (!pending.empty()) {
        const int index = pending.back(); pending.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= out.nodes.size() || !visited.insert(index).second) continue;
        const auto& node = out.nodes[index];
        pending.insert(pending.end(), node.children.begin(), node.children.end());
        const auto payload = authored_payload(data, data->nodes[index].extras);
        if (!payload) continue;
        const auto location = "node[" + std::to_string(index) + "].darktide_asset";
        const auto members = authored_members(*payload, location, {"version", "id", "body", "collider", "joint", "visibility_group", "dangle", "aim", "particles", "lod", "render", "light", "flow", "data", "actor", "mover"});
        if (const auto record = members.find("data"); record != members.end()) {
            // the unit's script data (Unit.get_data), a JSON object or a string holding it
            if (!out.asset_definition.script_data.empty()) throw std::runtime_error(location + ".data: only one object may carry the unit data");
            const auto text = authored_trim(record->second);
            if (!text.empty() && text.front() == '"') {
                if (text.size() < 2 || !json_unescape_string(text.substr(1, text.size() - 2), out.asset_definition.script_data))
                    throw std::runtime_error(location + ".data: not a valid JSON string");
            } else {
                out.asset_definition.script_data = std::string(text);
            }
        }
        if (const auto record = members.find("flow"); record != members.end()) {
            // the unit's flow graph (flow_authoring.h), as a JSON object or a string holding it
            if (!out.asset_definition.flow.empty()) throw std::runtime_error(location + ".flow: only one object may carry the unit flow");
            const auto text = authored_trim(record->second);
            if (!text.empty() && text.front() == '"') {
                if (text.size() < 2 || !json_unescape_string(text.substr(1, text.size() - 2), out.asset_definition.flow))
                    throw std::runtime_error(location + ".flow: not a valid JSON string");
            } else {
                out.asset_definition.flow = std::string(text);
            }
        }
        if (const auto record = members.find("actor"); record != members.end()) {
            const auto where = location + ".actor";
            const auto fields = authored_members(record->second, where, {"name", "template", "shape_template", "material", "shape", "spawn"});
            NodeActorDefinition actor;
            actor.source_node = index;
            const auto* node_name = data->nodes[index].name;
            actor.name = authored_string(fields, "name", where, node_name ? node_name : "");
            actor.actor_template = authored_string(fields, "template", where, "keyframed");
            actor.shape_template = authored_string(fields, "shape_template", where, "default");
            actor.material = authored_string(fields, "material", where, "default");
            const auto shape = authored_string(fields, "shape", where, "capsule");
            if (shape == "capsule") actor.shape = ColliderShape::Capsule;
            else if (shape == "sphere") actor.shape = ColliderShape::Sphere;
            else if (shape == "box") actor.shape = ColliderShape::Box;
            else if (shape == "convex") actor.shape = ColliderShape::Convex;
            else if (shape == "geometry") actor.shape = ColliderShape::Geometry;
            else throw std::runtime_error(where + ".shape must be capsule, sphere, box, convex or geometry");
            if (const auto spawn = fields.find("spawn"); spawn != fields.end()) {
                if (spawn->second != "true" && spawn->second != "false") throw std::runtime_error(where + ".spawn must be true or false");
                actor.spawn = spawn->second == "true";
            }
            out.asset_definition.node_actors.push_back(std::move(actor));
        }
        if (const auto record = members.find("mover"); record != members.end()) {
            const auto where = location + ".mover";
            const auto fields = authored_members(record->second, where, {"name", "height", "radius", "slope_limit", "collision_filter"});
            MoverDefinition mover;
            mover.name = authored_string(fields, "name", where, "mover");
            mover.collision_filter = authored_string(fields, "collision_filter", where, mover.collision_filter.c_str());
            const auto number = [&](const char* key, float& target) {
                if (const auto it = fields.find(key); it != fields.end()) target = authored_number(it->second, where + "." + key);
            };
            number("height", mover.height); number("radius", mover.radius); number("slope_limit", mover.slope_limit);
            if (!(mover.radius > 0.0f) || !(mover.height > 0.0f) || mover.slope_limit < 0.0f)
                throw std::runtime_error(where + ": height and radius must be above 0");
            out.asset_definition.movers.push_back(std::move(mover));
        }
        if (const auto record = members.find("render"); record != members.end()) {
            const auto where = location + ".render";
            const auto fields = authored_members(record->second, where, {"visible", "shadow"});
            RenderSetting setting;
            setting.source_node = index;
            for (const auto& [key, value] : fields) {
                if (value != "true" && value != "false") throw std::runtime_error(where + "." + key + " must be true or false");
                (key == "visible" ? setting.visible : setting.shadow) = value == "true";
            }
            if (!setting.visible && !setting.shadow)
                throw std::runtime_error(where + ": a mesh that is neither visible nor casts a shadow draws nothing");
            out.asset_definition.render_settings.push_back(setting);
        }
        if (const auto record = members.find("light"); record != members.end()) {
            const auto where = location + ".light";
            const auto fields = authored_members(record->second, where, {"shadow"});
            if (const auto shadow = fields.find("shadow"); shadow != fields.end()) {
                if (shadow->second != "true" && shadow->second != "false") throw std::runtime_error(where + ".shadow must be true or false");
                if (shadow->second == "true") out.asset_definition.shadow_lights.push_back(index);
            }
        }
        const int version = authored_version_value(members, location);
        const auto id = authored_string(members, "id", location);
        if (!ids.insert(id).second) throw std::runtime_error(location + ": duplicate authored id '" + id + "'");
        if (members.count("visibility_group")) {
            const auto group = authored_string(members, "visibility_group", location);
            if (!group.empty()) out.asset_definition.visibility_groups.push_back({group, index});
        }
        if (const auto record = members.find("lod"); record != members.end()) {
            const auto where = location + ".lod";
            const auto fields = authored_members(record->second, where, {"group", "level", "down_to"});
            LodLevel level;
            level.group = authored_string(fields, "group", where, "lod");
            const auto number = [&](const char* key) {
                const auto found = fields.find(key);
                return found == fields.end() ? 0.0f : authored_number(found->second, where + "." + key);
            };
            const float level_number = number("level");
            if (level_number < 0 || level_number > 255 || level_number != static_cast<float>(static_cast<int>(level_number)))
                throw std::runtime_error(where + ".level must be a whole number from 0");
            level.level = static_cast<int>(level_number);
            level.down_to = number("down_to");
            if (level.down_to < 0) throw std::runtime_error(where + ".down_to must not be negative");
            level.source_node = index;
            out.asset_definition.lod_levels.push_back(std::move(level));
        }
        if (const auto record = members.find("particles"); record != members.end()) {
            const auto where = location + ".particles";
            const auto fields = authored_members(record->second, where, {"name", "effect", "source", "extract", "materials"});
            ParticleEffectDefinition effect;
            effect.name = authored_string(fields, "name", where);
            if (const auto description = fields.find("effect"); description != fields.end()) {
                // the description as a JSON object, or as a string holding it
                const auto text = authored_trim(description->second);
                if (!text.empty() && text.front() == '"') {
                    if (text.size() < 2 || !json_unescape_string(text.substr(1, text.size() - 2), effect.description))
                        throw std::runtime_error(where + ".effect: not a valid JSON string");
                } else {
                    effect.description = std::string(text);
                }
            } else
                effect.source = authored_string(fields, "source", where);
            effect.extract = authored_string(fields, "extract", where);
            if (const auto materials = fields.find("materials"); materials != fields.end()) {
                for (const auto& [key, value] : authored_members(materials->second, where + ".materials", {})) {
                    if (key.size() != 16 || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                        throw std::runtime_error(where + ".materials: keys are 16-digit material hashes");
                    const auto at = where + ".materials." + key;
                    const auto edit = authored_members(value, at, {"variables", "textures"});
                    ParticleMaterialEdit out_edit;
                    if (const auto variables = edit.find("variables"); variables != edit.end())
                        for (const auto& [name, numbers] : authored_members(variables->second, at + ".variables", {}))
                            out_edit.variables.emplace(name, authored_floats(numbers, at + ".variables." + name));
                    if (const auto textures = edit.find("textures"); textures != edit.end())
                        for (const auto& [channel, image] : authored_members(textures->second, at + ".textures", {})) {
                            AuthoredMembers wrapper; wrapper.emplace("image", image);
                            out_edit.textures.emplace(channel, authored_string(wrapper, "image", at + ".textures." + channel));
                        }
                    effect.materials.emplace(std::stoull(key, nullptr, 16), std::move(out_edit));
                }
            }
            out.particle_effects.push_back(std::move(effect));
        }
        if (const auto record = members.find("dangle"); record != members.end()) {
            const auto fields = authored_members(record->second, location + ".dangle",
                {"mode", "length", "mass", "gravity", "damping", "stiffness", "max_angle", "max_stretch"});
            DangleDefinition dangle; dangle.source_node = index;
            const auto mode = authored_string(fields, "mode", location + ".dangle", "swing");
            if (mode != "swing" && mode != "jiggle") throw std::runtime_error(location + ".dangle: mode must be swing or jiggle");
            if (mode == "jiggle") { dangle.jiggle = true; dangle.stiffness = 1000.0f; dangle.damping = 400.0f; }
            const auto number = [&](const char* key, float& target) {
                if (const auto it = fields.find(key); it != fields.end()) target = authored_number(it->second, location + ".dangle." + key);
            };
            number("length", dangle.length); number("mass", dangle.mass); number("gravity", dangle.gravity);
            number("damping", dangle.damping); number("stiffness", dangle.stiffness); number("max_angle", dangle.max_angle_degrees);
            number("max_stretch", dangle.max_stretch);
            out.asset_definition.dangles.push_back(dangle);
        }
        if (const auto record = members.find("aim"); record != members.end()) {
            const auto fields = authored_members(record->second, location + ".aim", {"target", "turn"});
            AimDefinition aim; aim.source_node = index;
            aim.target = authored_string(fields, "target", location + ".aim");
            if (const auto turn = fields.find("turn"); turn != fields.end())
                for (const auto& [bone, weight] : authored_members(turn->second, location + ".aim.turn", {}))
                    aim.turn.push_back({bone, authored_number(weight, location + ".aim.turn." + bone)});
            if (aim.target.empty() || aim.turn.empty()) throw std::runtime_error(location + ".aim needs a target and bones to turn");
            out.asset_definition.aims.push_back(std::move(aim));
        }
        if (version == 1) {
        authored_version(members, location);
        if (members.count("body") || members.count("joint")) throw std::runtime_error(location + ": version 1 node extras only support collider");
        const auto record = members.find("collider");
        if (record == members.end()) continue;
        out.asset_definition.colliders.push_back(parse_node_collider(authored_members(record->second, location + ".collider", {"body", "shape", "role"}), location, index, id));
        continue;
        }
        has_v2 = true;
        if (const auto record = members.find("body"); record != members.end()) {
            out.asset_definition.node_bodies.push_back(parse_node_body(authored_members(record->second, location + ".body", {"actor", "mass", "material"}), location + ".body", index, id));
            ++out.source_features.physics_requested_count;
            ++out.source_features.physics_effective_count;
            out.source_features.physics_locations.push_back(location + ".body");
        }
        if (const auto record = members.find("collider"); record != members.end())
            out.asset_definition.colliders.push_back(parse_node_collider(authored_members(record->second, location + ".collider", {"body", "shape", "role"}), location, index, id));
        if (const auto record = members.find("joint"); record != members.end()) {
            const auto joint = authored_members(record->second, location + ".joint", {"body_a", "body_b", "kind", "twist_min", "twist_max", "swing_y", "swing_z",
                "slide_min", "slide_max", "spring_stiffness", "spring_damping", "break_force", "break_torque"});
            JointDefinition result; result.id = id; result.source_node = index;
            result.body_a = authored_string(joint, "body_a", location + ".joint"); result.body_b = authored_string(joint, "body_b", location + ".joint");
            const auto kind = authored_string(joint, "kind", location + ".joint");
            if (kind == "fixed") result.kind = JointKind::Fixed; else if (kind == "hinge") result.kind = JointKind::Hinge; else if (kind == "ragdoll") result.kind = JointKind::Ragdoll; else if (kind == "slider") result.kind = JointKind::Slider; else if (kind == "ball") result.kind = JointKind::Ball; else throw std::runtime_error(location + ".joint: unsupported kind '" + kind + "'");
            auto angle = [&](const char* key) -> std::optional<float> { const auto it = joint.find(key); return it == joint.end() ? std::nullopt : std::optional<float>(authored_number(it->second, location + ".joint." + key)); };
            result.twist_min = angle("twist_min"); result.twist_max = angle("twist_max"); if (const auto value = angle("swing_y")) result.swing_y = *value; if (const auto value = angle("swing_z")) result.swing_z = *value;
            result.slide_min = angle("slide_min"); result.slide_max = angle("slide_max");
            if (const auto value = angle("spring_stiffness")) result.spring_stiffness = *value;
            if (const auto value = angle("spring_damping")) result.spring_damping = *value;
            if (const auto value = angle("break_force")) result.break_force = *value;
            if (const auto value = angle("break_torque")) result.break_torque = *value;
            out.asset_definition.joints.push_back(std::move(result));
        }
    }
    if (has_v2 && out.asset_definition.body) throw std::runtime_error("version 1 scene body cannot be mixed with version 2 node bodies");
}
bool json_value_false(std::string_view value) {
    std::size_t begin = 0, end = value.size();
    while (begin < end && json_ws(value[begin])) ++begin;
    while (end > begin && json_ws(value[end - 1])) --end;
    return value.substr(begin, end - begin) == "false";
}

struct AnimationPointerTarget {
    int node = -1;
    cgltf_animation_path_type path = cgltf_animation_path_type_invalid;
};

// KHR_animation_pointer stores its JSON pointer in the channel extension
// object.  Only the node property forms have a native SourceScene lowering;
// callers deliberately retain every other valid pointer as a lossy diagnostic.
bool animation_pointer_target(const cgltf_animation_channel& channel,
                              std::string_view& pointer,
                              std::string& pointer_storage,
                              AnimationPointerTarget& target) {
    for (cgltf_size i = 0; i < channel.extensions_count; ++i) {
        const auto& extension = channel.extensions[i];
        if (!extension.name || std::strcmp(extension.name, "KHR_animation_pointer") != 0 || !extension.data) continue;
        JsonSlice value;
        if (!json_object_key(extension.data, "pointer", value)) return false;
        auto encoded = extension.data + value.begin;
        while (*encoded && json_ws(*encoded)) ++encoded;
        if (*encoded != '"') return false;
        const auto encoded_begin = static_cast<std::size_t>(encoded - extension.data);
        const auto end = json_string_end(extension.data, encoded_begin);
        if (end == std::string_view::npos || end <= encoded_begin + 1) return false;
        const std::string_view encoded_pointer(extension.data + encoded_begin + 1, end - encoded_begin - 2);
        if (!json_unescape_string(encoded_pointer, pointer_storage)) { pointer = {}; return true; }
        pointer = pointer_storage;
        constexpr std::string_view prefix = "/nodes/";
        if (!pointer.starts_with(prefix)) return true;
        const auto slash = pointer.find('/', prefix.size());
        if (slash == std::string_view::npos) return true;
        int node = -1;
        const auto number = pointer.substr(prefix.size(), slash - prefix.size());
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), node);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || node < 0) return true;
        const auto property = pointer.substr(slash + 1);
        if (property == "translation") target.path = cgltf_animation_path_type_translation;
        else if (property == "rotation") target.path = cgltf_animation_path_type_rotation;
        else if (property == "scale") target.path = cgltf_animation_path_type_scale;
        else if (property == "weights") target.path = cgltf_animation_path_type_weights;
        else return true;
        target.node = node;
        return true;
    }
    return false;
}
bool node_visibility(const cgltf_node& node) {
    for (cgltf_size i = 0; i < node.extensions_count; ++i) {
        const auto& extension = node.extensions[i];
        if (!extension.name || std::strcmp(extension.name, "KHR_node_visibility") != 0 || !extension.data) continue;
        JsonSlice value;
        if (json_object_key(extension.data, "visible", value))
            return !json_value_false(std::string_view(extension.data).substr(value.begin, value.end - value.begin));
    }
    return true;
}
void inventory_physics_json(std::string_view json, std::string location, SourceFeatureInventory& features) {
    JsonSlice physics;
    if (!json_object_key(json, "physics", physics)) return;
    ++features.physics_requested_count;
    features.physics_locations.push_back(location + ".physics");
    const auto value = json.substr(physics.begin, physics.end - physics.begin);
    if (!json_value_false(value) && !json_object_enabled_false(value)) ++features.physics_effective_count;
}
void inventory_extras(const cgltf_data* data, const cgltf_extras& extras,
                      const std::string& location, SourceFeatureInventory& features) {
    std::string_view json;
    if (extras.data) {
        json = extras.data;
    } else if (data && data->json && extras.end_offset > extras.start_offset && extras.end_offset <= data->json_size) {
        // cgltf intentionally parses mesh targetNames in-place and retains only
        // offsets for mesh extras. Other extras receive an owned data string.
        json = std::string_view(data->json + extras.start_offset, extras.end_offset - extras.start_offset);
    } else {
        return;
    }
    inventory_physics_json(json, location + ".extras", features);
    JsonSlice nested;
    for (const char* key : {"darktide", "stingray"}) {
        if (json_object_key(json, key, nested)) inventory_physics_json(json.substr(nested.begin, nested.end - nested.begin), location + ".extras." + key, features);
    }
}

struct CgltfDocument {
    struct MeshoptCoreFallback {
        std::size_t buffer_view_index = 0;
        std::string extension;
        std::string message;
    };
    cgltf_data* data = nullptr;
    std::size_t meshopt_buffer_view_count = 0;
    std::vector<MeshoptCoreFallback> meshopt_core_fallbacks;
    ~CgltfDocument() { if (data) cgltf_free(data); }
    CgltfDocument(const CgltfDocument&) = delete;
    CgltfDocument& operator=(const CgltfDocument&) = delete;
    CgltfDocument(CgltfDocument&& other) noexcept
        : data(other.data),
          meshopt_buffer_view_count(other.meshopt_buffer_view_count),
          meshopt_core_fallbacks(std::move(other.meshopt_core_fallbacks)) {
        other.data = nullptr;
    }
    CgltfDocument& operator=(CgltfDocument&& other) noexcept {
        if (this != &other) {
            if (data) cgltf_free(data);
            data = other.data;
            meshopt_buffer_view_count = other.meshopt_buffer_view_count;
            meshopt_core_fallbacks = std::move(other.meshopt_core_fallbacks);
            other.data = nullptr;
        }
        return *this;
    }
    CgltfDocument() = default;
};

std::string cgltf_result_name(cgltf_result result) {
    switch (result) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data too short";
        case cgltf_result_unknown_format: return "unknown format";
        case cgltf_result_invalid_json: return "invalid JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid cgltf options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "legacy glTF 1.x";
        default: return "unknown cgltf error";
    }
}

bool path_is_within(const std::filesystem::path& path, const std::filesystem::path& directory) {
    auto path_it = path.begin();
    auto directory_it = directory.begin();
    for (; directory_it != directory.end(); ++directory_it, ++path_it) {
        if (path_it == path.end() || *path_it != *directory_it) return false;
    }
    return true;
}

struct BufferFileReadContext {
    std::filesystem::path document_path;
    std::filesystem::path document_root;
};

#ifdef _WIN32
std::filesystem::path windows_long_path(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto native = absolute.native();
    if (native.rfind(LR"(\\?\)", 0) == 0) return absolute;
    if (native.rfind(LR"(\\)", 0) == 0)
        return std::filesystem::path(LR"(\\?\UNC\)" + native.substr(2));
    return std::filesystem::path(LR"(\\?\)" + native);
}
#endif

cgltf_result guarded_file_read(const cgltf_memory_options* memory_options,
                               const cgltf_file_options* file_options,
                               const char* path,
                               cgltf_size* size,
                               void** data) {
    const auto* context = static_cast<const BufferFileReadContext*>(file_options->user_data);
    if (!context || !path) return cgltf_result_file_not_found;

    const std::string requested(path);
    if (requested.find('\0') != std::string::npos) return cgltf_result_file_not_found;
    const std::filesystem::path requested_path(requested);
    const bool is_document_path = requested == context->document_path.string();
#ifdef _WIN32
    const auto native_requested = requested_path.native();
    const bool extended_drive_path = native_requested.rfind(LR"(\\?\)", 0) == 0 &&
                                     native_requested.size() >= 6 && native_requested[5] == L':';
#endif
    // The document path passed by cgltf is absolute.  Reject URI schemes or
    // drive-like components in the relative portion, while permitting that
    // absolute document path and absolute paths to contained files.
    if (!is_document_path) {
        for (auto component = requested_path.begin(); component != requested_path.end(); ++component) {
            const auto component_text = component->string();
            if (component_text.find(':') == std::string::npos ||
                component_text == requested_path.root_name().string()) continue;
#ifdef _WIN32
            if (extended_drive_path && component->native().size() == 2 &&
                component->native()[1] == L':' && component->native()[0] == native_requested[4]) continue;
#endif
            return cgltf_result_file_not_found;
        }
    }

    std::error_code error;
    std::filesystem::path candidate;
    if (requested == context->document_path.string()) {
        // Reuse the canonical wide path already established by load_document;
        // reconstructing it from cgltf's narrow path string loses long-path
        // filesystem handling on Windows.
        candidate = context->document_path;
    } else {
#ifdef _WIN32
        candidate = std::filesystem::canonical(windows_long_path(requested_path), error);
        if (!error) candidate = windows_long_path(candidate);
#else
        candidate = std::filesystem::canonical(requested_path, error);
#endif
        if (error) return cgltf_result_file_not_found;
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error ||
        (candidate != context->document_path && !path_is_within(candidate, context->document_root)))
        return cgltf_result_file_not_found;

    // Read the canonical target so a symlink cannot redirect the delegated
    // open after validation. Windows' narrow fopen cannot open extended paths.
#ifdef _WIN32
    void* (*memory_alloc)(void*, cgltf_size) = memory_options->alloc_func ? memory_options->alloc_func : &cgltf_default_alloc;
    void (*memory_free)(void*, void*) = memory_options->free_func ? memory_options->free_func : &cgltf_default_free;
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, candidate.c_str(), L"rb") != 0 || !file) return cgltf_result_file_not_found;

    cgltf_size file_size = size ? *size : 0;
    if (file_size == 0) {
        if (_fseeki64(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            return cgltf_result_io_error;
        }
        const __int64 length = _ftelli64(file);
        if (length < 0 || _fseeki64(file, 0, SEEK_SET) != 0) {
            std::fclose(file);
            return cgltf_result_io_error;
        }
        file_size = static_cast<cgltf_size>(length);
    }

    char* file_data = static_cast<char*>(memory_alloc(memory_options->user_data, file_size));
    if (!file_data) {
        std::fclose(file);
        return cgltf_result_out_of_memory;
    }
    const cgltf_size read_size = static_cast<cgltf_size>(std::fread(file_data, 1, file_size, file));
    std::fclose(file);
    if (read_size != file_size) {
        memory_free(memory_options->user_data, file_data);
        return cgltf_result_io_error;
    }
    if (size) *size = file_size;
    if (data) *data = file_data;
    return cgltf_result_success;
#else
    return cgltf_default_file_read(memory_options, file_options, candidate.string().c_str(), size, data);
#endif
}

using Mat4 = Matrix4;

Mat4 identity() {
    return Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}

Mat4 multiply(const Mat4& a, const Mat4& b, bool* clamped = nullptr) {
    Mat4 result{};
    bool did_clamp = false;
    constexpr double max_float = static_cast<double>(std::numeric_limits<float>::max());
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += static_cast<double>(a[static_cast<std::size_t>(k * 4 + row)]) *
                    static_cast<double>(b[static_cast<std::size_t>(column * 4 + k)]);
            }
            const auto index = static_cast<std::size_t>(column * 4 + row);
            if (sum > max_float) { result[index] = std::numeric_limits<float>::max(); did_clamp = true; }
            else if (sum < -max_float) { result[index] = -std::numeric_limits<float>::max(); did_clamp = true; }
            else result[index] = static_cast<float>(sum);
        }
    }
    if (clamped) *clamped = did_clamp;
    return result;
}

Mat4 from_cgltf_matrix(const cgltf_float* matrix) {
    Mat4 result{};
    std::copy(matrix, matrix + 16, result.begin());
    return result;
}

Mat4 instance_trs(const std::array<float, 3>& t, const std::array<float, 4>& q,
                 const std::array<float, 3>& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return Mat4{
        (1 - 2*y*y - 2*z*z) * s[0], (2*x*y + 2*z*w) * s[0], (2*x*z - 2*y*w) * s[0], 0,
        (2*x*y - 2*z*w) * s[1], (1 - 2*x*x - 2*z*z) * s[1], (2*y*z + 2*x*w) * s[1], 0,
        (2*x*z + 2*y*w) * s[2], (2*y*z - 2*x*w) * s[2], (1 - 2*x*x - 2*y*y) * s[2], 0,
        t[0], t[1], t[2], 1};
}

std::vector<float> accessor_floats(const cgltf_accessor* accessor);

std::vector<Mat4> mesh_gpu_instance_worlds(const cgltf_node& node, const Mat4& node_world) {
    if (!node.has_mesh_gpu_instancing) return {node_world};
    const auto& extension = node.mesh_gpu_instancing;
    if (!extension.attributes_count) throw std::runtime_error("EXT_mesh_gpu_instancing has no attributes");
    const cgltf_accessor* translation = nullptr;
    const cgltf_accessor* rotation = nullptr;
    const cgltf_accessor* scale = nullptr;
    std::set<std::string> attribute_names;
    const cgltf_size count = extension.attributes[0].data ? extension.attributes[0].data->count : 0;
    if (!count) throw std::runtime_error("EXT_mesh_gpu_instancing has an empty accessor");
    for (cgltf_size i = 0; i < extension.attributes_count; ++i) {
        const auto& attribute = extension.attributes[i];
        if (!attribute.name || !attribute.data) throw std::runtime_error("EXT_mesh_gpu_instancing attribute is missing a name or accessor");
        const std::string name(attribute.name);
        const cgltf_accessor* accessor = attribute.data;
        if (!attribute_names.insert(name).second) throw std::runtime_error("duplicate EXT_mesh_gpu_instancing attribute: " + name);
        const bool known = name == "TRANSLATION" || name == "ROTATION" || name == "SCALE";
        if (!known) {
            if (name.empty() || name.front() != '_') throw std::runtime_error("invalid EXT_mesh_gpu_instancing attribute semantic: " + name);
            if (accessor->count != count) throw std::runtime_error("EXT_mesh_gpu_instancing accessors have different instance counts");
            continue; // Retained in NodeInfo for explicit downstream canonicalization.
        }
        const cgltf_accessor** destination = name == "TRANSLATION" ? &translation : name == "ROTATION" ? &rotation : &scale;
        const cgltf_type expected_type = name == "ROTATION" ? cgltf_type_vec4 : cgltf_type_vec3;
        const bool float_component = accessor->component_type == cgltf_component_type_r_32f && !accessor->normalized;
        const bool packed_rotation = name == "ROTATION" && accessor->normalized &&
            (accessor->component_type == cgltf_component_type_r_8 || accessor->component_type == cgltf_component_type_r_16);
        if (accessor->type != expected_type || (!float_component && !packed_rotation))
            throw std::runtime_error("invalid EXT_mesh_gpu_instancing " + name + " accessor type");
        if (accessor->count != count) throw std::runtime_error("EXT_mesh_gpu_instancing accessors have different instance counts");
        *destination = accessor;
    }
    const auto read = [&](const cgltf_accessor* accessor, std::uint32_t components, const char* name) {
        std::vector<float> values = accessor_floats(accessor);
        if (accessor && values.size() != static_cast<std::size_t>(count) * components)
            throw std::runtime_error(std::string("cannot decode EXT_mesh_gpu_instancing ") + name + " accessor");
        for (float value : values) if (!std::isfinite(value))
            throw std::runtime_error(std::string("EXT_mesh_gpu_instancing ") + name + " contains a non-finite value");
        return values;
    };
    const auto tv = read(translation, 3, "TRANSLATION");
    const auto rv = read(rotation, 4, "ROTATION");
    const auto sv = read(scale, 3, "SCALE");
    std::vector<Mat4> worlds;
    worlds.reserve(static_cast<std::size_t>(count));
    for (cgltf_size i = 0; i < count; ++i) {
        const std::array<float, 3> t = translation ? std::array<float, 3>{tv[i*3], tv[i*3+1], tv[i*3+2]} : std::array<float, 3>{0,0,0};
        std::array<float, 4> r = rotation ? std::array<float, 4>{rv[i*4], rv[i*4+1], rv[i*4+2], rv[i*4+3]} : std::array<float, 4>{0,0,0,1};
        const std::array<float, 3> s = scale ? std::array<float, 3>{sv[i*3], sv[i*3+1], sv[i*3+2]} : std::array<float, 3>{1,1,1};
        const float quaternion_length_sq = r[0]*r[0] + r[1]*r[1] + r[2]*r[2] + r[3]*r[3];
        if (quaternion_length_sq <= 1e-12f ||
            (rotation && rotation->component_type == cgltf_component_type_r_32f && std::fabs(quaternion_length_sq - 1.0f) > 1e-4f)) {
            throw std::runtime_error("EXT_mesh_gpu_instancing ROTATION must contain unit quaternions");
        }
        const float inverse_quaternion_length = 1.0f / std::sqrt(quaternion_length_sq);
        for (float& component : r) component *= inverse_quaternion_length;
        worlds.push_back(multiply(node_world, instance_trs(t, r, s)));
    }
    return worlds;
}

const Mat4& basis_matrix() {
    static const Mat4 basis{1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1};
    return basis;
}

const Mat4& basis_inverse_matrix() {
    static const Mat4 inverse{1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1};
    return inverse;
}

Mat4 convert_matrix_basis(const Mat4& matrix) {
    return multiply(multiply(basis_matrix(), matrix), basis_inverse_matrix());
}

std::array<float, 3> convert_vector_basis(float x, float y, float z) {
    return {x, -z, y};
}

std::array<float, 3> transform_point(const Mat4& matrix, float x, float y, float z,
                                     bool* clamped = nullptr) {
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    bool did_clamp = false;
    const auto narrow = [&](double value) {
        if (value > limit) { did_clamp = true; return std::numeric_limits<float>::max(); }
        if (value < -limit) { did_clamp = true; return -std::numeric_limits<float>::max(); }
        return static_cast<float>(value);
    };
    const float tx = narrow(static_cast<double>(matrix[0]) * x + static_cast<double>(matrix[4]) * y +
                            static_cast<double>(matrix[8]) * z + matrix[12]);
    const float ty = narrow(static_cast<double>(matrix[1]) * x + static_cast<double>(matrix[5]) * y +
                            static_cast<double>(matrix[9]) * z + matrix[13]);
    const float tz = narrow(static_cast<double>(matrix[2]) * x + static_cast<double>(matrix[6]) * y +
                            static_cast<double>(matrix[10]) * z + matrix[14]);
    if (clamped) *clamped = *clamped || did_clamp;
    return convert_vector_basis(tx, ty, tz);
}

double determinant3(const Mat4& matrix) {
    return static_cast<double>(matrix[0]) * (static_cast<double>(matrix[5]) * matrix[10] - static_cast<double>(matrix[9]) * matrix[6])
         - static_cast<double>(matrix[4]) * (static_cast<double>(matrix[1]) * matrix[10] - static_cast<double>(matrix[9]) * matrix[2])
         + static_cast<double>(matrix[8]) * (static_cast<double>(matrix[1]) * matrix[6] - static_cast<double>(matrix[5]) * matrix[2]);
}

std::array<float, 3> transform_normal(const Mat4& matrix, float x, float y, float z) {
    const double a = matrix[0], b = matrix[4], c = matrix[8];
    const double d = matrix[1], e = matrix[5], f = matrix[9];
    const double g = matrix[2], h = matrix[6], i = matrix[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    double tx = x, ty = y, tz = z;
    if (std::isfinite(det) && std::fabs(det) > 1e-30) {
        const double ia = (e * i - f * h) / det;
        const double ib = (c * h - b * i) / det;
        const double ic = (b * f - c * e) / det;
        const double id = (f * g - d * i) / det;
        const double ie = (a * i - c * g) / det;
        const double iff = (c * d - a * f) / det;
        const double ig = (d * h - e * g) / det;
        const double ih = (b * g - a * h) / det;
        const double ii = (a * e - b * d) / det;
        tx = ia * x + id * y + ig * z;
        ty = ib * x + ie * y + ih * z;
        tz = ic * x + iff * y + ii * z;
    }
    const double converted_x = tx, converted_y = -tz, converted_z = ty;
    const double length = std::hypot(converted_x, std::hypot(converted_y, converted_z));
    if (!std::isfinite(length) || length <= 1e-30) return {0.0f, 0.0f, 1.0f};
    return {static_cast<float>(converted_x / length), static_cast<float>(converted_y / length),
            static_cast<float>(converted_z / length)};
}

std::array<float, 3> transform_tangent(const Mat4& matrix, float x, float y, float z) {
    const double tx = static_cast<double>(matrix[0]) * x + static_cast<double>(matrix[4]) * y +
        static_cast<double>(matrix[8]) * z;
    const double ty = static_cast<double>(matrix[1]) * x + static_cast<double>(matrix[5]) * y +
        static_cast<double>(matrix[9]) * z;
    const double tz = static_cast<double>(matrix[2]) * x + static_cast<double>(matrix[6]) * y +
        static_cast<double>(matrix[10]) * z;
    const double converted_x = tx, converted_y = -tz, converted_z = ty;
    const double length = std::hypot(converted_x, std::hypot(converted_y, converted_z));
    if (!std::isfinite(length) || length <= 1e-30) return {1.0f, 0.0f, 0.0f};
    return {static_cast<float>(converted_x / length), static_cast<float>(converted_y / length),
            static_cast<float>(converted_z / length)};
}

std::array<float, 4> quaternion_multiply(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

std::array<float, 4> convert_quaternion_basis(const std::array<float, 4>& q) {
    const std::array<float, 4> basis_q{kSqrtHalf, 0, 0, kSqrtHalf};
    const std::array<float, 4> basis_inv{-kSqrtHalf, 0, 0, kSqrtHalf};
    return quaternion_multiply(quaternion_multiply(basis_q, q), basis_inv);
}

template <typename T>
int pointer_index(const T* base, std::size_t count, const T* pointer) {
    if (!pointer) return -1;
    const auto index = pointer - base;
    if (index < 0 || static_cast<std::size_t>(index) >= count) return -1;
    return static_cast<int>(index);
}

std::optional<cgltf_size> selected_material_variant(const cgltf_data* data,
                                                    const LoadOptions& options) {
    if (!options.material_variant) return std::nullopt;
    if (options.material_variant->empty())
        throw std::runtime_error("material variant name must not be empty");
    std::optional<cgltf_size> selected;
    for (cgltf_size i = 0; i < data->variants_count; ++i) {
        if (!data->variants[i].name || *options.material_variant != data->variants[i].name) continue;
        if (selected) throw std::runtime_error("material variant name is ambiguous: " + *options.material_variant);
        selected = i;
    }
    if (!selected) throw std::runtime_error("material variant is absent from glTF: " + *options.material_variant);
    return selected;
}

const cgltf_material* effective_material(const cgltf_data* data,
                                         const cgltf_primitive& primitive,
                                         const LoadOptions& options) {
    const auto selected = selected_material_variant(data, options);
    if (!selected) return primitive.material;
    const cgltf_material* result = primitive.material;
    bool mapped = false;
    for (cgltf_size i = 0; i < primitive.mappings_count; ++i) {
        const auto& mapping = primitive.mappings[i];
        if (mapping.variant != *selected) continue;
        if (mapped) throw std::runtime_error("material variant has duplicate mappings on one primitive");
        result = mapping.material;
        mapped = true;
    }
    return result;
}

std::vector<float> accessor_floats(const cgltf_accessor* accessor) {
    if (!accessor) return {};
    const cgltf_size count = cgltf_accessor_unpack_floats(accessor, nullptr, 0);
    if (count == 0) return {};
    std::vector<float> values(static_cast<std::size_t>(count));
    const cgltf_size written = cgltf_accessor_unpack_floats(accessor, values.data(), count);
    if (written != count) return {};
    return values;
}

std::vector<std::uint32_t> accessor_indices(const cgltf_accessor* accessor, std::size_t vertex_count) {
    if (!accessor) {
        std::vector<std::uint32_t> indices(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) indices[i] = static_cast<std::uint32_t>(i);
        return indices;
    }
    std::vector<std::uint32_t> indices(static_cast<std::size_t>(accessor->count));
    const cgltf_size component_size = cgltf_component_size(accessor->component_type);
    if (accessor->type != cgltf_type_scalar || component_size == 0 || component_size > sizeof(std::uint32_t)) return {};
    const auto range_fits = [](cgltf_size offset, cgltf_size count, cgltf_size stride,
                               cgltf_size width, cgltf_size available) {
        if (offset > available) return false;
        if (count == 0) return true;
        if (stride == 0 || width > available - offset) return false;
        return count - 1 <= (available - offset - width) / stride;
    };
    if (accessor->buffer_view) {
        const auto* data = static_cast<const std::uint8_t*>(cgltf_buffer_view_data(accessor->buffer_view));
        const cgltf_size stride = accessor->stride ? accessor->stride : component_size;
        if (!data || !range_fits(accessor->offset, accessor->count, stride, component_size,
                                 accessor->buffer_view->size)) return {};
        data += accessor->offset;
        for (cgltf_size i = 0; i < accessor->count; ++i) {
            indices[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(
                cgltf_component_read_index(data + i * stride, accessor->component_type));
        }
    } // No bufferView is the core glTF zero-initialized base accessor.
    if (accessor->is_sparse) {
        const auto& sparse = accessor->sparse;
        const cgltf_size sparse_index_size = cgltf_component_size(sparse.indices_component_type);
        const auto* sparse_indices = sparse.indices_buffer_view
            ? static_cast<const std::uint8_t*>(cgltf_buffer_view_data(sparse.indices_buffer_view)) : nullptr;
        const auto* sparse_values = sparse.values_buffer_view
            ? static_cast<const std::uint8_t*>(cgltf_buffer_view_data(sparse.values_buffer_view)) : nullptr;
        if (!sparse_indices || !sparse_values || sparse_index_size == 0 ||
            !range_fits(sparse.indices_byte_offset, sparse.count, sparse_index_size, sparse_index_size,
                        sparse.indices_buffer_view->size) ||
            !range_fits(sparse.values_byte_offset, sparse.count, component_size, component_size,
                        sparse.values_buffer_view->size)) return {};
        sparse_indices += sparse.indices_byte_offset;
        sparse_values += sparse.values_byte_offset;
        for (cgltf_size i = 0; i < sparse.count; ++i) {
            const cgltf_size destination = cgltf_component_read_index(
                sparse_indices + i * sparse_index_size, sparse.indices_component_type);
            if (destination >= accessor->count) return {};
            indices[static_cast<std::size_t>(destination)] = static_cast<std::uint32_t>(
                cgltf_component_read_index(sparse_values + i * component_size, accessor->component_type));
        }
    }
    return indices;
}

bool accessor_uses_meshopt(const cgltf_accessor* accessor) {
    if (!accessor) return false;
    if (accessor->buffer_view && accessor->buffer_view->has_meshopt_compression && !accessor->buffer_view->data) return true;
    if (accessor->is_sparse) {
        if (accessor->sparse.indices_buffer_view && accessor->sparse.indices_buffer_view->has_meshopt_compression && !accessor->sparse.indices_buffer_view->data) return true;
        if (accessor->sparse.values_buffer_view && accessor->sparse.values_buffer_view->has_meshopt_compression && !accessor->sparse.values_buffer_view->data) return true;
    }
    return false;
}

int primitive_mode(cgltf_primitive_type type) {
    switch (type) {
        case cgltf_primitive_type_points: return 0;
        case cgltf_primitive_type_lines: return 1;
        case cgltf_primitive_type_line_loop: return 2;
        case cgltf_primitive_type_line_strip: return 3;
        case cgltf_primitive_type_triangles: return 4;
        case cgltf_primitive_type_triangle_strip: return 5;
        case cgltf_primitive_type_triangle_fan: return 6;
        default: return 4;
    }
}

bool attribute_semantic(const cgltf_attribute& attribute, VertexChannel::Semantic& semantic, std::uint32_t& set) {
    set = attribute.index >= 0 ? static_cast<std::uint32_t>(attribute.index) : 0;
    switch (attribute.type) {
        case cgltf_attribute_type_position: semantic = VertexChannel::Semantic::Position; set = 0; return true;
        case cgltf_attribute_type_normal: semantic = VertexChannel::Semantic::Normal; set = 0; return true;
        case cgltf_attribute_type_tangent: semantic = VertexChannel::Semantic::Tangent; set = 0; return true;
        case cgltf_attribute_type_texcoord: semantic = VertexChannel::Semantic::Texcoord; return true;
        case cgltf_attribute_type_color: semantic = VertexChannel::Semantic::Color; return true;
        case cgltf_attribute_type_joints: semantic = VertexChannel::Semantic::BlendIndices; return true;
        case cgltf_attribute_type_weights: semantic = VertexChannel::Semantic::BlendWeights; return true;
        default: return false;
    }
}

VertexChannel* find_channel(Primitive& primitive, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    for (auto& channel : primitive.channels) {
        if (channel.semantic == semantic && channel.set == set) return &channel;
    }
    return nullptr;
}

const VertexChannel* find_channel(const std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    for (const auto& channel : channels) {
        if (channel.semantic == semantic && channel.set == set) return &channel;
    }
    return nullptr;
}

VertexChannel* find_channel(std::vector<VertexChannel>& channels, VertexChannel::Semantic semantic, std::uint32_t set = 0) {
    for (auto& channel : channels) {
        if (channel.semantic == semantic && channel.set == set) return &channel;
    }
    return nullptr;
}

void calculate_bounds(const std::vector<VertexChannel>& channels,
                      std::array<float, 3>& bounds_min,
                      std::array<float, 3>& bounds_max,
                      float& bounds_radius) {
    const auto* position = find_channel(channels, VertexChannel::Semantic::Position);
    if (!position || position->components < 3 || position->values.empty()) return;
    bounds_min = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    bounds_max = {-bounds_min[0], -bounds_min[1], -bounds_min[2]};
    double radius = 0.0;
    for (std::size_t i = 0; i + 2 < position->values.size(); i += position->components) {
        const float x = position->values[i];
        const float y = position->values[i + 1];
        const float z = position->values[i + 2];
        bounds_min[0] = std::min(bounds_min[0], x);
        bounds_min[1] = std::min(bounds_min[1], y);
        bounds_min[2] = std::min(bounds_min[2], z);
        bounds_max[0] = std::max(bounds_max[0], x);
        bounds_max[1] = std::max(bounds_max[1], y);
        bounds_max[2] = std::max(bounds_max[2], z);
        radius = std::max(radius, std::hypot(static_cast<double>(x),
                                             std::hypot(static_cast<double>(y), static_cast<double>(z))));
    }
    bounds_radius = static_cast<float>(std::min(radius,
        static_cast<double>(std::numeric_limits<float>::max())));
}

bool generate_normals(Primitive& primitive, std::size_t vertex_count) {
    if (find_channel(primitive, VertexChannel::Semantic::Normal)) return false;
    auto* position = find_channel(primitive, VertexChannel::Semantic::Position);
    if (!position || position->components < 3) return false;
    VertexChannel normal;
    normal.semantic = VertexChannel::Semantic::Normal;
    normal.components = 3;
    normal.values.assign(vertex_count * 3, 0.0f);
    std::vector<std::array<double, 3>> accumulated(vertex_count, {0.0, 0.0, 0.0});
    for (std::size_t k = 0; k + 2 < primitive.indices.size(); k += 3) {
        const auto a = primitive.indices[k];
        const auto b = primitive.indices[k + 1];
        const auto c = primitive.indices[k + 2];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        const float ax = position->values[a * position->components];
        const float ay = position->values[a * position->components + 1];
        const float az = position->values[a * position->components + 2];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
            !std::isfinite(position->values[b * position->components]) ||
            !std::isfinite(position->values[b * position->components + 1]) ||
            !std::isfinite(position->values[b * position->components + 2]) ||
            !std::isfinite(position->values[c * position->components]) ||
            !std::isfinite(position->values[c * position->components + 1]) ||
            !std::isfinite(position->values[c * position->components + 2]))
            throw std::runtime_error("POSITION contains a non-finite value");
        const double ux = static_cast<double>(position->values[b * position->components]) - ax;
        const double uy = static_cast<double>(position->values[b * position->components + 1]) - ay;
        const double uz = static_cast<double>(position->values[b * position->components + 2]) - az;
        const double vx = static_cast<double>(position->values[c * position->components]) - ax;
        const double vy = static_cast<double>(position->values[c * position->components + 1]) - ay;
        const double vz = static_cast<double>(position->values[c * position->components + 2]) - az;
        const double nx = static_cast<double>(uy) * vz - static_cast<double>(uz) * vy;
        const double ny = static_cast<double>(uz) * vx - static_cast<double>(ux) * vz;
        const double nz = static_cast<double>(ux) * vy - static_cast<double>(uy) * vx;
        for (auto index : {a, b, c}) {
            accumulated[index][0] += nx; accumulated[index][1] += ny; accumulated[index][2] += nz;
        }
    }
    bool numeric_fallback = false;
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const double x = accumulated[i][0], y = accumulated[i][1], z = accumulated[i][2];
        const double length = std::sqrt(x * x + y * y + z * z);
        if (std::isfinite(length) && length > 1e-12) {
            normal.values[i * 3] = static_cast<float>(x / length);
            normal.values[i * 3 + 1] = static_cast<float>(y / length);
            normal.values[i * 3 + 2] = static_cast<float>(z / length);
        } else {
            normal.values[i * 3] = 0.0f; normal.values[i * 3 + 1] = 0.0f; normal.values[i * 3 + 2] = 1.0f;
            numeric_fallback = numeric_fallback || !std::isfinite(length);
        }
    }
    primitive.channels.push_back(std::move(normal));
    return numeric_fallback;
}

void generate_tangents(Primitive& primitive, std::size_t vertex_count) {
    if (find_channel(primitive, VertexChannel::Semantic::Tangent)) return;
    auto* position = find_channel(primitive, VertexChannel::Semantic::Position);
    auto* uv = find_channel(primitive, VertexChannel::Semantic::Texcoord, 0);
    auto* normal = find_channel(primitive, VertexChannel::Semantic::Normal);
    if (!position || !uv || !normal || position->components < 3 || uv->components < 2 || normal->components < 3) return;

    std::vector<std::array<float, 3>> tangent(vertex_count, {0, 0, 0});
    std::vector<std::array<float, 3>> bitangent(vertex_count, {0, 0, 0});
    for (std::size_t k = 0; k + 2 < primitive.indices.size(); k += 3) {
        const auto i0 = primitive.indices[k];
        const auto i1 = primitive.indices[k + 1];
        const auto i2 = primitive.indices[k + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) continue;
        const float x1 = position->values[i1 * position->components] - position->values[i0 * position->components];
        const float x2 = position->values[i2 * position->components] - position->values[i0 * position->components];
        const float y1 = position->values[i1 * position->components + 1] - position->values[i0 * position->components + 1];
        const float y2 = position->values[i2 * position->components + 1] - position->values[i0 * position->components + 1];
        const float z1 = position->values[i1 * position->components + 2] - position->values[i0 * position->components + 2];
        const float z2 = position->values[i2 * position->components + 2] - position->values[i0 * position->components + 2];
        const float s1 = uv->values[i1 * uv->components] - uv->values[i0 * uv->components];
        const float s2 = uv->values[i2 * uv->components] - uv->values[i0 * uv->components];
        const float t1 = uv->values[i1 * uv->components + 1] - uv->values[i0 * uv->components + 1];
        const float t2 = uv->values[i2 * uv->components + 1] - uv->values[i0 * uv->components + 1];
        const float denominator = s1 * t2 - s2 * t1;
        if (std::fabs(denominator) < 1e-20f) continue;
        const float r = 1.0f / denominator;
        const std::array<float, 3> t{(x1 * t2 - x2 * t1) * r, (y1 * t2 - y2 * t1) * r, (z1 * t2 - z2 * t1) * r};
        const std::array<float, 3> b{(x2 * s1 - x1 * s2) * r, (y2 * s1 - y1 * s2) * r, (z2 * s1 - z1 * s2) * r};
        for (auto index : {i0, i1, i2}) {
            for (int component = 0; component < 3; ++component) {
                tangent[index][component] += t[component];
                bitangent[index][component] += b[component];
            }
        }
    }

    VertexChannel output;
    output.semantic = VertexChannel::Semantic::Tangent;
    output.components = 4;
    output.values.resize(vertex_count * 4);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const std::array<float, 3> n{normal->values[i * normal->components], normal->values[i * normal->components + 1], normal->values[i * normal->components + 2]};
        auto t = tangent[i];
        const float ndt = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
        for (int component = 0; component < 3; ++component) t[component] -= n[component] * ndt;
        const float length = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (length > 1e-12f) for (float& value : t) value /= length;
        const std::array<float, 3> cross{
            n[1] * t[2] - n[2] * t[1],
            n[2] * t[0] - n[0] * t[2],
            n[0] * t[1] - n[1] * t[0],
        };
        const float handedness = (cross[0] * bitangent[i][0] + cross[1] * bitangent[i][1] + cross[2] * bitangent[i][2]) < 0 ? -1.0f : 1.0f;
        output.values[i * 4] = t[0];
        output.values[i * 4 + 1] = t[1];
        output.values[i * 4 + 2] = t[2];
        output.values[i * 4 + 3] = handedness;
    }
    primitive.channels.push_back(std::move(output));
}

std::vector<std::uint32_t> triangulate(std::vector<std::uint32_t> indices, int mode) {
    if (mode <= 4) return indices;
    std::vector<std::uint32_t> output;
    if (mode == 5) {
        for (std::size_t i = 2; i < indices.size(); ++i) {
            if (i & 1) output.insert(output.end(), {indices[i - 1], indices[i - 2], indices[i]});
            else output.insert(output.end(), {indices[i - 2], indices[i - 1], indices[i]});
        }
    } else if (mode == 6 && indices.size() >= 3) {
        for (std::size_t i = 2; i < indices.size(); ++i) output.insert(output.end(), {indices[0], indices[i - 1], indices[i]});
    }
    return output;
}

void lower_to_placeholder(Primitive& primitive) {
    primitive.source_node = -1;
    primitive.mode = 4;
    primitive.indices = {0, 0, 0};
    primitive.source_channels.clear();
    primitive.channels.clear();
    primitive.custom_attributes.clear();
    primitive.morph_targets.clear();
    primitive.morph_weights.clear();
    auto add_position = [](std::vector<VertexChannel>& channels) {
        VertexChannel position;
        position.semantic = VertexChannel::Semantic::Position;
        position.components = 3;
        position.values = {0.0f, 0.0f, 0.0f};
        channels.push_back(std::move(position));
    };
    add_position(primitive.source_channels);
    add_position(primitive.channels);
}

TextureBinding convert_texture_view(const cgltf_data* data, const cgltf_texture_view& view) {
    TextureBinding binding;
    binding.texture = pointer_index(data->textures, data->textures_count, view.texture);
    binding.texcoord = view.texcoord;
    binding.scale = view.scale;
    binding.strength = view.scale;
    if (view.has_transform) {
        binding.transform.offset = {view.transform.offset[0], view.transform.offset[1]};
        binding.transform.scale = {view.transform.scale[0], view.transform.scale[1]};
        binding.transform.rotation = view.transform.rotation;
        binding.transform.texcoord = view.transform.has_texcoord ? view.transform.texcoord : -1;
    }
    return binding;
}

std::uint64_t count_material_texture_bindings(const cgltf_material& material) {
    std::uint64_t count = 0;
    const auto add = [&count](const cgltf_texture_view& view) { if (view.texture) ++count; };
    if (material.has_pbr_metallic_roughness) {
        add(material.pbr_metallic_roughness.base_color_texture);
        add(material.pbr_metallic_roughness.metallic_roughness_texture);
    }
    if (material.has_pbr_specular_glossiness) {
        add(material.pbr_specular_glossiness.diffuse_texture);
        add(material.pbr_specular_glossiness.specular_glossiness_texture);
    }
    add(material.normal_texture);
    add(material.occlusion_texture);
    add(material.emissive_texture);
    if (material.has_clearcoat) {
        add(material.clearcoat.clearcoat_texture);
        add(material.clearcoat.clearcoat_roughness_texture);
        add(material.clearcoat.clearcoat_normal_texture);
    }
    if (material.has_transmission) add(material.transmission.transmission_texture);
    if (material.has_volume) add(material.volume.thickness_texture);
    if (material.has_specular) {
        add(material.specular.specular_texture);
        add(material.specular.specular_color_texture);
    }
    if (material.has_sheen) {
        add(material.sheen.sheen_color_texture);
        add(material.sheen.sheen_roughness_texture);
    }
    if (material.has_iridescence) {
        add(material.iridescence.iridescence_texture);
        add(material.iridescence.iridescence_thickness_texture);
    }
    if (material.has_anisotropy) add(material.anisotropy.anisotropy_texture);
    if (material.has_diffuse_transmission) {
        add(material.diffuse_transmission.diffuse_transmission_texture);
        add(material.diffuse_transmission.diffuse_transmission_color_texture);
    }
    return count;
}

void populate_source_inventory(const cgltf_data* data, Scene& out) {
    auto& features = out.source_features;
    features.scene_count = data->scenes_count;
    features.node_count = data->nodes_count;
    features.mesh_count = data->meshes_count;
    features.material_count = data->materials_count;
    features.image_count = data->images_count;
    features.sampler_count = data->samplers_count;
    features.texture_count = data->textures_count;
    features.skin_count = data->skins_count;
    features.animation_count = data->animations_count;
    features.camera_count = data->cameras_count;
    features.light_count = data->lights_count;
    inventory_physics_json(std::string_view(data->json, data->json_size), "root", features);

    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto& node = data->nodes[i];
        inventory_extras(data, node.extras, "node[" + std::to_string(i) + "]", features);
        if (node.mesh) ++features.mesh_node_count;
        features.parent_edge_count += node.children_count;
        if (node.has_mesh_gpu_instancing) ++features.gpu_instancing_node_count;
    }

    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
        const auto& mesh = data->meshes[mesh_index];
        inventory_extras(data, mesh.extras, "mesh[" + std::to_string(mesh_index) + "]", features);
        for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
            const auto& primitive = mesh.primitives[primitive_index];
            inventory_extras(data, primitive.extras, "mesh[" + std::to_string(mesh_index) + "].primitive[" + std::to_string(primitive_index) + "]", features);
            ++features.source_primitive_count;
            const int mode = primitive_mode(primitive.type);
            if (mode >= 0 && mode < static_cast<int>(features.primitive_modes.size())) ++features.primitive_modes[static_cast<std::size_t>(mode)];
            for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count; ++attribute_index) {
                const auto& attribute = primitive.attributes[attribute_index];
                switch (attribute.type) {
                    case cgltf_attribute_type_texcoord:
                        ++features.texcoord_attribute_count;
                        if (attribute.index >= 0) features.max_texcoord_set = std::max(features.max_texcoord_set, static_cast<std::uint32_t>(attribute.index));
                        break;
                    case cgltf_attribute_type_color: ++features.color_attribute_count; break;
                    case cgltf_attribute_type_joints: ++features.joint_attribute_count; break;
                    case cgltf_attribute_type_weights: ++features.weight_attribute_count; break;
                    case cgltf_attribute_type_custom: ++features.custom_attribute_count; break;
                    default: break;
                }
            }
            if (primitive.targets_count) {
                ++features.morph_target_primitive_count;
                features.morph_target_count += primitive.targets_count;
            }
            if (primitive.has_draco_mesh_compression) ++features.draco_primitive_count;
        }
    }

    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        features.material_texture_binding_count += count_material_texture_bindings(data->materials[i]);
        if (data->materials[i].has_pbr_specular_glossiness) ++features.pbr_specular_glossiness_material_count;
        if (data->materials[i].has_ior) ++features.ior_material_count;
    }
    features.material_variant_count = data->variants_count;
    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index)
        for (cgltf_size primitive_index = 0; primitive_index < data->meshes[mesh_index].primitives_count; ++primitive_index)
            features.material_variant_mapping_count += data->meshes[mesh_index].primitives[primitive_index].mappings_count;
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        features.joint_count += data->skins[i].joints_count;
        if (data->skins[i].inverse_bind_matrices) ++features.inverse_bind_skin_count;
    }
    for (cgltf_size animation_index = 0; animation_index < data->animations_count; ++animation_index) {
        const auto& animation = data->animations[animation_index];
        features.animation_channel_count += animation.channels_count;
        for (cgltf_size sampler_index = 0; sampler_index < animation.samplers_count; ++sampler_index) {
            switch (animation.samplers[sampler_index].interpolation) {
                case cgltf_interpolation_type_step: ++features.animation_step_sampler_count; break;
                case cgltf_interpolation_type_cubic_spline: ++features.animation_cubic_sampler_count; break;
                default: ++features.animation_linear_sampler_count; break;
            }
        }
        for (cgltf_size channel_index = 0; channel_index < animation.channels_count; ++channel_index) {
            if (animation.channels[channel_index].target_path == cgltf_animation_path_type_weights) ++features.animation_weight_channel_count;
        }
    }
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
        if (data->buffer_views[i].has_meshopt_compression) ++features.meshopt_buffer_view_count;
    }
}

MaterialInfo convert_material(const cgltf_data* data, const cgltf_material& source, std::size_t index,
                              const std::filesystem::path& document_path) {
    MaterialInfo material;
    material.name = source.name ? source.name : "material_" + std::to_string(index);
    if (const auto payload = authored_payload(data, source.extras, "darktide_material")) {
        const std::string location = "material[" + std::to_string(index) + "].extras.darktide_material";
        const auto members = authored_members(*payload, location, {"version", "mode", "resource", "template", "surface", "family", "stream", "variable_overrides", "variables", "textures", "shader_streams", "weapon"});
        authored_version(members, location, "darktide_material");
        const auto mode = authored_string(members, "mode", location);
        if (mode == "generated") {
            if (members.count("resource") || members.count("template") || members.count("family") ||
                members.count("stream") || members.count("variable_overrides"))
                throw std::runtime_error(location + ": generated mode accepts no resource, template, or donor fields");
            material.intent = MaterialInfo::Intent::Generated;
            material.surface_material = authored_string(members, "surface", location, "default");
        } else if (mode == "emissive") {
            if (members.count("resource") || members.count("template") || members.count("family") ||
                members.count("stream") || members.count("variable_overrides"))
                throw std::runtime_error(location + ": emissive mode accepts no resource, template, or donor fields");
            material.intent = MaterialInfo::Intent::Emissive;
            material.surface_material = authored_string(members, "surface", location, "default");
        } else if (mode == "external") {
            if (members.count("template") || members.count("surface") || members.count("family") ||
                members.count("stream") || members.count("variable_overrides"))
                throw std::runtime_error(location + ": external mode accepts no template, surface, or donor fields");
            material.intent = MaterialInfo::Intent::External;
            material.external_resource = authored_string(members, "resource", location);
            if (!compiler::ResourceKey{"material", material.external_resource}.valid())
                throw std::runtime_error(location + ": resource is not a valid material resource identity");
        } else if (mode == "template") {
            if (members.count("resource") || members.count("surface") || members.count("family") ||
                members.count("stream") || members.count("variable_overrides"))
                throw std::runtime_error(location + ": template mode accepts no resource, surface, or donor fields");
            material.intent = MaterialInfo::Intent::Template;
            material.external_resource = authored_string(members, "template", location);
        } else if (mode == "donor") {
            if (members.count("resource") || members.count("template") || members.count("surface"))
                throw std::runtime_error(location + ": donor mode accepts no resource, template, or surface");
            material.intent = MaterialInfo::Intent::Donor;
            material.donor_family = authored_string(members, "family", location);
            if (material.donor_family != "world_surface_blend_v1")
                throw std::runtime_error(location + ": unsupported donor family '" + material.donor_family + "'");
            const auto stream = std::filesystem::path(authored_string(members, "stream", location));
            auto extension = stream.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (extension != ".streamdata" && extension != ".stream")
                throw std::runtime_error(location + ": donor stream must have a .streamdata or .stream extension");
            const auto resolved_stream = stream.is_absolute() ? stream : document_path.parent_path() / stream;
            material.donor_stream_path = std::filesystem::absolute(resolved_stream).lexically_normal().string();
            const auto overrides = members.find("variable_overrides");
            if (overrides == members.end()) throw std::runtime_error(location + ": missing 'variable_overrides'");
            const auto values = authored_members(overrides->second, location + ".variable_overrides",
                                                 {"nm_r_blend", "shared_blend", "bc_blend"});
            for (const auto& [key, value] : values)
                material.donor_variable_overrides.emplace(key, authored_number(value, location + ".variable_overrides." + key));
        } else if (mode == "game_shader") {
            if (members.count("resource") || members.count("template") || members.count("family") ||
                members.count("variable_overrides"))
                throw std::runtime_error(location + ": game_shader mode takes stream, variables, textures, shader_streams and surface");
            material.intent = MaterialInfo::Intent::GameShader;
            const auto stream = std::filesystem::path(authored_string(members, "stream", location));
            const auto resolved_stream = std::filesystem::absolute(stream.is_absolute() ? stream : document_path.parent_path() / stream).lexically_normal();
            material.donor_stream_path = resolved_stream.string();
            if (const auto shaders = members.find("shader_streams"); shaders != members.end()) {
                // relative to the bundle folder the stream lives in (<bundle>/data/xx/<hash>)
                const auto bundle = resolved_stream.parent_path().parent_path().parent_path();
                for (const auto& [key, value] : authored_members(shaders->second, location + ".shader_streams", {})) {
                    AuthoredMembers wrapper; wrapper.emplace("path", value);
                    const auto relative = authored_string(wrapper, "path", location + ".shader_streams." + key);
                    if (key.size() != 16 || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
                        throw std::runtime_error(location + ".shader_streams: keys are 16-digit material hashes");
                    material.shader_streams.emplace(std::stoull(key, nullptr, 16), (bundle / relative).lexically_normal().string());
                }
            }
            if (const auto variables = members.find("variables"); variables != members.end()) {
                for (const auto& [key, value] : authored_members(variables->second, location + ".variables", {}))
                    material.shader_variables.emplace(key, authored_floats(value, location + ".variables." + key));
            }
            if (const auto textures = members.find("textures"); textures != members.end()) {
                for (const auto& [key, value] : authored_members(textures->second, location + ".textures", {})) {
                    AuthoredMembers wrapper; wrapper.emplace("slot", value);
                    const auto slot = authored_string(wrapper, "slot", location + ".textures." + key);
                    if (slot != "base_color" && slot != "normal" && slot != "orm" && slot != "emissive")
                        throw std::runtime_error(location + ".textures." + key + ": use base_color, normal, orm or emissive");
                    material.shader_textures.emplace(key, slot);
                }
            }
            material.surface_material = authored_string(members, "surface", location, "default");
        } else {
            throw std::runtime_error(location + ": mode must be generated, emissive, external, template, donor, or game_shader");
        }
        if (const auto weapon = members.find("weapon"); weapon != members.end()) {
            if (material.intent != MaterialInfo::Intent::Generated)
                throw std::runtime_error(location + ": weapon applies to generated materials only");
            if (weapon->second != "true" && weapon->second != "false")
                throw std::runtime_error(location + ": weapon must be true or false");
            material.weapon = weapon->second == "true";
        }
    }
    material.double_sided = source.double_sided != 0;
    material.alpha_cutoff = source.alpha_cutoff;
    material.alpha_mode = source.alpha_mode == cgltf_alpha_mode_mask ? "MASK" : source.alpha_mode == cgltf_alpha_mode_blend ? "BLEND" : "OPAQUE";
    material.unlit = source.unlit != 0;
    std::copy(source.emissive_factor, source.emissive_factor + 3, material.emissive.begin());

    material.has_pbr_specular_glossiness = source.has_pbr_specular_glossiness != 0;
    material.has_ior = source.has_ior != 0;
    if (source.has_pbr_metallic_roughness) {
        std::copy(source.pbr_metallic_roughness.base_color_factor,
                  source.pbr_metallic_roughness.base_color_factor + 4,
                  material.base_color.begin());
        material.metallic = source.pbr_metallic_roughness.metallic_factor;
        material.roughness = source.pbr_metallic_roughness.roughness_factor;
        material.base_color_texture = convert_texture_view(data, source.pbr_metallic_roughness.base_color_texture);
        material.metallic_roughness_texture = convert_texture_view(data, source.pbr_metallic_roughness.metallic_roughness_texture);
    }
    if (source.has_pbr_specular_glossiness) {
        std::copy(source.pbr_specular_glossiness.diffuse_factor,
                  source.pbr_specular_glossiness.diffuse_factor + 4,
                  material.diffuse_factor.begin());
        std::copy(source.pbr_specular_glossiness.specular_factor,
                  source.pbr_specular_glossiness.specular_factor + 3,
                  material.specular_factor.begin());
        material.glossiness_factor = source.pbr_specular_glossiness.glossiness_factor;
        material.diffuse_texture = convert_texture_view(data, source.pbr_specular_glossiness.diffuse_texture);
        material.specular_glossiness_texture = convert_texture_view(data, source.pbr_specular_glossiness.specular_glossiness_texture);
    }
    material.normal_texture = convert_texture_view(data, source.normal_texture);
    material.occlusion_texture = convert_texture_view(data, source.occlusion_texture);
    material.emissive_texture = convert_texture_view(data, source.emissive_texture);

    if (source.has_ior) material.ior = source.ior.ior;
    if (source.has_emissive_strength) material.emissive_strength = source.emissive_strength.emissive_strength;

    material.has_clearcoat = source.has_clearcoat != 0;
    if (source.has_clearcoat) {
        material.clearcoat = source.clearcoat.clearcoat_factor;
        material.clearcoat_roughness = source.clearcoat.clearcoat_roughness_factor;
        material.clearcoat_texture = convert_texture_view(data, source.clearcoat.clearcoat_texture);
        material.clearcoat_roughness_texture = convert_texture_view(data, source.clearcoat.clearcoat_roughness_texture);
        material.clearcoat_normal_texture = convert_texture_view(data, source.clearcoat.clearcoat_normal_texture);
    }

    material.has_transmission = source.has_transmission != 0;
    if (source.has_transmission) {
        material.transmission = source.transmission.transmission_factor;
        material.transmission_texture = convert_texture_view(data, source.transmission.transmission_texture);
    }

    material.has_volume = source.has_volume != 0;
    if (source.has_volume) {
        material.thickness = source.volume.thickness_factor;
        std::copy(source.volume.attenuation_color, source.volume.attenuation_color + 3, material.attenuation_color.begin());
        material.attenuation_distance = source.volume.attenuation_distance;
        material.thickness_texture = convert_texture_view(data, source.volume.thickness_texture);
    }

    material.has_sheen = source.has_sheen != 0;
    if (source.has_sheen) {
        material.sheen_roughness = source.sheen.sheen_roughness_factor;
        std::copy(source.sheen.sheen_color_factor, source.sheen.sheen_color_factor + 3, material.sheen_color.begin());
        material.sheen_color_texture = convert_texture_view(data, source.sheen.sheen_color_texture);
        material.sheen_roughness_texture = convert_texture_view(data, source.sheen.sheen_roughness_texture);
    }

    material.has_specular = source.has_specular != 0;
    if (source.has_specular) {
        material.specular = source.specular.specular_factor;
        std::copy(source.specular.specular_color_factor, source.specular.specular_color_factor + 3, material.specular_color.begin());
        material.specular_texture = convert_texture_view(data, source.specular.specular_texture);
        material.specular_color_texture = convert_texture_view(data, source.specular.specular_color_texture);
    }

    material.has_iridescence = source.has_iridescence != 0;
    if (source.has_iridescence) {
        material.iridescence = source.iridescence.iridescence_factor;
        material.iridescence_ior = source.iridescence.iridescence_ior;
        material.iridescence_thickness_min = source.iridescence.iridescence_thickness_min;
        material.iridescence_thickness_max = source.iridescence.iridescence_thickness_max;
        material.iridescence_texture = convert_texture_view(data, source.iridescence.iridescence_texture);
        material.iridescence_thickness_texture = convert_texture_view(data, source.iridescence.iridescence_thickness_texture);
    }

    material.has_anisotropy = source.has_anisotropy != 0;
    if (source.has_anisotropy) {
        material.anisotropy_strength = source.anisotropy.anisotropy_strength;
        material.anisotropy_rotation = source.anisotropy.anisotropy_rotation;
        material.anisotropy_texture = convert_texture_view(data, source.anisotropy.anisotropy_texture);
    }

    material.has_diffuse_transmission = source.has_diffuse_transmission != 0;
    if (source.has_diffuse_transmission) {
        material.diffuse_transmission = source.diffuse_transmission.diffuse_transmission_factor;
        std::copy(source.diffuse_transmission.diffuse_transmission_color_factor,
                  source.diffuse_transmission.diffuse_transmission_color_factor + 3,
                  material.diffuse_transmission_color.begin());
        material.diffuse_transmission_texture = convert_texture_view(data, source.diffuse_transmission.diffuse_transmission_texture);
        material.diffuse_transmission_color_texture = convert_texture_view(data, source.diffuse_transmission.diffuse_transmission_color_texture);
    }

    material.has_dispersion = source.has_dispersion != 0;
    if (source.has_dispersion) material.dispersion = source.dispersion.dispersion;
    return material;
}

std::set<std::size_t> active_texture_indices(const Scene& scene) {
    std::set<std::size_t> material_indices;
    for (const auto& primitive : scene.primitives) {
        if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < scene.materials.size())
            material_indices.insert(static_cast<std::size_t>(primitive.material));
    }
    std::set<std::size_t> textures;
    for (const auto material_index : material_indices) {
        const auto& material = scene.materials[material_index];
        const std::array<const TextureBinding*,21> bindings{{
            &material.base_color_texture, &material.metallic_roughness_texture,
            &material.normal_texture, &material.occlusion_texture, &material.emissive_texture,
            &material.diffuse_texture, &material.specular_glossiness_texture,
            &material.clearcoat_texture, &material.clearcoat_roughness_texture, &material.clearcoat_normal_texture,
            &material.transmission_texture, &material.thickness_texture,
            &material.sheen_color_texture, &material.sheen_roughness_texture,
            &material.specular_texture, &material.specular_color_texture,
            &material.iridescence_texture, &material.iridescence_thickness_texture,
            &material.anisotropy_texture, &material.diffuse_transmission_texture,
            &material.diffuse_transmission_color_texture}};
        for (const auto* binding : bindings) {
            if (binding->texture >= 0 && static_cast<std::size_t>(binding->texture) < scene.textures.size())
                textures.insert(static_cast<std::size_t>(binding->texture));
        }
    }
    return textures;
}

std::set<std::size_t> active_image_indices(const Scene& scene) {
    std::set<std::size_t> images;
    for (const auto texture_index : active_texture_indices(scene)) {
        const auto& texture = scene.textures[texture_index];
            for (const int source : {texture.basisu_source, texture.webp_source, texture.dds_source, texture.source})
                if (source >= 0) images.insert(static_cast<std::size_t>(source));
    }
    return images;
}

std::set<std::size_t> optional_active_image_indices(const Scene& scene) {
    std::set<std::size_t> optional;
    std::set<std::size_t> mandatory;
    for (const auto texture_index : active_texture_indices(scene)) {
        const auto& texture = scene.textures[texture_index];
        std::set<std::size_t> sources;
        for (const int source : {texture.basisu_source, texture.webp_source, texture.dds_source, texture.source})
            if (source >= 0) sources.insert(static_cast<std::size_t>(source));
        if (sources.size() > 1) optional.insert(sources.begin(), sources.end());
        else mandatory.insert(sources.begin(), sources.end());
    }
    for (const auto image : mandatory) optional.erase(image);
    return optional;
}

std::set<std::size_t> populate_active_extensions(
    const cgltf_data* data, Scene& out, const LoadOptions& options,
    const std::vector<CgltfDocument::MeshoptCoreFallback>& meshopt_fallbacks) {
    std::set<std::string> names;
    std::set<std::size_t> active_buffer_views;
    const auto add = [&](const cgltf_extension* extensions, cgltf_size count) {
        for (cgltf_size i = 0; i < count; ++i) if (extensions[i].name) names.emplace(extensions[i].name);
    };
    const auto add_view = [&](const cgltf_buffer_view* view) {
        if (!view) return;
        const int view_index = pointer_index(data->buffer_views, data->buffer_views_count, view);
        if (view_index >= 0) {
            active_buffer_views.insert(static_cast<std::size_t>(view_index));
            for (const auto& fallback : meshopt_fallbacks)
                if (fallback.buffer_view_index == static_cast<std::size_t>(view_index)) names.emplace(fallback.extension);
        }
        add(view->extensions, view->extensions_count);
        if (view->buffer) add(view->buffer->extensions, view->buffer->extensions_count);
        if (view->has_meshopt_compression) {
            names.emplace(view->meshopt_compression.is_khr ? "KHR_meshopt_compression" : "EXT_meshopt_compression");
            if (view->meshopt_compression.buffer)
                add(view->meshopt_compression.buffer->extensions, view->meshopt_compression.buffer->extensions_count);
        }
    };
    const auto add_accessor = [&](const cgltf_accessor* accessor) {
        if (!accessor) return;
        add(accessor->extensions, accessor->extensions_count);
        add_view(accessor->buffer_view);
        if (accessor->is_sparse) {
            add_view(accessor->sparse.indices_buffer_view);
            add_view(accessor->sparse.values_buffer_view);
        }
    };

    add(data->asset.extensions, data->asset.extensions_count);
    add(data->data_extensions, data->data_extensions_count);
    const cgltf_scene* selected = out.selected_scene >= 0 ? &data->scenes[static_cast<cgltf_size>(out.selected_scene)] :
        (data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr));
    if (selected) add(selected->extensions, selected->extensions_count);

    std::set<int> active_nodes;
    std::vector<int> pending(out.scene_roots.begin(), out.scene_roots.end());
    while (!pending.empty()) {
        const int index = pending.back(); pending.pop_back();
        if (index < 0 || static_cast<cgltf_size>(index) >= data->nodes_count || !active_nodes.insert(index).second) continue;
        const auto& node = data->nodes[static_cast<cgltf_size>(index)];
        add(node.extensions, node.extensions_count);
        if (node.camera) add(node.camera->extensions, node.camera->extensions_count);
        if (node.light) names.emplace("KHR_lights_punctual");
        if (node.skin) {
            add(node.skin->extensions, node.skin->extensions_count);
            add_accessor(node.skin->inverse_bind_matrices);
            for (cgltf_size i = 0; i < node.skin->joints_count; ++i) {
                int joint = pointer_index(data->nodes, data->nodes_count, node.skin->joints[i]);
                while (joint >= 0 && static_cast<cgltf_size>(joint) < data->nodes_count) {
                    if (!active_nodes.insert(joint).second) break;
                    const auto& joint_node = data->nodes[static_cast<cgltf_size>(joint)];
                    add(joint_node.extensions, joint_node.extensions_count);
                    joint = pointer_index(data->nodes, data->nodes_count, joint_node.parent);
                }
            }
        }
        if (node.has_mesh_gpu_instancing) {
            names.emplace("EXT_mesh_gpu_instancing");
            for (cgltf_size i = 0; i < node.mesh_gpu_instancing.attributes_count; ++i)
                add_accessor(node.mesh_gpu_instancing.attributes[i].data);
        }
        for (cgltf_size i = 0; i < node.children_count; ++i)
            pending.push_back(pointer_index(data->nodes, data->nodes_count, node.children[i]));
    }

    std::set<std::size_t> active_materials;
    for (const auto& primitive : out.primitives) {
        if (primitive.source_mesh < 0 || primitive.source_primitive < 0 ||
            static_cast<cgltf_size>(primitive.source_mesh) >= data->meshes_count) continue;
        const auto& mesh = data->meshes[static_cast<cgltf_size>(primitive.source_mesh)];
        add(mesh.extensions, mesh.extensions_count);
        if (static_cast<cgltf_size>(primitive.source_primitive) >= mesh.primitives_count) continue;
        const auto& source = mesh.primitives[static_cast<cgltf_size>(primitive.source_primitive)];
        add(source.extensions, source.extensions_count);
        if (source.has_draco_mesh_compression) {
            names.emplace("KHR_draco_mesh_compression");
            add_view(source.draco_mesh_compression.buffer_view);
        }
        if (source.mappings_count) names.emplace("KHR_materials_variants");
        add_accessor(source.indices);
        for (cgltf_size i = 0; i < source.attributes_count; ++i) add_accessor(source.attributes[i].data);
        for (cgltf_size target = 0; target < source.targets_count; ++target)
            for (cgltf_size i = 0; i < source.targets[target].attributes_count; ++i)
                add_accessor(source.targets[target].attributes[i].data);
        if (primitive.material >= 0) active_materials.insert(static_cast<std::size_t>(primitive.material));
    }
    for (const auto material_index : active_materials) {
        if (material_index >= data->materials_count || material_index >= out.materials.size()) continue;
        const auto& source_material = data->materials[material_index];
        add(source_material.extensions, source_material.extensions_count);
        if (source_material.unlit) names.emplace("KHR_materials_unlit");
        if (source_material.has_pbr_specular_glossiness) names.emplace("KHR_materials_pbrSpecularGlossiness");
        if (source_material.has_clearcoat) names.emplace("KHR_materials_clearcoat");
        if (source_material.has_transmission) names.emplace("KHR_materials_transmission");
        if (source_material.has_volume) names.emplace("KHR_materials_volume");
        if (source_material.has_ior) names.emplace("KHR_materials_ior");
        if (source_material.has_specular) names.emplace("KHR_materials_specular");
        if (source_material.has_sheen) names.emplace("KHR_materials_sheen");
        if (source_material.has_emissive_strength) names.emplace("KHR_materials_emissive_strength");
        if (source_material.has_iridescence) names.emplace("KHR_materials_iridescence");
        if (source_material.has_anisotropy) names.emplace("KHR_materials_anisotropy");
        if (source_material.has_diffuse_transmission) names.emplace("KHR_materials_diffuse_transmission");
        if (source_material.has_dispersion) names.emplace("KHR_materials_dispersion");
        const std::array<const cgltf_texture_view*,21> source_bindings{{
            &source_material.pbr_metallic_roughness.base_color_texture,
            &source_material.pbr_metallic_roughness.metallic_roughness_texture,
            &source_material.normal_texture, &source_material.occlusion_texture,
            &source_material.emissive_texture,
            &source_material.pbr_specular_glossiness.diffuse_texture,
            &source_material.pbr_specular_glossiness.specular_glossiness_texture,
            &source_material.clearcoat.clearcoat_texture,
            &source_material.clearcoat.clearcoat_roughness_texture,
            &source_material.clearcoat.clearcoat_normal_texture,
            &source_material.transmission.transmission_texture,
            &source_material.volume.thickness_texture,
            &source_material.sheen.sheen_color_texture,
            &source_material.sheen.sheen_roughness_texture,
            &source_material.specular.specular_texture,
            &source_material.specular.specular_color_texture,
            &source_material.iridescence.iridescence_texture,
            &source_material.iridescence.iridescence_thickness_texture,
            &source_material.anisotropy.anisotropy_texture,
            &source_material.diffuse_transmission.diffuse_transmission_texture,
            &source_material.diffuse_transmission.diffuse_transmission_color_texture,
        }};
        if (std::any_of(source_bindings.begin(), source_bindings.end(),
                        [](const cgltf_texture_view* binding) { return binding->has_transform != 0; }))
            names.emplace("KHR_texture_transform");
        const auto& material = out.materials[material_index];
        const std::array<const TextureBinding*,21> bindings{{
            &material.base_color_texture, &material.metallic_roughness_texture, &material.normal_texture,
            &material.occlusion_texture, &material.emissive_texture, &material.clearcoat_texture,
            &material.diffuse_texture, &material.specular_glossiness_texture,
            &material.clearcoat_roughness_texture, &material.clearcoat_normal_texture,
            &material.transmission_texture, &material.thickness_texture, &material.sheen_color_texture,
            &material.sheen_roughness_texture, &material.specular_texture, &material.specular_color_texture,
            &material.iridescence_texture, &material.iridescence_thickness_texture,
            &material.anisotropy_texture, &material.diffuse_transmission_texture,
            &material.diffuse_transmission_color_texture}};
        for (const auto* binding : bindings) {
            if (binding->texture < 0 || static_cast<cgltf_size>(binding->texture) >= data->textures_count) continue;
            const auto texture_index = static_cast<std::size_t>(binding->texture);
            const auto& texture = data->textures[texture_index];
            add(texture.extensions, texture.extensions_count);
            if (texture.sampler) add(texture.sampler->extensions, texture.sampler->extensions_count);
            if (texture.has_basisu) names.emplace("KHR_texture_basisu");
            if (texture.has_webp) names.emplace("EXT_texture_webp");
            if (texture.has_dds) names.emplace("MSFT_texture_dds");

            const auto& resolved = out.textures[texture_index];
            const int source_index = resolved.basisu_source >= 0 ? resolved.basisu_source :
                (resolved.webp_source >= 0 ? resolved.webp_source :
                 (resolved.dds_source >= 0 ? resolved.dds_source : resolved.source));
            if (source_index >= 0 && static_cast<cgltf_size>(source_index) < data->images_count) {
                const auto& image = data->images[static_cast<cgltf_size>(source_index)];
                add(image.extensions, image.extensions_count);
                add_view(image.buffer_view);
            }
        }
    }
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        if (options.animation_index && *options.animation_index != static_cast<int>(ai)) continue;
        const auto& animation = data->animations[ai]; bool animation_active = false;
        for (cgltf_size ci = 0; ci < animation.channels_count; ++ci) {
            const auto& channel = animation.channels[ci];
            std::string_view pointer;
            std::string pointer_storage;
            AnimationPointerTarget pointer_target;
            const bool is_pointer = animation_pointer_target(channel, pointer, pointer_storage, pointer_target);
            const int target = channel.target_node ? pointer_index(data->nodes, data->nodes_count, channel.target_node) : -1;
            const bool active = is_pointer ?
                (pointer_target.node < 0 || active_nodes.count(pointer_target.node) != 0) : active_nodes.count(target) != 0;
            if (!active) continue;
            animation_active = true;
            add(channel.extensions, channel.extensions_count);
            if (channel.sampler) {
                add(channel.sampler->extensions, channel.sampler->extensions_count);
                add_accessor(channel.sampler->input); add_accessor(channel.sampler->output);
            }
        }
        if (animation_active) add(animation.extensions, animation.extensions_count);
    }
    out.active_extensions_used.assign(names.begin(), names.end());
    return active_buffer_views;
}

void load_images(const std::filesystem::path& gltf_path, const cgltf_data* data,
                 const std::set<std::size_t>& active_images,
                 const std::set<std::size_t>& optional_images, Scene& out) {
    out.images.reserve(data->images_count);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const auto& source = data->images[i];
        ImageInfo image;
        image.name = source.name ? source.name : "image_" + std::to_string(i);
        if (source.mime_type) image.mime_type = source.mime_type;
        if (source.uri) image.uri = source.uri;
        if (!active_images.count(static_cast<std::size_t>(i))) {
            // Preserve document-wide metadata/indexing without touching an
            // inactive external payload.
        } else if (source.buffer_view) {
            const auto* bytes = cgltf_buffer_view_data(source.buffer_view);
            if (bytes && source.buffer_view->size) image.bytes.assign(bytes, bytes + source.buffer_view->size);
        } else if (!image.uri.empty() && image.uri.starts_with("data:")) {
            image.bytes = decode_data_uri(image.uri, &image.mime_type);
        } else if (!image.uri.empty()) {
            try {
                image.bytes = read_image_file(resolve_image_path(gltf_path, image.uri));
            } catch (const std::exception& exception) {
                if (!optional_images.count(static_cast<std::size_t>(i))) throw;
                image.load_error = exception.what();
            }
        }
        if (image.mime_type.empty()) image.mime_type = infer_image_mime(image.bytes, image.uri);
        out.images.push_back(std::move(image));
    }
}

bool extension_is_required(const Scene& scene, std::string_view extension) {
    return std::find(scene.extensions_required.begin(), scene.extensions_required.end(), extension) !=
        scene.extensions_required.end();
}

bool decode_and_cache_image(ImageInfo& image, std::string& error) {
    if (!image.load_error.empty()) {
        error = image.load_error;
        return false;
    }
    if (image.rgba8) {
        const auto pixels = static_cast<std::uint64_t>(image.rgba8->width) * image.rgba8->height;
        if (image.rgba8->width == 0 || image.rgba8->height == 0 ||
            pixels > std::numeric_limits<std::size_t>::max() / 4u ||
            image.rgba8->bytes.size() != static_cast<std::size_t>(pixels * 4u)) {
            error = "in-memory RGBA8 image has invalid dimensions or pixel payload";
            return false;
        }
        return true;
    }
    stingray::texture::ImageRGBA decoded;
    if (!stingray::texture::decode_image_rgba(image.bytes, image.mime_type, decoded, error)) return false;
    image.rgba8 = RGBA8Payload{decoded.width, decoded.height, std::move(decoded.pixels)};
    return true;
}

bool canonicalize_active_texture_sources(const cgltf_data* data, Scene& scene, std::string& error) {
    struct SourceCandidate {
        int TextureInfo::* member;
        const char* extension;
    };
    static constexpr std::array<SourceCandidate,3> candidates{{
        {&TextureInfo::basisu_source, "KHR_texture_basisu"},
        {&TextureInfo::webp_source, "EXT_texture_webp"},
        {&TextureInfo::dds_source, "MSFT_texture_dds"},
    }};
    struct FailedSource {
        const char* extension = nullptr;
        int source = -1;
        std::string reason;
    };

    for (const auto texture_index : active_texture_indices(scene)) {
        auto& texture = scene.textures[texture_index];
        const auto& source_texture = data->textures[static_cast<cgltf_size>(texture_index)];
        const std::array<bool,3> present{{
            source_texture.has_basisu != 0,
            source_texture.has_webp != 0,
            source_texture.has_dds != 0,
        }};

        // A required alternate source must be genuinely decodable even when
        // another optional alternate appears earlier in the preference order.
        for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
            const auto& candidate = candidates[candidate_index];
            if (!present[candidate_index]) continue;
            const int source = texture.*(candidate.member);
            if (!extension_is_required(scene, candidate.extension)) continue;
            if (source < 0) {
                error = "texture[" + std::to_string(texture_index) + "] required " +
                    candidate.extension + " extension is missing its image source";
                return false;
            }
            if (static_cast<std::size_t>(source) >= scene.images.size()) {
                error = "texture[" + std::to_string(texture_index) + "] required " +
                    candidate.extension + " image index is outside the decoded image table";
                return false;
            }
            std::string decode_error;
            if (!decode_and_cache_image(scene.images[static_cast<std::size_t>(source)], decode_error)) {
                error = "texture[" + std::to_string(texture_index) + "] required " +
                    candidate.extension + " image decode failed: " + decode_error;
                return false;
            }
        }

        std::vector<FailedSource> failed;
        std::string selected = "ordinary core image source";
        bool usable = false;
        for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
            const auto& candidate = candidates[candidate_index];
            if (!present[candidate_index]) continue;
            int& source = texture.*(candidate.member);
            if (source < 0) {
                failed.push_back({candidate.extension, source, "extension image source is absent"});
                continue;
            }
            std::string decode_error;
            if (static_cast<std::size_t>(source) < scene.images.size() &&
                decode_and_cache_image(scene.images[static_cast<std::size_t>(source)], decode_error)) {
                selected = candidate.extension;
                usable = true;
                break;
            }
            if (extension_is_required(scene, candidate.extension)) {
                error = "texture[" + std::to_string(texture_index) + "] required " +
                    candidate.extension + " image decode failed: " +
                    (decode_error.empty() ? "image index is outside the decoded image table" : decode_error);
                return false;
            }
            failed.push_back({candidate.extension, source,
                decode_error.empty() ? "image index is outside the decoded image table" : std::move(decode_error)});
            source = -1;
        }
        if (!usable && texture.source >= 0 && static_cast<std::size_t>(texture.source) < scene.images.size()) {
            std::string core_error;
            usable = decode_and_cache_image(scene.images[static_cast<std::size_t>(texture.source)], core_error);
        }
        if (!usable) continue;
        for (const auto& item : failed) {
            const std::string note =
                "texture[" + std::to_string(texture_index) + "] optional " + item.extension +
                " image[" + std::to_string(item.source) + "] decode failed (" + item.reason +
                "); " + selected + " used";
            scene.source_features.approximation_locations.push_back(note);
            scene.notes.push_back(note);
        }
    }
    return true;
}

void populate_samplers_textures(const cgltf_data* data, Scene& out) {
    out.samplers.reserve(data->samplers_count);
    for (cgltf_size i = 0; i < data->samplers_count; ++i) {
        const auto& source = data->samplers[i];
        SamplerInfo sampler;
        sampler.name = source.name ? source.name : "sampler_" + std::to_string(i);
        sampler.mag_filter = static_cast<int>(source.mag_filter);
        sampler.min_filter = static_cast<int>(source.min_filter);
        sampler.wrap_s = static_cast<int>(source.wrap_s);
        sampler.wrap_t = static_cast<int>(source.wrap_t);
        out.samplers.push_back(std::move(sampler));
    }

    out.textures.reserve(data->textures_count);
    for (cgltf_size i = 0; i < data->textures_count; ++i) {
        const auto& source = data->textures[i];
        TextureInfo texture;
        texture.name = source.name ? source.name : "texture_" + std::to_string(i);
        texture.source = pointer_index(data->images, data->images_count, source.image);
        texture.sampler = pointer_index(data->samplers, data->samplers_count, source.sampler);
        texture.basisu_source = source.has_basisu ? pointer_index(data->images, data->images_count, source.basisu_image) : -1;
        texture.webp_source = source.has_webp ? pointer_index(data->images, data->images_count, source.webp_image) : -1;
        texture.dds_source = source.has_dds ? pointer_index(data->images, data->images_count, source.dds_image) : -1;
        out.textures.push_back(std::move(texture));
    }
}

void populate_cameras_lights(const cgltf_data* data, Scene& out) {
    out.cameras.reserve(data->cameras_count);
    for (cgltf_size i = 0; i < data->cameras_count; ++i) {
        const auto& source = data->cameras[i];
        CameraInfo camera;
        camera.name = source.name ? source.name : "camera_" + std::to_string(i);
        if (source.type == cgltf_camera_type_orthographic) {
            camera.type = CameraInfo::Type::Orthographic;
            camera.xmag = source.data.orthographic.xmag;
            camera.ymag = source.data.orthographic.ymag;
            camera.znear = source.data.orthographic.znear;
            camera.zfar = source.data.orthographic.zfar;
        } else {
            camera.type = CameraInfo::Type::Perspective;
            camera.yfov = source.data.perspective.yfov;
            camera.aspect_ratio = source.data.perspective.has_aspect_ratio ? source.data.perspective.aspect_ratio : 0.0f;
            camera.znear = source.data.perspective.znear;
            camera.zfar = source.data.perspective.has_zfar ? source.data.perspective.zfar : 0.0f;
        }
        out.cameras.push_back(std::move(camera));
    }

    out.lights.reserve(data->lights_count);
    for (cgltf_size i = 0; i < data->lights_count; ++i) {
        const auto& source = data->lights[i];
        LightInfo light;
        light.name = source.name ? source.name : "light_" + std::to_string(i);
        std::copy(source.color, source.color + 3, light.color.begin());
        light.intensity = source.intensity;
        light.range = source.range;
        light.inner_cone_angle = source.spot_inner_cone_angle;
        light.outer_cone_angle = source.spot_outer_cone_angle;
        if (source.type == cgltf_light_type_directional) light.type = LightInfo::Type::Directional;
        else if (source.type == cgltf_light_type_spot) light.type = LightInfo::Type::Spot;
        else light.type = LightInfo::Type::Point;
        out.lights.push_back(std::move(light));
    }
}

void populate_nodes_scenes(const cgltf_data* data, Scene& out, const LoadOptions& options) {
    out.nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const auto& source = data->nodes[i];
        auto& node = out.nodes[i];
        node.name = source.name ? source.name : "node_" + std::to_string(i);
        node.parent = pointer_index(data->nodes, data->nodes_count, source.parent);
        node.mesh = pointer_index(data->meshes, data->meshes_count, source.mesh);
        node.skin = pointer_index(data->skins, data->skins_count, source.skin);
        node.camera = pointer_index(data->cameras, data->cameras_count, source.camera);
        node.light = pointer_index(data->lights, data->lights_count, source.light);
        node.weights.assign(source.weights, source.weights + source.weights_count);
        node.children.reserve(source.children_count);
        for (cgltf_size child_index = 0; child_index < source.children_count; ++child_index) {
            node.children.push_back(pointer_index(data->nodes, data->nodes_count, source.children[child_index]));
        }
        cgltf_float local[16]{};
        cgltf_node_transform_local(&source, local);
        node.local_gltf = from_cgltf_matrix(local);
        node.world_gltf = node.local_gltf;
        node.local_stingray = convert_matrix_basis(node.local_gltf);
        node.world_stingray = node.local_stingray;
        node.has_exact_trs = source.has_matrix == 0;
        node.visible = node_visibility(source);
        if (node.has_exact_trs) {
            const auto translation = convert_vector_basis(source.translation[0], source.translation[1], source.translation[2]);
            node.local_translation_stingray = translation;
            const std::array<float, 4> rotation{source.rotation[0], source.rotation[1], source.rotation[2], source.rotation[3]};
            node.local_rotation_stingray = convert_quaternion_basis(rotation);
            node.local_scale_stingray = {source.scale[0], source.scale[2], source.scale[1]};
        }
        if (source.has_mesh_gpu_instancing) {
            node.instance_attributes.reserve(source.mesh_gpu_instancing.attributes_count);
            for (cgltf_size attribute_index = 0; attribute_index < source.mesh_gpu_instancing.attributes_count; ++attribute_index) {
                const auto& attribute = source.mesh_gpu_instancing.attributes[attribute_index];
                CustomAttribute instance;
                instance.name = attribute.name ? attribute.name : "INSTANCE_ATTRIBUTE_" + std::to_string(attribute_index);
                instance.components = attribute.data ? static_cast<std::uint32_t>(cgltf_num_components(attribute.data->type)) : 0;
                instance.values = accessor_floats(attribute.data);
                node.instance_attributes.push_back(std::move(instance));
            }
        }
    }

    // cgltf's convenience world-transform path performs float matrix products.
    // Compose here in double precision so finite source transforms cannot turn
    // into infinities before the native-profile approximation pass sees them.
    std::vector<unsigned char> world_state(out.nodes.size(), 0);
    std::function<void(std::size_t)> compose_world = [&](std::size_t index) {
        if (world_state[index] == 2) return;
        if (world_state[index] == 1) return; // cgltf_validate reports cycles; keep local as a safe diagnostic fallback.
        world_state[index] = 1;
        auto& node = out.nodes[index];
        bool clamped = false;
        if (node.parent >= 0 && static_cast<std::size_t>(node.parent) < out.nodes.size()) {
            compose_world(static_cast<std::size_t>(node.parent));
            node.world_gltf = multiply(out.nodes[static_cast<std::size_t>(node.parent)].world_gltf,
                                       node.local_gltf, &clamped);
        } else {
            node.world_gltf = node.local_gltf;
        }
        node.world_stingray = convert_matrix_basis(node.world_gltf);
        if (clamped) {
            const std::string detail = node.name +
                ": finite transform hierarchy exceeded float range; clamped matrix products [transform_composition_clamp]";
            out.notes.push_back(detail);
            out.source_features.approximation_locations.push_back(detail);
        }
        world_state[index] = 2;
    };
    for (std::size_t i = 0; i < out.nodes.size(); ++i) compose_world(i);

    out.scenes.reserve(data->scenes_count);
    for (cgltf_size i = 0; i < data->scenes_count; ++i) {
        const auto& source = data->scenes[i];
        SceneInfo scene;
        scene.name = source.name ? source.name : "scene_" + std::to_string(i);
        scene.roots.reserve(source.nodes_count);
        for (cgltf_size root_index = 0; root_index < source.nodes_count; ++root_index) {
            scene.roots.push_back(pointer_index(data->nodes, data->nodes_count, source.nodes[root_index]));
        }
        out.scenes.push_back(std::move(scene));
    }
    out.default_scene = pointer_index(data->scenes, data->scenes_count, data->scene);

    if (options.scene_index) {
        if (*options.scene_index < 0 || static_cast<cgltf_size>(*options.scene_index) >= data->scenes_count)
            throw std::runtime_error("requested scene index is out of range");
        out.selected_scene = *options.scene_index;
        out.scene_roots = out.scenes[static_cast<std::size_t>(out.selected_scene)].roots;
    } else if (data->scene) {
        out.selected_scene = out.default_scene;
        out.scene_roots = out.scenes[static_cast<std::size_t>(out.default_scene)].roots;
    } else if (!out.scenes.empty()) {
        out.selected_scene = 0;
        out.scene_roots = out.scenes.front().roots;
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (!data->nodes[i].parent) out.scene_roots.push_back(static_cast<int>(i));
        }
    }
    out.source_features.root_count = out.scene_roots.size();
}

void populate_skins(const cgltf_data* data, Scene& out) {
    out.skins.reserve(data->skins_count);
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const auto& source = data->skins[i];
        SkinInfo skin;
        skin.name = source.name ? source.name : "skin_" + std::to_string(i);
        skin.skeleton_root = pointer_index(data->nodes, data->nodes_count, source.skeleton);
        skin.joints.reserve(source.joints_count);
        skin.joint_names.reserve(source.joints_count);
        for (cgltf_size joint_index = 0; joint_index < source.joints_count; ++joint_index) {
            const int node_index = pointer_index(data->nodes, data->nodes_count, source.joints[joint_index]);
            skin.joints.push_back(node_index);
            skin.joint_names.push_back(node_index >= 0 ? out.nodes[static_cast<std::size_t>(node_index)].name : "joint_" + std::to_string(joint_index));
        }

        skin.inverse_bind_matrices_gltf.assign(source.joints_count, identity());
        if (source.inverse_bind_matrices) {
            const auto values = accessor_floats(source.inverse_bind_matrices);
            const std::size_t matrix_count = std::min<std::size_t>(source.joints_count, values.size() / 16);
            for (std::size_t matrix_index = 0; matrix_index < matrix_count; ++matrix_index) {
                std::copy(values.begin() + static_cast<std::ptrdiff_t>(matrix_index * 16),
                          values.begin() + static_cast<std::ptrdiff_t>((matrix_index + 1) * 16),
                          skin.inverse_bind_matrices_gltf[matrix_index].begin());
            }
        }
        skin.inverse_bind_matrices_stingray.reserve(skin.inverse_bind_matrices_gltf.size());
        for (const auto& matrix : skin.inverse_bind_matrices_gltf) skin.inverse_bind_matrices_stingray.push_back(convert_matrix_basis(matrix));
        out.skins.push_back(std::move(skin));
    }
}

AnimationInterpolation convert_interpolation(cgltf_interpolation_type interpolation) {
    if (interpolation == cgltf_interpolation_type_step) return AnimationInterpolation::Step;
    if (interpolation == cgltf_interpolation_type_cubic_spline) return AnimationInterpolation::CubicSpline;
    return AnimationInterpolation::Linear;
}

bool convert_animation_path(cgltf_animation_path_type source, AnimationPath& destination) {
    switch (source) {
        case cgltf_animation_path_type_translation: destination = AnimationPath::Translation; return true;
        case cgltf_animation_path_type_rotation: destination = AnimationPath::Rotation; return true;
        case cgltf_animation_path_type_scale: destination = AnimationPath::Scale; return true;
        case cgltf_animation_path_type_weights: destination = AnimationPath::Weights; return true;
        default: return false;
    }
}

void convert_animation_values(AnimationTrack& track) {
    if (track.path == AnimationPath::Translation && track.value_components == 3) {
        for (std::size_t i = 0; i + 2 < track.values.size(); i += 3) {
            const auto converted = convert_vector_basis(track.values[i], track.values[i + 1], track.values[i + 2]);
            track.values[i] = converted[0];
            track.values[i + 1] = converted[1];
            track.values[i + 2] = converted[2];
        }
    } else if (track.path == AnimationPath::Rotation && track.value_components == 4) {
        for (std::size_t i = 0; i + 3 < track.values.size(); i += 4) {
            const std::array<float, 4> source{track.values[i], track.values[i + 1], track.values[i + 2], track.values[i + 3]};
            const auto converted = convert_quaternion_basis(source);
            std::copy(converted.begin(), converted.end(), track.values.begin() + static_cast<std::ptrdiff_t>(i));
        }
    } else if (track.path == AnimationPath::Scale && track.value_components == 3) {
        for (std::size_t i = 0; i + 2 < track.values.size(); i += 3) {
            std::swap(track.values[i + 1], track.values[i + 2]);
        }
    }
}

void populate_animations(const cgltf_data* data, Scene& out) {
    std::set<int> active_nodes;
    std::vector<int> pending(out.scene_roots.begin(), out.scene_roots.end());
    while (!pending.empty()) {
        const int node_index = pending.back();
        pending.pop_back();
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= out.nodes.size() ||
            !active_nodes.insert(node_index).second) continue;
        const auto& children = out.nodes[static_cast<std::size_t>(node_index)].children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    // A skin may legally reference joint nodes that are not descendants of a
    // selected scene root. They still participate in an active skinned mesh,
    // so their animation channels are active runtime semantics.
    const auto selected_scene_nodes = active_nodes;
    for (const int node_index : selected_scene_nodes) {
        const auto& source_node = data->nodes[static_cast<std::size_t>(node_index)];
        if (!source_node.skin) continue;
        for (cgltf_size joint_index = 0; joint_index < source_node.skin->joints_count; ++joint_index) {
            int joint_node = pointer_index(data->nodes, data->nodes_count, source_node.skin->joints[joint_index]);
            while (joint_node >= 0 && static_cast<std::size_t>(joint_node) < out.nodes.size()) {
                if (!active_nodes.insert(joint_node).second) break;
                joint_node = out.nodes[static_cast<std::size_t>(joint_node)].parent;
            }
        }
    }

    out.animations.reserve(data->animations_count);
    for (cgltf_size animation_index = 0; animation_index < data->animations_count; ++animation_index) {
        const auto& source = data->animations[animation_index];
        AnimationInfo animation;
        animation.source_index = static_cast<int>(animation_index);
        animation.name = source.name ? source.name : "animation_" + std::to_string(animation_index);
        animation.tracks.reserve(source.channels_count);
        bool has_active_source_channel = false;
        for (cgltf_size channel_index = 0; channel_index < source.channels_count; ++channel_index) {
            const auto& channel = source.channels[channel_index];
            if (!channel.sampler) continue;
            std::string_view pointer;
            std::string pointer_storage;
            AnimationPointerTarget pointer_target;
            const bool is_pointer = animation_pointer_target(channel, pointer, pointer_storage, pointer_target);
            const int target_node = channel.target_node ? pointer_index(data->nodes, data->nodes_count, channel.target_node) : -1;
            const bool active = is_pointer ?
                (pointer_target.node < 0 || active_nodes.count(pointer_target.node) != 0) : active_nodes.count(target_node) != 0;
            if (!active) continue;
            has_active_source_channel = true;
            ++animation.source_channel_count;
            ++out.source_features.active_animation_channel_count;
            if ((is_pointer ? pointer_target.path : channel.target_path) == cgltf_animation_path_type_weights)
                ++out.source_features.active_animation_weight_channel_count;
            AnimationTrack track;
            const auto source_path = is_pointer ? pointer_target.path : channel.target_path;
            if (is_pointer && (pointer_target.node < 0 || pointer_target.path == cgltf_animation_path_type_invalid)) {
                ++animation.dropped_pointer_channel_count;
                const std::string message = "KHR_animation_pointer channel[" + std::to_string(channel_index) + "] dropped: unsupported property pointer " + std::string(pointer);
                out.source_features.approximation_locations.push_back(message);
                out.notes.push_back(message);
                // Still unpack below so compressed/sparse sampler data is validated and inventoried.
                accessor_floats(channel.sampler->input);
                accessor_floats(channel.sampler->output);
                continue;
            }
            if (!convert_animation_path(source_path, track.path)) continue;
            track.target_node = is_pointer ? pointer_target.node : target_node;
            track.interpolation = convert_interpolation(channel.sampler->interpolation);
            track.times = accessor_floats(channel.sampler->input);
            track.values = accessor_floats(channel.sampler->output);
            if (track.path == AnimationPath::Translation || track.path == AnimationPath::Scale) {
                track.value_components = 3;
            } else if (track.path == AnimationPath::Rotation) {
                track.value_components = 4;
            } else {
                const std::size_t spline_multiplier = track.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
                const std::size_t denominator = track.times.size() * spline_multiplier;
                track.value_components = denominator ? static_cast<std::uint32_t>(track.values.size() / denominator) : 0;
            }
            convert_animation_values(track);
            animation.tracks.push_back(std::move(track));
        }
        if (has_active_source_channel) ++out.source_features.active_animation_count;
        if (!animation.tracks.empty() || animation.dropped_pointer_channel_count != 0)
            out.animations.push_back(std::move(animation));
    }
}

void add_morph_targets(const cgltf_mesh& mesh, const cgltf_primitive& source, Primitive& primitive, std::size_t vertex_count) {
    primitive.morph_targets.reserve(source.targets_count);
    for (cgltf_size target_index = 0; target_index < source.targets_count; ++target_index) {
        const auto& source_target = source.targets[target_index];
        MorphTarget target;
        if (target_index < mesh.target_names_count && mesh.target_names[target_index]) target.name = mesh.target_names[target_index];
        else target.name = "morph_" + std::to_string(target_index);
        for (cgltf_size attribute_index = 0; attribute_index < source_target.attributes_count; ++attribute_index) {
            const auto& attribute = source_target.attributes[attribute_index];
            VertexChannel::Semantic semantic{};
            std::uint32_t set = 0;
            if (!attribute.data || !attribute_semantic(attribute, semantic, set) ||
                (semantic != VertexChannel::Semantic::Position && semantic != VertexChannel::Semantic::Normal && semantic != VertexChannel::Semantic::Tangent)) {
                throw std::runtime_error("morph target contains an unknown attribute semantic");
            }
            if (set != 0 || attribute.data->type != cgltf_type_vec3) {
                throw std::runtime_error("morph target attributes must be VEC3 set 0");
            }
            for (const auto& existing : target.channels) {
                if (existing.semantic == semantic && existing.set == set) {
                    throw std::runtime_error("morph target contains duplicate attribute semantics");
                }
            }
            VertexChannel channel;
            channel.semantic = semantic;
            channel.set = set;
            channel.components = static_cast<std::uint32_t>(cgltf_num_components(attribute.data->type));
            channel.values = accessor_floats(attribute.data);
            if (attribute.data->count != vertex_count ||
                channel.values.size() != static_cast<std::size_t>(attribute.data->count) * 3u ||
                !std::all_of(channel.values.begin(), channel.values.end(), [](float v) { return std::isfinite(v); })) {
                throw std::runtime_error("morph target accessor does not exactly match the base vertex count or contains non-finite values");
            }
            if (channel.components >= 3) {
                for (std::size_t value_index = 0; value_index + 2 < channel.values.size(); value_index += channel.components) {
                    const auto converted = convert_vector_basis(channel.values[value_index], channel.values[value_index + 1], channel.values[value_index + 2]);
                    channel.values[value_index] = converted[0];
                    channel.values[value_index + 1] = converted[1];
                    channel.values[value_index + 2] = converted[2];
                }
            }
            target.channels.push_back(std::move(channel));
        }
        primitive.morph_targets.push_back(std::move(target));
    }
}

std::vector<float> resolve_morph_weights(const cgltf_mesh& mesh, const std::vector<float>& node_weights,
                                         std::size_t target_count) {
    std::vector<float> mesh_weights;
    if (mesh.weights_count) mesh_weights.assign(mesh.weights, mesh.weights + mesh.weights_count);
    const auto& authored = !node_weights.empty() ? node_weights : mesh_weights;
    if (!authored.empty() && authored.size() != target_count) {
        throw std::runtime_error("morph weights have wrong cardinality");
    }
    if (!std::all_of(authored.begin(), authored.end(), [](float v) { return std::isfinite(v); })) {
        throw std::runtime_error("morph weights must be finite");
    }
    return authored.empty() ? std::vector<float>(target_count, 0.0f) : authored;
}

float narrow_finite_float(double value, bool* clamped = nullptr) {
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value)) {
        if (clamped) *clamped = true;
        return std::isnan(value) ? 0.0f : (value < 0.0 ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max());
    }
    if (value > limit) {
        if (clamped) *clamped = true;
        return std::numeric_limits<float>::max();
    }
    if (value < -limit) {
        if (clamped) *clamped = true;
        return -std::numeric_limits<float>::max();
    }
    return static_cast<float>(value);
}

std::array<float, 3> transform_linear(const Mat4& matrix, float x, float y, float z, bool* clamped = nullptr) {
    // Double intermediates avoid float overflow; narrowing is an explicit finite approximation.
    return {narrow_finite_float(static_cast<double>(matrix[0]) * x + static_cast<double>(matrix[4]) * y + static_cast<double>(matrix[8]) * z, clamped),
            narrow_finite_float(static_cast<double>(matrix[1]) * x + static_cast<double>(matrix[5]) * y + static_cast<double>(matrix[9]) * z, clamped),
            narrow_finite_float(static_cast<double>(matrix[2]) * x + static_cast<double>(matrix[6]) * y + static_cast<double>(matrix[10]) * z, clamped)};
}

std::array<float, 3> transform_point_pure(const Mat4& matrix, float x, float y, float z, bool* clamped = nullptr) {
    // Keep translation in the checked double expression so large terms do not overflow float first.
    return {narrow_finite_float(static_cast<double>(matrix[0]) * x + static_cast<double>(matrix[4]) * y +
                                static_cast<double>(matrix[8]) * z + static_cast<double>(matrix[12]), clamped),
            narrow_finite_float(static_cast<double>(matrix[1]) * x + static_cast<double>(matrix[5]) * y +
                                static_cast<double>(matrix[9]) * z + static_cast<double>(matrix[13]), clamped),
            narrow_finite_float(static_cast<double>(matrix[2]) * x + static_cast<double>(matrix[6]) * y +
                                static_cast<double>(matrix[10]) * z + static_cast<double>(matrix[14]), clamped)};
}

std::array<float, 3> transform_inverse_transpose(const Mat4& matrix, float x, float y, float z,
                                                 bool* approximated = nullptr) {
    const double a=matrix[0], b=matrix[4], c=matrix[8], d=matrix[1], e=matrix[5], f=matrix[9],
                 g=matrix[2], h=matrix[6], i=matrix[10];
    const double det = a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    double tx=x, ty=y, tz=z;
    if (std::isfinite(det) && std::fabs(det) > 1e-300) {
        tx=((e*i-f*h)*x + (f*g-d*i)*y + (d*h-e*g)*z) / det;
        ty=((c*h-b*i)*x + (a*i-c*g)*y + (b*g-a*h)*z) / det;
        tz=((b*f-c*e)*x + (c*d-a*f)*y + (a*e-b*d)*z) / det;
    } else if (approximated) *approximated = true;
    const double length = std::hypot(tx, std::hypot(ty, tz));
    if (!std::isfinite(length) || length <= 1e-300) {
        if (approximated) *approximated = true;
        return {0.0f, 0.0f, 1.0f};
    }
    return {static_cast<float>(tx/length), static_cast<float>(ty/length), static_cast<float>(tz/length)};
}

void normalize_orthogonalize(std::vector<VertexChannel>& channels, std::size_t vertex_count,
                             bool* approximated = nullptr) {
    auto* normal = find_channel(channels, VertexChannel::Semantic::Normal);
    auto* tangent = find_channel(channels, VertexChannel::Semantic::Tangent);
    if (normal) for (std::size_t i = 0; i < vertex_count; ++i) {
        const std::size_t offset = i * normal->components;
        const double x = normal->values[offset], y = normal->values[offset + 1], z = normal->values[offset + 2];
        const double length = std::hypot(x, std::hypot(y, z));
        if (std::isfinite(length) && length > 1e-12) {
            normal->values[offset] = static_cast<float>(x / length);
            normal->values[offset + 1] = static_cast<float>(y / length);
            normal->values[offset + 2] = static_cast<float>(z / length);
        } else {
            normal->values[offset] = 0.0f;
            normal->values[offset + 1] = 0.0f;
            normal->values[offset + 2] = 1.0f;
            if (approximated) *approximated = true;
        }
    }
    if (normal && tangent && tangent->components >= 3) for (std::size_t i = 0; i < vertex_count; ++i) {
        const std::size_t no = i * normal->components;
        const std::size_t to = i * tangent->components;
        const double nx = normal->values[no], ny = normal->values[no + 1], nz = normal->values[no + 2];
        const double tx = tangent->values[to], ty = tangent->values[to + 1], tz = tangent->values[to + 2];
        const double dot = nx * tx + ny * ty + nz * tz;
        const double ox = tx - nx * dot, oy = ty - ny * dot, oz = tz - nz * dot;
        const double length = std::hypot(ox, std::hypot(oy, oz));
        if (std::isfinite(length) && length > 1e-12) {
            tangent->values[to] = static_cast<float>(ox / length);
            tangent->values[to + 1] = static_cast<float>(oy / length);
            tangent->values[to + 2] = static_cast<float>(oz / length);
        } else {
            // Pick the least-aligned coordinate axis so the cross product is stable.
            double fx, fy, fz;
            const double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
            if (ax <= ay && ax <= az) { fx = 0.0; fy = nz; fz = -ny; }
            else if (ay <= az) { fx = -nz; fy = 0.0; fz = nx; }
            else { fx = ny; fy = -nx; fz = 0.0; }
            const double fallback_length = std::hypot(fx, std::hypot(fy, fz));
            if (std::isfinite(fallback_length) && fallback_length > 1e-12) {
                tangent->values[to] = static_cast<float>(fx / fallback_length);
                tangent->values[to + 1] = static_cast<float>(fy / fallback_length);
                tangent->values[to + 2] = static_cast<float>(fz / fallback_length);
            } else {
                tangent->values[to] = 1.0f;
                tangent->values[to + 1] = 0.0f;
                tangent->values[to + 2] = 0.0f;
            }
            if (approximated) *approximated = true;
        }
    }
}

void bake_initial_morphs(Primitive& primitive, const Mat4& world, const std::vector<float>& weights,
                         std::size_t vertex_count, Scene& out) {
    if (primitive.morph_targets.empty()) return;
    if (weights.size() != primitive.morph_targets.size()) throw std::runtime_error("morph weights have wrong cardinality");
    const std::size_t baked_target_count = weights.size();
    for (const auto& target : primitive.morph_targets) for (const auto& channel : target.channels) {
        if (channel.values.size() != vertex_count * 3u) throw std::runtime_error("morph target channel has invalid vertex count");
    }
    bool multiplication_clamped = false;
    bool transform_clamped = false;
    bool normalization_approximated = false;
    auto apply = [&](VertexChannel::Semantic wanted) {
        auto* base = find_channel(primitive.source_channels, wanted);
        if (!base) return;
        for (std::size_t i=0; i<vertex_count; ++i) for (int component = 0; component < 3; ++component) {
            const std::size_t offset = i * base->components + static_cast<std::size_t>(component);
            // Accumulate all deltas in double and narrow once, preserving cancellation/endpoints.
            double value = static_cast<double>(base->values[offset]);
            for (std::size_t ti=0; ti<primitive.morph_targets.size(); ++ti) for (const auto& delta : primitive.morph_targets[ti].channels) {
                if (delta.semantic == wanted)
                    value += static_cast<double>(weights[ti]) * static_cast<double>(delta.values[i * 3 + component]);
            }
            base->values[offset] = narrow_finite_float(value, &multiplication_clamped);
        }
    };
    apply(VertexChannel::Semantic::Position);
    Primitive generated = primitive;
    generated.channels = primitive.source_channels;
    generate_normals(generated, vertex_count);
    primitive.source_channels = std::move(generated.channels);
    apply(VertexChannel::Semantic::Normal);
    normalize_orthogonalize(primitive.source_channels, vertex_count, &normalization_approximated);
    generated = primitive;
    generated.channels = primitive.source_channels;
    generate_tangents(generated, vertex_count);
    primitive.source_channels = std::move(generated.channels);
    apply(VertexChannel::Semantic::Tangent);
    normalize_orthogonalize(primitive.source_channels, vertex_count, &normalization_approximated);
    if (multiplication_clamped) {
        const std::string detail = primitive.name +
            ": finite morph weight/delta multiplication exceeded float range; clamped [morph_numeric_clamp]";
        out.notes.push_back(detail);
        out.source_features.approximation_locations.push_back(detail);
    }
    const Mat4 stingray_world = convert_matrix_basis(world);
    primitive.channels.clear();
    primitive.channels.reserve(primitive.source_channels.size());
    for (const auto& source : primitive.source_channels) {
        auto channel = source;
        const bool transform_channel = source.components >= 3 &&
            (source.semantic == VertexChannel::Semantic::Position ||
             source.semantic == VertexChannel::Semantic::Normal ||
             source.semantic == VertexChannel::Semantic::Tangent);
        if (transform_channel) {
            for (std::size_t i=0; i<vertex_count; ++i) {
                const float x=source.values[i*source.components], y=source.values[i*source.components+1], z=source.values[i*source.components+2];
                std::array<float,3> value;
                if (source.semantic == VertexChannel::Semantic::Position) value = transform_point_pure(stingray_world, x,y,z, &transform_clamped);
                else if (source.semantic == VertexChannel::Semantic::Normal) value = transform_inverse_transpose(stingray_world, x,y,z, &transform_clamped);
                else value = transform_linear(stingray_world, x,y,z, &transform_clamped);
                channel.values[i*channel.components]=value[0]; channel.values[i*channel.components+1]=value[1]; channel.values[i*channel.components+2]=value[2];
            }
        }
        if (channel.semantic == VertexChannel::Semantic::Normal) {
            // normalize after inverse-transpose conversion below
        } else if (channel.semantic == VertexChannel::Semantic::Tangent && channel.components >= 4 && determinant3(world) < 0) {
            for (std::size_t i=0; i<vertex_count; ++i) channel.values[i*channel.components+3] = -channel.values[i*channel.components+3];
        }
        primitive.channels.push_back(std::move(channel));
    }
    if (transform_clamped) {
        const std::string detail = primitive.name +
            ": finite morph world transform exceeded numeric range; used a finite fallback [morph_numeric_clamp]";
        out.notes.push_back(detail);
        out.source_features.approximation_locations.push_back(detail);
    }
    normalize_orthogonalize(primitive.channels, vertex_count, &normalization_approximated);
    normalize_orthogonalize(primitive.source_channels, vertex_count, &normalization_approximated);
    if (normalization_approximated) {
        const std::string detail = primitive.name +
            ": finite morph NORMAL/TANGENT normalization used a deterministic unit fallback [morph_numeric_clamp]";
        out.notes.push_back(detail);
        out.source_features.approximation_locations.push_back(detail);
    }
    primitive.morph_targets.clear(); primitive.morph_weights.clear();
    out.source_features.fixed_initial_morph_bake_count += baked_target_count;
    out.source_features.fixed_initial_morph_bake_succeeded = true;
}

bool primitive_uses_meshopt(const cgltf_primitive& primitive) {
    if (accessor_uses_meshopt(primitive.indices)) return true;
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) if (accessor_uses_meshopt(primitive.attributes[i].data)) return true;
    for (cgltf_size target_index = 0; target_index < primitive.targets_count; ++target_index) {
        const auto& target = primitive.targets[target_index];
        for (cgltf_size i = 0; i < target.attributes_count; ++i) if (accessor_uses_meshopt(target.attributes[i].data)) return true;
    }
    return false;
}

std::vector<float> remap_vertex_rows(const std::vector<float>& values,
                                     std::uint32_t components,
                                     const std::vector<std::uint32_t>& source_vertices) {
    if (components == 0) throw std::runtime_error("vertex channel has zero components while splitting");
    std::vector<float> result;
    result.reserve(source_vertices.size() * components);
    for (const auto source_vertex : source_vertices) {
        const auto begin = static_cast<std::size_t>(source_vertex) * components;
        if (begin > values.size() || components > values.size() - begin) {
            throw std::runtime_error("vertex channel is shorter than the indexed geometry while splitting");
        }
        result.insert(result.end(), values.begin() + static_cast<std::ptrdiff_t>(begin),
                      values.begin() + static_cast<std::ptrdiff_t>(begin + components));
    }
    return result;
}

void append_uint16_safe_primitives(Primitive primitive, std::size_t vertex_count, Scene& out) {
    const bool already_safe = vertex_count <= 65536u &&
        std::all_of(primitive.indices.begin(), primitive.indices.end(),
                    [](std::uint32_t index) { return index <= 65535u; });
    if (already_safe || primitive.mode < 4 || primitive.mode > 6) {
        out.primitives.push_back(std::move(primitive));
        return;
    }
    if (primitive.indices.empty() || primitive.indices.size() % 3u != 0u) {
        throw std::runtime_error("triangle primitive cannot be partitioned because its index count is invalid");
    }

    struct Chunk {
        std::vector<std::uint32_t> source_vertices;
        std::vector<std::uint32_t> indices;
    };
    std::vector<Chunk> chunks;
    Chunk chunk;
    std::unordered_map<std::uint32_t, std::uint32_t> local_index;
    auto flush = [&]() {
        if (chunk.indices.empty()) return;
        chunks.push_back(std::move(chunk));
        chunk = {};
        local_index.clear();
    };
    for (std::size_t triangle = 0; triangle < primitive.indices.size(); triangle += 3u) {
        std::size_t new_vertices = 0;
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            const auto source_vertex = primitive.indices[triangle + corner];
            if (source_vertex >= vertex_count) throw std::runtime_error("primitive index exceeds vertex count");
            if (!local_index.count(source_vertex)) ++new_vertices;
        }
        if (!chunk.indices.empty() && local_index.size() + new_vertices > 65536u) flush();
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            const auto source_vertex = primitive.indices[triangle + corner];
            auto [it, inserted] = local_index.emplace(source_vertex, static_cast<std::uint32_t>(local_index.size()));
            if (inserted) chunk.source_vertices.push_back(source_vertex);
            chunk.indices.push_back(it->second);
        }
    }
    flush();

    for (std::size_t part_index = 0; part_index < chunks.size(); ++part_index) {
        const auto& source_vertices = chunks[part_index].source_vertices;
        Primitive part;
        part.name = primitive.name + "_part" + std::to_string(part_index);
        part.material_name = primitive.material_name;
        part.source_node = primitive.source_node;
        part.render_owner_node = primitive.render_owner_node;
        part.source_mesh = primitive.source_mesh;
        part.source_primitive = primitive.source_primitive;
        part.collision_object = primitive.collision_object;
        part.collision_instance = primitive.collision_instance;
        part.material = primitive.material;
        part.mode = 4;
        part.indices = chunks[part_index].indices;
        part.morph_weights = primitive.morph_weights;
        for (const auto& channel : primitive.source_channels) {
            auto remapped = channel;
            remapped.values = remap_vertex_rows(channel.values, channel.components, source_vertices);
            part.source_channels.push_back(std::move(remapped));
        }
        for (const auto& channel : primitive.channels) {
            auto remapped = channel;
            remapped.values = remap_vertex_rows(channel.values, channel.components, source_vertices);
            part.channels.push_back(std::move(remapped));
        }
        for (const auto& attribute : primitive.custom_attributes) {
            auto remapped = attribute;
            remapped.values = remap_vertex_rows(attribute.values, attribute.components, source_vertices);
            part.custom_attributes.push_back(std::move(remapped));
        }
        for (const auto& target : primitive.morph_targets) {
            MorphTarget remapped_target;
            remapped_target.name = target.name;
            for (const auto& channel : target.channels) {
                auto remapped = channel;
                remapped.values = remap_vertex_rows(channel.values, channel.components, source_vertices);
                remapped_target.channels.push_back(std::move(remapped));
            }
            part.morph_targets.push_back(std::move(remapped_target));
        }
        calculate_bounds(part.source_channels, part.source_bounds_min, part.source_bounds_max, part.source_bounds_radius);
        calculate_bounds(part.channels, part.bounds_min, part.bounds_max, part.bounds_radius);
        out.primitives.push_back(std::move(part));
    }
}

void add_primitive(const cgltf_data* data,
                   const cgltf_mesh& mesh,
                   const cgltf_primitive& source,
                   const LoadOptions& options,
                   const Mat4& world,
                   std::string name,
                   const std::vector<float>& instance_weights,
                   Scene& out,
                   int source_node,
                   int source_mesh,
                   int source_primitive,
                   int collision_object,
                   int collision_instance) {
    if (primitive_uses_meshopt(source)) return;

    Primitive primitive;
    primitive.name = std::move(name);
    primitive.source_node = source_node;
    primitive.source_mesh = source_mesh;
    primitive.source_primitive = source_primitive;
    primitive.collision_object = collision_object;
    primitive.collision_instance = collision_instance;
    primitive.mode = primitive_mode(source.type);
    primitive.material = pointer_index(data->materials, data->materials_count,
                                       effective_material(data, source, options));
    if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < out.materials.size()) {
        primitive.material_name = out.materials[static_cast<std::size_t>(primitive.material)].name;
    }
    bool primitive_approximated = false;
    bool world_position_clamped = false;
    const auto record_approximation = [&](const std::string& detail) {
        if (!primitive_approximated) {
            ++out.source_features.approximated_primitive_count;
            primitive_approximated = true;
        }
        const std::string location = primitive.name + ": " + detail;
        out.source_features.approximation_locations.push_back(location);
        out.notes.push_back(location);
    };

    DracoPrimitiveData draco;
    if (source.has_draco_mesh_compression) {
        std::string draco_error;
        if (!decode_draco_primitive(data, source, draco, draco_error)) {
            throw std::runtime_error(draco_error);
        }
        if (draco.used_core_fallback)
            record_approximation("optional KHR_draco_mesh_compression decode failed; ordinary core accessors used");
    }

    std::size_t vertex_count = source.has_draco_mesh_compression ? draco.vertex_count : 0;
    for (cgltf_size attribute_index = 0; attribute_index < source.attributes_count; ++attribute_index) {
        const auto& attribute = source.attributes[attribute_index];
        if (!attribute.data) continue;

        std::vector<float> values;
        if (source.has_draco_mesh_compression &&
            static_cast<std::size_t>(attribute_index) < draco.attribute_values.size() &&
            !draco.attribute_values[static_cast<std::size_t>(attribute_index)].empty()) {
            values = draco.attribute_values[static_cast<std::size_t>(attribute_index)];
        } else {
            values = accessor_floats(attribute.data);
        }

        if (attribute.type == cgltf_attribute_type_custom) {
            CustomAttribute custom;
            custom.name = attribute.name ? attribute.name : "_CUSTOM_" + std::to_string(attribute_index);
            custom.components = static_cast<std::uint32_t>(cgltf_num_components(attribute.data->type));
            custom.values = std::move(values);
            primitive.custom_attributes.push_back(std::move(custom));
            continue;
        }
        VertexChannel::Semantic semantic{};
        std::uint32_t set = 0;
        if (!attribute_semantic(attribute, semantic, set)) continue;
        const std::uint32_t components = static_cast<std::uint32_t>(cgltf_num_components(attribute.data->type));
        if (components == 0 || components > 4) continue;
        if (values.empty() && attribute.data->count) continue;
        vertex_count = std::max(vertex_count, static_cast<std::size_t>(attribute.data->count));

        VertexChannel source_channel;
        source_channel.semantic = semantic;
        source_channel.set = set;
        source_channel.components = components;
        source_channel.values = values;
        VertexChannel world_channel = source_channel;

        if (components >= 3 && semantic == VertexChannel::Semantic::Position) {
            for (std::size_t i = 0; i + 2 < values.size(); i += components) {
                const auto local_value = convert_vector_basis(values[i], values[i + 1], values[i + 2]);
                source_channel.values[i] = local_value[0];
                source_channel.values[i + 1] = local_value[1];
                source_channel.values[i + 2] = local_value[2];
                const auto world_value = transform_point(world, values[i], values[i + 1], values[i + 2],
                                                         &world_position_clamped);
                world_channel.values[i] = world_value[0];
                world_channel.values[i + 1] = world_value[1];
                world_channel.values[i + 2] = world_value[2];
            }
        } else if (components >= 3 && (semantic == VertexChannel::Semantic::Normal || semantic == VertexChannel::Semantic::Tangent)) {
            for (std::size_t i = 0; i + 2 < values.size(); i += components) {
                const auto local_value = semantic == VertexChannel::Semantic::Normal
                    ? transform_normal(identity(), values[i], values[i + 1], values[i + 2])
                    : transform_tangent(identity(), values[i], values[i + 1], values[i + 2]);
                source_channel.values[i] = local_value[0];
                source_channel.values[i + 1] = local_value[1];
                source_channel.values[i + 2] = local_value[2];
                const auto world_value = semantic == VertexChannel::Semantic::Normal
                    ? transform_normal(world, values[i], values[i + 1], values[i + 2])
                    : transform_tangent(world, values[i], values[i + 1], values[i + 2]);
                world_channel.values[i] = world_value[0];
                world_channel.values[i + 1] = world_value[1];
                world_channel.values[i + 2] = world_value[2];
                if (semantic == VertexChannel::Semantic::Tangent && components >= 4 && determinant3(world) < 0) {
                    world_channel.values[i + 3] = -world_channel.values[i + 3];
                }
            }
        }
        primitive.source_channels.push_back(std::move(source_channel));
        primitive.channels.push_back(std::move(world_channel));
    }
    if (world_position_clamped)
        record_approximation("finite transformed POSITION exceeded float range; clamped to finite limits");

    bool placeholder_lowered = false;
    if (!find_channel(primitive, VertexChannel::Semantic::Position) || vertex_count == 0) {
        lower_to_placeholder(primitive);
        record_approximation("missing POSITION/vertices lowered to a one-vertex static origin placeholder");
        vertex_count = 1;
        placeholder_lowered = true;
    }
    if (!placeholder_lowered) {
        if (source.has_draco_mesh_compression && !draco.used_core_fallback) {
            primitive.indices = std::move(draco.indices);
        } else {
            auto indices = accessor_indices(source.indices, vertex_count);
            if (source.indices && source.indices->count && indices.empty())
                throw std::runtime_error("primitive index accessor is malformed or out of bounds");
            primitive.indices = triangulate(std::move(indices), primitive.mode);
        }
        for (const auto index : primitive.indices)
            if (index >= vertex_count) throw std::runtime_error("primitive index exceeds decoded vertex count");
        if (primitive.mode == 4) {
            const auto complete = primitive.indices.size() - (primitive.indices.size() % 3u);
            if (primitive.indices.size() != complete) {
                primitive.indices.resize(complete);
                record_approximation("discarded incomplete TRIANGLES tail");
            }
            if (primitive.indices.empty()) {
                lower_to_placeholder(primitive);
                record_approximation("underfilled TRIANGLES lowered to a one-vertex static origin placeholder");
                vertex_count = 1;
                placeholder_lowered = true;
            }
        } else if (primitive.indices.empty()) {
            lower_to_placeholder(primitive);
            record_approximation("underfilled topology lowered to a one-vertex static origin placeholder");
            vertex_count = 1;
            placeholder_lowered = true;
        }
    }
    if (primitive.mode == 5 || primitive.mode == 6) primitive.mode = 4;
    if (primitive.mode == 4 && determinant3(world) < 0) {
        for (std::size_t i = 0; i + 2 < primitive.indices.size(); i += 3) std::swap(primitive.indices[i + 1], primitive.indices[i + 2]);
    }

    if (!placeholder_lowered) {
        add_morph_targets(mesh, source, primitive, vertex_count);
        out.source_features.active_morph_target_count += primitive.morph_targets.size();
        primitive.morph_weights = resolve_morph_weights(mesh, instance_weights, primitive.morph_targets.size());
    }

    if (primitive.morph_targets.empty()) {
        if (generate_normals(primitive, vertex_count))
            record_approximation("generated NORMAL overflowed finite geometry range; used deterministic (0,0,1) fallback");
        generate_tangents(primitive, vertex_count);
    }
    // Skinning consumes node-local source channels. Keep generated basis channels
    // consistent with the world-baked static representation above.
    if (!primitive.source_channels.empty() && primitive.morph_targets.empty()) {
        Primitive source_view = primitive;
        source_view.channels = primitive.source_channels;
        if (generate_normals(source_view, vertex_count))
            record_approximation("generated source NORMAL overflowed finite geometry range; used deterministic (0,0,1) fallback");
        generate_tangents(source_view, vertex_count);
        primitive.source_channels = std::move(source_view.channels);
    }
    if (primitive.morph_targets.empty()) {
        bool normalization_approximated = false;
        normalize_orthogonalize(primitive.channels, vertex_count, &normalization_approximated);
        normalize_orthogonalize(primitive.source_channels, vertex_count, &normalization_approximated);
        if (normalization_approximated)
            record_approximation("NORMAL/TANGENT normalization used a deterministic unit fallback");
    }
    bake_initial_morphs(primitive, world, primitive.morph_weights, vertex_count, out);
    if (primitive.morph_targets.empty() && !primitive.morph_weights.empty()) primitive.morph_weights.clear();
    calculate_bounds(primitive.source_channels, primitive.source_bounds_min, primitive.source_bounds_max, primitive.source_bounds_radius);
    calculate_bounds(primitive.channels, primitive.bounds_min, primitive.bounds_max, primitive.bounds_radius);
    const auto output_count_before = out.primitives.size();
    append_uint16_safe_primitives(std::move(primitive), vertex_count, out);
    if (out.primitives.size() > output_count_before) {
        ++out.source_features.decoded_source_primitive_count;
        out.source_features.active_material_variant_mapping_count += source.mappings_count;
    }
}

void add_mesh_instance(const cgltf_data* data,
                       const cgltf_node* node,
                       const cgltf_mesh& mesh,
                       const LoadOptions& options,
                       const Mat4& world,
                       std::size_t instance_index,
                       Scene& out,
                       std::set<int>& used_meshes) {
    const int mesh_index = static_cast<int>(&mesh - data->meshes);
    used_meshes.insert(mesh_index);
    const int node_index = node ? pointer_index(data->nodes, data->nodes_count, node) : -1;
    const std::string base_name = node && node->name ? node->name : mesh.name ? mesh.name : "mesh_" + std::to_string(mesh_index);
    std::vector<float> weights;
    if (node && node->weights_count) weights.assign(node->weights, node->weights + node->weights_count);
    for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
        ++out.source_features.active_source_primitive_count;
        const int mode = primitive_mode(mesh.primitives[primitive_index].type);
        if (mode >= 0 && mode < static_cast<int>(out.source_features.active_primitive_modes.size())) {
            ++out.source_features.active_primitive_modes[static_cast<std::size_t>(mode)];
        }
        add_primitive(data, mesh, mesh.primitives[primitive_index], options, world,
                      base_name + "_p" + std::to_string(primitive_index) +
                          (node && node->has_mesh_gpu_instancing ? "_i" + std::to_string(instance_index) : ""), weights, out,
                      node_index, mesh_index, static_cast<int>(primitive_index),
                      node_index >= 0 ? node_index : static_cast<int>(data->nodes_count) + mesh_index,
                      static_cast<int>(instance_index));
    }
}

void populate_active_primitives(const cgltf_data* data, Scene& out, const LoadOptions& options) {
    std::set<int> used_meshes;
    std::set<int> visited_nodes;
    std::function<void(int, const Mat4&, bool)> visit = [&](int node_index, const Mat4& parent_world, bool ancestors_visible) {
        if (node_index < 0 || static_cast<std::size_t>(node_index) >= data->nodes_count || visited_nodes.count(node_index)) return;
        visited_nodes.insert(node_index);
        const auto& node = data->nodes[static_cast<std::size_t>(node_index)];
        const bool visible = ancestors_visible && node_visibility(node);
        cgltf_float local_values[16]{};
        cgltf_node_transform_local(&node, local_values);
        bool transform_clamped = false;
        const Mat4 node_world = multiply(parent_world, from_cgltf_matrix(local_values), &transform_clamped);
        if (transform_clamped) {
            const std::string name = node.name ? node.name : "unnamed node";
            out.notes.push_back(name + ": finite transform composition exceeded float range; clamped matrix products [transform_composition_clamp]");
            out.source_features.approximation_locations.push_back(name + ": transform composition overflow clamped [transform_composition_clamp]");
        }
        if (visible && node.has_mesh_gpu_instancing && !node.mesh) {
            mesh_gpu_instance_worlds(node, node_world);
        }
        if (visible && node.mesh) {
            const auto instance_worlds = mesh_gpu_instance_worlds(node, node_world);
            for (std::size_t instance_index = 0; instance_index < instance_worlds.size(); ++instance_index) {
                add_mesh_instance(data, &node, *node.mesh, options, instance_worlds[instance_index], instance_index, out, used_meshes);
            }
        }
        for (cgltf_size i = 0; i < node.children_count; ++i)
            visit(pointer_index(data->nodes, data->nodes_count, node.children[i]), node_world, visible);
    };
    for (int root : out.scene_roots) visit(root, identity(), true);

    if (options.include_unused_meshes) {
        for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
            if (used_meshes.count(static_cast<int>(mesh_index))) continue;
            add_mesh_instance(data, nullptr, data->meshes[mesh_index], options, identity(), 0, out, used_meshes);
        }
    }
    out.source_features.decoded_primitive_count = out.primitives.size();
    out.source_features.fixed_initial_morph_bake_succeeded =
        out.source_features.active_morph_target_count != 0 &&
        out.source_features.active_morph_target_count == out.source_features.fixed_initial_morph_bake_count;
}

std::set<std::size_t> predecode_active_buffer_views(const cgltf_data* data,
                                                    const LoadOptions& options) {
    std::set<std::size_t> result;
    const auto add_view = [&](const cgltf_buffer_view* view) {
        const int index = pointer_index(data->buffer_views, data->buffer_views_count, view);
        if (index >= 0) result.insert(static_cast<std::size_t>(index));
    };
    const auto add_accessor = [&](const cgltf_accessor* accessor) {
        if (!accessor) return;
        add_view(accessor->buffer_view);
        if (accessor->is_sparse) {
            add_view(accessor->sparse.indices_buffer_view);
            add_view(accessor->sparse.values_buffer_view);
        }
    };

    std::set<int> active_nodes;
    std::vector<int> pending;
    if (options.scene_index) {
        if (*options.scene_index < 0 || static_cast<cgltf_size>(*options.scene_index) >= data->scenes_count)
            throw std::runtime_error("requested scene index is out of range");
        const auto& scene = data->scenes[static_cast<cgltf_size>(*options.scene_index)];
        for (cgltf_size i = 0; i < scene.nodes_count; ++i)
            pending.push_back(pointer_index(data->nodes, data->nodes_count, scene.nodes[i]));
    } else {
        const cgltf_scene* scene = data->scene ? data->scene :
            (data->scenes_count ? &data->scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
                pending.push_back(pointer_index(data->nodes, data->nodes_count, scene->nodes[i]));
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent) pending.push_back(static_cast<int>(i));
        }
    }
    while (!pending.empty()) {
        const int node_index = pending.back();
        pending.pop_back();
        if (node_index < 0 || static_cast<cgltf_size>(node_index) >= data->nodes_count ||
            !active_nodes.insert(node_index).second) continue;
        const auto& node = data->nodes[static_cast<cgltf_size>(node_index)];
        for (cgltf_size i = 0; i < node.children_count; ++i)
            pending.push_back(pointer_index(data->nodes, data->nodes_count, node.children[i]));
    }

    // Match animation activation: joints referenced by skins attached to the
    // selected scene participate even when they are outside the root subtree.
    const auto selected_nodes = active_nodes;
    for (const int node_index : selected_nodes) {
        const auto& node = data->nodes[static_cast<cgltf_size>(node_index)];
        if (!node.skin) continue;
        add_accessor(node.skin->inverse_bind_matrices);
        for (cgltf_size i = 0; i < node.skin->joints_count; ++i) {
            int joint = pointer_index(data->nodes, data->nodes_count, node.skin->joints[i]);
            while (joint >= 0 && static_cast<cgltf_size>(joint) < data->nodes_count) {
                if (!active_nodes.insert(joint).second) break;
                joint = pointer_index(data->nodes, data->nodes_count,
                                      data->nodes[static_cast<cgltf_size>(joint)].parent);
            }
        }
    }

    std::set<int> visible_nodes;
    std::function<void(int, bool)> mark_visible = [&](int node_index, bool ancestors_visible) {
        if (node_index < 0 || static_cast<cgltf_size>(node_index) >= data->nodes_count) return;
        const auto& node = data->nodes[static_cast<cgltf_size>(node_index)];
        const bool visible = ancestors_visible && node_visibility(node);
        if (visible) visible_nodes.insert(node_index);
        for (cgltf_size i = 0; i < node.children_count; ++i)
            mark_visible(pointer_index(data->nodes, data->nodes_count, node.children[i]), visible);
    };
    for (const int node_index : active_nodes) {
        const auto& node = data->nodes[static_cast<cgltf_size>(node_index)];
        const int parent = pointer_index(data->nodes, data->nodes_count, node.parent);
        if (parent < 0 || !active_nodes.count(parent)) mark_visible(node_index, true);
    }
    std::set<cgltf_size> active_meshes;
    for (const int node_index : selected_nodes) {
        const auto& node = data->nodes[static_cast<cgltf_size>(node_index)];
        if (!visible_nodes.count(node_index)) continue;
        const int mesh = pointer_index(data->meshes, data->meshes_count, node.mesh);
        if (mesh >= 0) active_meshes.insert(static_cast<cgltf_size>(mesh));
        if (node.has_mesh_gpu_instancing)
            for (cgltf_size i = 0; i < node.mesh_gpu_instancing.attributes_count; ++i)
                add_accessor(node.mesh_gpu_instancing.attributes[i].data);
    }
    if (options.include_unused_meshes)
        for (cgltf_size i = 0; i < data->meshes_count; ++i) active_meshes.insert(i);

    std::set<cgltf_size> active_materials;
    for (const auto mesh_index : active_meshes) {
        const auto& mesh = data->meshes[mesh_index];
        for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
            const auto& primitive = mesh.primitives[primitive_index];
            add_accessor(primitive.indices);
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
                add_accessor(primitive.attributes[i].data);
            for (cgltf_size target = 0; target < primitive.targets_count; ++target)
                for (cgltf_size i = 0; i < primitive.targets[target].attributes_count; ++i)
                    add_accessor(primitive.targets[target].attributes[i].data);
            if (primitive.has_draco_mesh_compression)
                add_view(primitive.draco_mesh_compression.buffer_view);
            const int material = pointer_index(data->materials, data->materials_count,
                                               effective_material(data, primitive, options));
            if (material >= 0) active_materials.insert(static_cast<cgltf_size>(material));
        }
    }

    const auto add_texture = [&](const cgltf_texture_view& binding) {
        if (!binding.texture) return;
        const auto& texture = *binding.texture;
        for (const cgltf_image* image : {
                 texture.basisu_image, texture.webp_image, texture.dds_image, texture.image})
            if (image) add_view(image->buffer_view);
    };
    for (const auto material_index : active_materials) {
        const auto& material = data->materials[material_index];
        add_texture(material.pbr_metallic_roughness.base_color_texture);
        add_texture(material.pbr_metallic_roughness.metallic_roughness_texture);
        add_texture(material.normal_texture);
        add_texture(material.occlusion_texture);
        add_texture(material.emissive_texture);
        add_texture(material.pbr_specular_glossiness.diffuse_texture);
        add_texture(material.pbr_specular_glossiness.specular_glossiness_texture);
        add_texture(material.clearcoat.clearcoat_texture);
        add_texture(material.clearcoat.clearcoat_roughness_texture);
        add_texture(material.clearcoat.clearcoat_normal_texture);
        add_texture(material.transmission.transmission_texture);
        add_texture(material.volume.thickness_texture);
        add_texture(material.sheen.sheen_color_texture);
        add_texture(material.sheen.sheen_roughness_texture);
        add_texture(material.specular.specular_texture);
        add_texture(material.specular.specular_color_texture);
        add_texture(material.iridescence.iridescence_texture);
        add_texture(material.iridescence.iridescence_thickness_texture);
        add_texture(material.anisotropy.anisotropy_texture);
        add_texture(material.diffuse_transmission.diffuse_transmission_texture);
        add_texture(material.diffuse_transmission.diffuse_transmission_color_texture);
    }

    for (cgltf_size animation_index = 0; animation_index < data->animations_count; ++animation_index) {
        if (options.animation_index && *options.animation_index != static_cast<int>(animation_index)) continue;
        const auto& animation = data->animations[animation_index];
        for (cgltf_size channel_index = 0; channel_index < animation.channels_count; ++channel_index) {
            const auto& channel = animation.channels[channel_index];
            if (!channel.sampler) continue;
            std::string_view pointer;
            std::string pointer_storage;
            AnimationPointerTarget pointer_target;
            const bool is_pointer = animation_pointer_target(channel, pointer, pointer_storage, pointer_target);
            const int target = channel.target_node ? pointer_index(data->nodes, data->nodes_count, channel.target_node) : -1;
            const bool active = is_pointer ?
                (pointer_target.node < 0 || active_nodes.count(pointer_target.node) != 0) : active_nodes.count(target) != 0;
            if (!active) continue;
            add_accessor(channel.sampler->input);
            add_accessor(channel.sampler->output);
        }
    }
    return result;
}

void decode_meshopt_buffer_views(CgltfDocument& document,
                                 const std::set<std::size_t>& active_buffer_views) {
    auto* data = document.data;
    const auto required = [&](const cgltf_meshopt_compression& compression) {
        const char* name = compression.is_khr ? "KHR_meshopt_compression" : "EXT_meshopt_compression";
        for (cgltf_size i = 0; i < data->extensions_required_count; ++i)
            if (data->extensions_required[i] && std::strcmp(data->extensions_required[i], name) == 0) return true;
        return false;
    };
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
        auto& view = data->buffer_views[i];
        if (!view.has_meshopt_compression) continue;
        ++document.meshopt_buffer_view_count;
        if (!active_buffer_views.count(static_cast<std::size_t>(i))) continue;

        const auto& compression = view.meshopt_compression;
        const auto fallback = [&](std::string_view reason) {
            if (required(compression))
                throw std::runtime_error(std::string(reason) + " (required " +
                                         (compression.is_khr ? "KHR_meshopt_compression" : "EXT_meshopt_compression") + ")");
            if (!view.buffer || !view.buffer->data || view.offset > view.buffer->size ||
                view.size > view.buffer->size - view.offset)
                throw std::runtime_error(std::string(reason) + "; optional meshopt compression has no valid core bufferView fallback");
            const std::string extension =
                compression.is_khr ? "KHR_meshopt_compression" : "EXT_meshopt_compression";
            document.meshopt_core_fallbacks.push_back({
                static_cast<std::size_t>(i),
                extension,
                "bufferView " + std::to_string(i) + ": optional " + extension +
                    " decode failed or was unsupported (" + std::string(reason) +
                    "); ordinary core bufferView used"
            });
            view.has_meshopt_compression = 0;
        };
        if (!compression.buffer || !compression.buffer->data) {
            fallback("meshopt compression source buffer is not loaded");
            continue;
        }
        if (compression.offset > compression.buffer->size ||
            compression.size > compression.buffer->size - compression.offset) {
            fallback("meshopt compression source range is out of bounds");
            continue;
        }
        if (compression.count != 0 && compression.stride > std::numeric_limits<std::size_t>::max() / compression.count) {
            fallback("meshopt compression decoded size overflows host size_t");
            continue;
        }
        const std::size_t decoded_size = static_cast<std::size_t>(compression.count * compression.stride);
        if (decoded_size != view.size) {
            fallback("meshopt compression decoded size disagrees with bufferView byteLength");
            continue;
        }

        if (!data->memory.alloc_func || !data->memory.free_func) {
            fallback("cgltf memory allocator is unavailable for meshopt compression output");
            continue;
        }
        view.data = data->memory.alloc_func(data->memory.user_data, decoded_size ? decoded_size : 1);
        if (!view.data) {
            if (required(compression)) throw std::bad_alloc();
            fallback("meshopt compression output allocation failed");
            continue;
        }
        auto* decoded = static_cast<unsigned char*>(view.data);
        const auto* source = static_cast<const unsigned char*>(compression.buffer->data) + compression.offset;
        int decode_result = -1;
        switch (compression.mode) {
            case cgltf_meshopt_compression_mode_attributes:
                decode_result = meshopt_decodeVertexBuffer(decoded, compression.count, compression.stride,
                                                           source, compression.size);
                break;
            case cgltf_meshopt_compression_mode_triangles:
                decode_result = meshopt_decodeIndexBuffer(decoded, compression.count, compression.stride,
                                                          source, compression.size);
                break;
            case cgltf_meshopt_compression_mode_indices:
                decode_result = meshopt_decodeIndexSequence(decoded, compression.count, compression.stride,
                                                            source, compression.size);
                break;
            default:
                if (data->memory.free_func) data->memory.free_func(data->memory.user_data, view.data);
                view.data = nullptr;
                fallback("meshopt compression uses an unsupported mode");
                continue;
        }
        if (decode_result != 0) {
            if (data->memory.free_func) data->memory.free_func(data->memory.user_data, view.data);
            view.data = nullptr;
            fallback("meshoptimizer failed to decode meshopt compression bufferView " + std::to_string(i));
            continue;
        }

        switch (compression.filter) {
            case cgltf_meshopt_compression_filter_none:
                break;
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(decoded, compression.count, compression.stride);
                break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(decoded, compression.count, compression.stride);
                break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(decoded, compression.count, compression.stride);
                break;
            case cgltf_meshopt_compression_filter_color:
                meshopt_decodeFilterColor(decoded, compression.count, compression.stride);
                break;
            default:
                if (data->memory.free_func) data->memory.free_func(data->memory.user_data, view.data);
                view.data = nullptr;
                fallback("meshopt compression uses an unsupported filter");
                continue;
        }
    }
}

CgltfDocument load_document(const std::filesystem::path& path, const LoadOptions& load_options) {
    CgltfDocument document;
    cgltf_options options{};
    std::error_code error;
    auto document_path = std::filesystem::canonical(path, error);
    if (error) throw std::runtime_error("cannot resolve glTF document path: " + path.string());
#ifdef _WIN32
    document_path = windows_long_path(document_path);
#endif
    BufferFileReadContext file_context{document_path, document_path.parent_path()};
    options.file.read = &guarded_file_read;
    options.file.user_data = &file_context;
    const std::string path_string = file_context.document_path.string();
    cgltf_result result = cgltf_parse_file(&options, path_string.c_str(), &document.data);
    if (result != cgltf_result_success) {
        throw std::runtime_error("cgltf parse failed: " + cgltf_result_name(result));
    }
    for (cgltf_size i = 0; i < document.data->buffers_count; ++i) {
        const char* uri = document.data->buffers[i].uri;
        if (!uri || std::strncmp(uri, "data:", 5) == 0) continue;
        const std::string decoded_uri = percent_decode_image_uri(uri);
        const std::filesystem::path uri_path(decoded_uri);
        if (decoded_uri.find('\0') != std::string::npos || decoded_uri.find(':') != std::string::npos ||
            uri_path.has_root_name() || uri_path.has_root_directory()) {
            throw std::runtime_error("buffer URI must be a relative file path: " + std::string(uri));
        }
    }
    result = cgltf_load_buffers(&options, document.data, path_string.c_str());
    if (result != cgltf_result_success) {
        throw std::runtime_error("cgltf buffer load failed: " + cgltf_result_name(result));
    }
    result = cgltf_validate(document.data);
    if (result != cgltf_result_success) {
        throw std::runtime_error("cgltf validation failed: " + cgltf_result_name(result));
    }
    // Resolve the requested variant before decoding buffer views, including
    // sources where no active primitive happens to reference that variant.
    selected_material_variant(document.data, load_options);
    decode_meshopt_buffer_views(document, predecode_active_buffer_views(document.data, load_options));
    return document;
}

} // namespace

bool load_scene(const std::filesystem::path& path, Scene& out, std::string& error, const LoadOptions& options) {
    try {
        auto document = load_document(path, options);
        const auto* data = document.data;
        out = {};

        populate_source_inventory(data, out);
        out.source_features.material_variant_selected = options.material_variant.has_value();
        out.source_features.meshopt_buffer_view_count = document.meshopt_buffer_view_count;
        out.extensions_used.reserve(data->extensions_used_count);
        for (cgltf_size i = 0; i < data->extensions_used_count; ++i) if (data->extensions_used[i]) out.extensions_used.emplace_back(data->extensions_used[i]);
        out.extensions_required.reserve(data->extensions_required_count);
        for (cgltf_size i = 0; i < data->extensions_required_count; ++i) if (data->extensions_required[i]) out.extensions_required.emplace_back(data->extensions_required[i]);

        out.materials.reserve(data->materials_count);
        for (cgltf_size i = 0; i < data->materials_count; ++i)
            out.materials.push_back(convert_material(data, data->materials[i], i, std::filesystem::absolute(path)));
        populate_samplers_textures(data, out);
        populate_cameras_lights(data, out);
        populate_nodes_scenes(data, out, options);
        populate_asset_definition(data, out);
        std::set<int> selected_visibility_nodes;
        std::vector<int> visibility_pending(out.scene_roots.begin(), out.scene_roots.end());
        while (!visibility_pending.empty()) {
            const int node_index = visibility_pending.back(); visibility_pending.pop_back();
            if (node_index < 0 || static_cast<std::size_t>(node_index) >= out.nodes.size() ||
                !selected_visibility_nodes.insert(node_index).second) continue;
            const auto& node = out.nodes[static_cast<std::size_t>(node_index)];
            visibility_pending.insert(visibility_pending.end(), node.children.begin(), node.children.end());
        }
        for (const int node_index : selected_visibility_nodes)
            if (!out.nodes[static_cast<std::size_t>(node_index)].visible) {
                const std::string detail = "KHR_node_visibility visible:false on node[" +
                    std::to_string(node_index) +
                    "] was lowered by suppressing its static geometry subtree; hierarchy metadata was retained [node_visibility_static_lowering]";
                out.notes.push_back(detail);
                out.source_features.approximation_locations.push_back(detail);
            }
        populate_skins(data, out);
        populate_animations(data, out);
        populate_active_primitives(data, out, options);
        if (!compiler::resolve_asset_definition(out, error)) return false;
        const auto active_images = active_image_indices(out);
        const auto optional_images = optional_active_image_indices(out);
        load_images(path, data, active_images, optional_images, out);
        if (!canonicalize_active_texture_sources(data, out, error)) return false;
        const auto active_buffer_views =
            populate_active_extensions(data, out, options, document.meshopt_core_fallbacks);
        for (const auto& fallback : document.meshopt_core_fallbacks) {
            if (!active_buffer_views.count(fallback.buffer_view_index)) continue;
            out.source_features.approximation_locations.push_back(fallback.message);
            out.notes.push_back(fallback.message);
        }

        if (out.source_features.meshopt_buffer_view_count) {
            out.notes.push_back(std::to_string(out.source_features.meshopt_buffer_view_count) +
                                " meshopt compression buffer view(s) were handled through the pinned meshoptimizer source or an explicit optional core fallback before accessor unpacking.");
        }
        if (out.source_features.draco_primitive_count) {
            out.notes.push_back(std::to_string(out.source_features.draco_primitive_count) +
                                " KHR_draco_mesh_compression primitive(s) were decoded through the pinned Draco source before SourceScene lowering.");
        }
        if (!out.skins.empty()) {
            out.notes.push_back("Skin joints and inverse bind matrices were decoded for BONES/SkinDT native emission.");
        }
        if (!out.animations.empty()) {
            out.notes.push_back("Animation samplers/channels were decoded into SourceScene tracks for native ANIMATION emission.");
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace dtglb::gltf
