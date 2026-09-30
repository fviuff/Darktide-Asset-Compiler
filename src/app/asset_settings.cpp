#include "app/asset_settings.h"

#include "cgltf.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <set>

namespace dtglb::app {
namespace {

std::size_t ws(std::string_view s, std::size_t p) { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; return p; }
std::size_t string_end(std::string_view s, std::size_t p) {
    if (p >= s.size() || s[p] != '"') return std::string_view::npos;
    for (++p; p < s.size(); ++p) { if (s[p] == '\\') ++p; else if (s[p] == '"') return p + 1; }
    return std::string_view::npos;
}
std::size_t value_end(std::string_view s, std::size_t p) {
    p = ws(s, p); if (p >= s.size()) return std::string_view::npos;
    if (s[p] == '"') return string_end(s, p);
    if (s[p] == '{' || s[p] == '[') {
        const char open = s[p], close = open == '{' ? '}' : ']'; int depth = 0; bool quoted = false;
        for (std::size_t i = p; i < s.size(); ++i) { const char c = s[i];
            if (quoted) { if (c == '\\') ++i; else if (c == '"') quoted = false; continue; }
            if (c == '"') quoted = true; else if (c == open) ++depth; else if (c == close && --depth == 0) return i + 1;
        }
        return std::string_view::npos;
    }
    std::size_t e = p; while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != ']' && !std::isspace(static_cast<unsigned char>(s[e]))) ++e; return e;
}
std::string_view trim(std::string_view value) { const auto begin = ws(value, 0); value.remove_prefix(begin); while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1); return value; }
std::string decode_json_string(std::string_view encoded) {
    std::string result;
    if (encoded.size() < 2 || encoded.front() != '"' || encoded.back() != '"') return {};
    for (std::size_t i = 1; i + 1 < encoded.size(); ++i) {
        if (encoded[i] != '\\') { result.push_back(encoded[i]); continue; }
        if (++i + 0 >= encoded.size() - 1) return {};
        const char escape = encoded[i];
        if (escape == '"' || escape == '\\' || escape == '/') result.push_back(escape);
        else if (escape == 'b') result.push_back('\b'); else if (escape == 'f') result.push_back('\f'); else if (escape == 'n') result.push_back('\n'); else if (escape == 'r') result.push_back('\r'); else if (escape == 't') result.push_back('\t');
        else if (escape == 'u' && i + 4 < encoded.size()) { unsigned value = 0; for (int j = 0; j < 4; ++j) { const char c = encoded[++i]; const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (digit < 0) return {}; value = value * 16u + static_cast<unsigned>(digit); } if (value > 0x7f) return {}; result.push_back(static_cast<char>(value)); }
        else return {};
    }
    return result;
}
struct JsonParser {
    std::string_view text; std::size_t pos = 0; int depth = 0;
    void space() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; }
    bool string() {
        if (pos >= text.size() || text[pos++] != '"') return false;
        while (pos < text.size()) { const unsigned char c = static_cast<unsigned char>(text[pos++]);
            if (c == '"') return true; if (c < 0x20) return false;
            if (c == '\\') { if (pos >= text.size()) return false; const char e = text[pos++];
                if (std::strchr("\"\\/bfnrt", e)) continue;
                if (e != 'u' || pos + 4 > text.size()) return false;
                for (int i = 0; i < 4; ++i) if (!std::isxdigit(static_cast<unsigned char>(text[pos++]))) return false;
            }
        } return false;
    }
    bool number() {
        const auto start = pos; if (pos < text.size() && text[pos] == '-') ++pos;
        if (pos >= text.size()) return false;
        if (text[pos] == '0') ++pos; else { if (text[pos] < '1' || text[pos] > '9') return false; while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos; }
        if (pos < text.size() && text[pos] == '.') { ++pos; const auto digit = pos; while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos; if (pos == digit) return false; }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) { ++pos; if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos; const auto digit = pos; while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos; if (pos == digit) return false; }
        return pos > start;
    }
    bool value() {
        space(); if (pos >= text.size() || ++depth > 128) return false;
        const char c = text[pos]; bool ok = c == '"' ? string() : c == '{' ? object() : c == '[' ? array() : c == 't' ? token("true") : c == 'f' ? token("false") : c == 'n' ? token("null") : number();
        --depth; return ok;
    }
    bool token(const char* expected) { const std::size_t n = std::strlen(expected); if (text.substr(pos, n) != expected) return false; pos += n; return true; }
    bool object() {
        ++pos; space(); if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
        while (true) { if (!string()) return false; space(); if (pos >= text.size() || text[pos++] != ':') return false; if (!value()) return false; space(); if (pos >= text.size()) return false; if (text[pos] == '}') { ++pos; return true; } if (text[pos++] != ',') return false; space(); }
    }
    bool array() {
        ++pos; space(); if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
        while (true) { if (!value()) return false; space(); if (pos >= text.size()) return false; if (text[pos] == ']') { ++pos; return true; } if (text[pos++] != ',') return false; space(); }
    }
};
bool valid_json(std::string_view text) { JsonParser parser{text}; if (!parser.value()) return false; parser.space(); return parser.pos == text.size(); }
struct Member { std::string_view key; std::string_view value; };
std::vector<Member> members(std::string_view object) {
    std::vector<Member> result; std::size_t p = ws(object, 0);
    if (p >= object.size() || object[p++] != '{') return result;
    while (true) {
        p = ws(object, p); if (p < object.size() && object[p] == '}') break;
        if (p < object.size() && object[p] == ',') p = ws(object, p + 1);
        if (p >= object.size() || object[p] != '"') break;
        const auto ke = string_end(object, p); if (ke == std::string_view::npos) break;
        const auto key = object.substr(p + 1, ke - p - 2); p = ws(object, ke);
        if (p >= object.size() || object[p++] != ':') break;
        const auto ve = value_end(object, p); if (ve == std::string_view::npos) break;
        result.push_back({key, object.substr(p, ve - p)}); p = ve;
    }
    return result;
}
std::string decode_key(std::string_view encoded) {
    std::string result;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '\\') { result.push_back(encoded[i]); continue; }
        if (++i >= encoded.size()) return {};
        if (encoded[i] != 'u' || i + 4 >= encoded.size()) return {};
        unsigned value = 0; for (int j = 0; j < 4; ++j) { const char c = encoded[++i]; value = value * 16u + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16); }
        if (value > 0x7f) return {}; result.push_back(static_cast<char>(value));
    }
    return result;
}
bool key_is(std::string_view encoded, std::string_view wanted) {
    return decode_key(encoded) == wanted;
}

bool strict_bool(std::string_view value, bool& result) {
    value = trim(value);
    if (value == "true") { result = true; return true; }
    if (value == "false") { result = false; return true; }
    return false;
}
bool strict_scene_index(std::string_view value, int& result) {
    value = trim(value);
    if (value.empty()) return false;
    unsigned long long parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned>(c - '0');
        if (parsed > (static_cast<unsigned long long>(std::numeric_limits<int>::max()) - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    result = static_cast<int>(parsed);
    return true;
}
std::string result_name(cgltf_result r) { return r == cgltf_result_invalid_json ? "invalid JSON" : "parse error"; }

} // namespace

bool load_asset_settings(const std::filesystem::path& path, AssetSettings& out, std::string& error) {
    out = {};
    if (path.empty()) return true;
    cgltf_data* data = nullptr;
    try {
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("settings file not found: " + path.string());
        cgltf_options options{};
        const auto result = cgltf_parse_file(&options, path.string().c_str(), &data);
        if (result != cgltf_result_success || !data || !data->json) throw std::runtime_error("settings parse failed: " + result_name(result));
        std::string_view json(data->json, data->json_size);
        if (!valid_json(json)) throw std::runtime_error("settings parse failed: invalid JSON syntax");
        if (ws(json, 0) >= json.size() || json[ws(json, 0)] != '{') throw std::runtime_error("settings root must be a JSON object");
        bool saw_physics = false, saw_scale = false, saw_approximations = false, saw_scene_index = false, saw_animation_index = false, saw_material_variant = false;
        for (const auto& member : members(json)) {
            if (key_is(member.key, "asset_path")) {
                if (out.asset_path) throw std::runtime_error("settings contains duplicate asset_path key");
                const auto value = trim(member.value);
                if (value.size() < 2 || value.front() != '"' || value.back() != '"')
                    throw std::runtime_error("settings asset_path must be a string");
                out.asset_path = decode_json_string(value);
            } else if (key_is(member.key, "material_variant")) {
                if (saw_material_variant) throw std::runtime_error("settings contains duplicate material_variant key");
                saw_material_variant = true;
                const auto value = trim(member.value);
                if (value.size() < 2 || value.front() != '"' || value.back() != '"')
                    throw std::runtime_error("settings material_variant must be a string");
                out.material_variant = decode_json_string(value);
                if (out.material_variant->empty()) throw std::runtime_error("settings material_variant must not be empty");
            } else if (key_is(member.key, "physics")) {
                if (saw_physics) throw std::runtime_error("settings contains duplicate physics key");
                saw_physics = true;
                out.has_physics = true;
                const auto physics_value = trim(member.value);
                if (physics_value == "false") {
                    out.physics_enabled = false;
                } else if (physics_value == "true") {
                    out.physics_enabled = true;
                } else {
                    if (physics_value.empty() || physics_value.front() != '{' || physics_value.back() != '}')
                        throw std::runtime_error("settings physics must be a boolean or object");
                    stingray::physics::FittedActorOptions fitted;
                    bool saw_enabled = false, saw_shape = false, shape_none = false, saw_actor = false,
                         saw_mass = false, saw_material = false;
                    for (const auto& nested : members(physics_value)) {
                        const auto key = decode_key(nested.key);
                        if (key == "enabled") {
                            if (saw_enabled) throw std::runtime_error("settings contains duplicate physics.enabled key");
                            saw_enabled = true;
                            if (!strict_bool(nested.value, out.physics_enabled))
                                throw std::runtime_error("settings physics.enabled must be a boolean");
                        } else if (key == "shape") {
                            if (saw_shape) throw std::runtime_error("settings contains duplicate physics.shape key");
                            saw_shape = true;
                            const auto value = trim(nested.value);
                            const auto shape = decode_json_string(value);
                            if (shape == "geometry" || shape == "mesh") fitted.shape = stingray::physics::PhysicsShapeType::TriangleMesh;
                            else if (shape == "convex") fitted.shape = stingray::physics::PhysicsShapeType::Convex;
                            else if (shape == "box") fitted.shape = stingray::physics::PhysicsShapeType::Box;
                            else if (shape == "sphere") fitted.shape = stingray::physics::PhysicsShapeType::Sphere;
                            else if (shape == "capsule") fitted.shape = stingray::physics::PhysicsShapeType::Capsule;
                            else if (shape == "none") shape_none = true;
                            else throw std::runtime_error("settings physics.shape must be geometry, convex, box, sphere, capsule, or none");
                        } else if (key == "actor") {
                            if (saw_actor) throw std::runtime_error("settings contains duplicate physics.actor key");
                            saw_actor = true;
                            fitted.actor_template = decode_json_string(trim(nested.value));
                            if (fitted.actor_template != "static" && fitted.actor_template != "dynamic" &&
                                fitted.actor_template != "keyframed")
                                throw std::runtime_error("settings physics.actor must be static, dynamic, or keyframed");
                        } else if (key == "mass") {
                            if (saw_mass) throw std::runtime_error("settings contains duplicate physics.mass key");
                            saw_mass = true;
                            const auto number = trim(nested.value); double parsed = 0.0;
                            const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
                            if (result.ec != std::errc{} || result.ptr != number.data() + number.size() ||
                                !std::isfinite(parsed) || parsed < 0.0 || parsed > std::numeric_limits<float>::max())
                                throw std::runtime_error("settings physics.mass must be a finite nonnegative number");
                            fitted.mass = static_cast<float>(parsed);
                        } else if (key == "material") {
                            if (saw_material) throw std::runtime_error("settings contains duplicate physics.material key");
                            saw_material = true;
                            fitted.material = decode_json_string(trim(nested.value));
                            if (fitted.material != "default" && fitted.material != "iron" && fitted.material != "rubber")
                                throw std::runtime_error("settings physics.material must be default, iron, or rubber");
                        } else {
                            throw std::runtime_error("settings physics contains unknown field: " + key);
                        }
                    }
                    if (shape_none) out.physics_enabled = false;
                    else if (saw_shape && out.physics_enabled) out.fitted_physics = fitted;
                }
            } else if (key_is(member.key, "scale")) {
                if (saw_scale) throw std::runtime_error("settings contains duplicate scale key");
                saw_scale = true;
                const auto number = trim(member.value); double parsed = 0.0;
                const auto result = std::from_chars(number.data(), number.data() + number.size(), parsed);
                if (result.ec != std::errc{} || result.ptr != number.data() + number.size() ||
                    !std::isfinite(parsed) || parsed <= 0.0 || parsed > std::numeric_limits<float>::max())
                    throw std::runtime_error("settings scale must be a finite positive number");
                out.asset_scale = static_cast<float>(parsed);
            } else if (key_is(member.key, "approximations")) {
                if (saw_approximations) throw std::runtime_error("settings contains duplicate approximations key");
                saw_approximations = true;
                const auto approximation_value = trim(member.value);
                if (approximation_value.empty() || approximation_value.front() != '{' || approximation_value.back() != '}')
                    throw std::runtime_error("settings approximations must be a JSON object");
                std::set<std::string> decoded_keys;
                for (const auto& nested : members(approximation_value)) {
                    const auto key = decode_key(nested.key);
                    if (key.empty() || !decoded_keys.insert(key).second)
                        throw std::runtime_error("settings contains duplicate approximations key: " + key);
                    bool enabled = false;
                    if (key == "fixed_morph_bake") {
                        if (!strict_bool(nested.value, enabled)) throw std::runtime_error("settings approximations.fixed_morph_bake must be a boolean");
                        out.allow_fixed_morph_bake = enabled;
                    } else if (key == "animation_resampling") {
                        if (!strict_bool(nested.value, enabled)) throw std::runtime_error("settings approximations.animation_resampling must be a boolean");
                        out.allow_animation_resampling = enabled;
                    } else if (key == "native_animation_interpolation") {
                        if (!strict_bool(nested.value, enabled)) throw std::runtime_error("settings approximations.native_animation_interpolation must be a boolean");
                        out.allow_native_animation_interpolation = enabled;
                    }
                    else throw std::runtime_error("settings approximations contains unknown field: " + key);
                }
            } else if (key_is(member.key, "animation_fit")) {
                if(out.animation_fit) throw std::runtime_error("settings contains duplicate animation_fit key");
                const auto value=trim(member.value);
                if(value.empty() || value.front()!='{' || value.back()!='}')
                    throw std::runtime_error("settings animation_fit must be an object");
                stingray::animation::FitOptions fit;
                std::set<std::string> seen;
                for(const auto& nested:members(value)) {
                    const auto key=decode_key(nested.key);
                    if(!seen.insert(key).second) throw std::runtime_error("settings contains duplicate animation_fit field: "+key);
                    double* tolerance=key=="translation_tolerance"?&fit.translation_tolerance:
                        key=="scale_tolerance"?&fit.scale_tolerance:key=="rotation_tolerance_radians"?&fit.rotation_tolerance_radians:nullptr;
                    std::size_t* budget=key=="max_controls"?&fit.max_controls:
                        key=="max_refinements"?&fit.max_refinements:key=="max_evaluations"?&fit.max_evaluations:nullptr;
                    if(tolerance) {
                        const auto number=trim(nested.value);double parsed=0;
                        const auto result=std::from_chars(number.data(),number.data()+number.size(),parsed);
                        if(result.ec!=std::errc{} || result.ptr!=number.data()+number.size() || !std::isfinite(parsed) || parsed<=0)
                            throw std::runtime_error("settings animation_fit."+key+" must be a finite positive number");
                        *tolerance=parsed;
                    } else if(budget) {
                        int parsed=0;
                        if(!strict_scene_index(nested.value,parsed) || parsed<=0)
                            throw std::runtime_error("settings animation_fit."+key+" must be a positive integer <= INT_MAX");
                        *budget=static_cast<std::size_t>(parsed);
                    } else throw std::runtime_error("settings animation_fit contains unknown field: "+key);
                }
                out.animation_fit=fit;
            } else if (key_is(member.key, "scene_index")) {
                if (saw_scene_index) throw std::runtime_error("settings contains duplicate scene_index key");
                saw_scene_index = true;
                int index = 0;
                if (!strict_scene_index(member.value, index)) throw std::runtime_error("settings scene_index must be a nonnegative integer <= INT_MAX");
                out.scene_index = index;
            } else if (key_is(member.key, "animation_index")) {
                if (saw_animation_index) throw std::runtime_error("settings contains duplicate animation_index key");
                saw_animation_index = true;
                int index = 0;
                if (!strict_scene_index(member.value, index)) throw std::runtime_error("settings animation_index must be a nonnegative integer <= INT_MAX");
                out.animation_index = index;
            } else {
                out.unknown_keys.emplace_back(member.key);
            }
        }
        cgltf_free(data); data = nullptr; return true;
    } catch (const std::exception& e) { if (data) cgltf_free(data); error = e.what(); return false; }
}

ResolvedPhysicsInventory resolve_physics_inventory(const SourceFeatureInventory& source,
                                                   const AssetSettings& settings, bool physics_enabled) {
    ResolvedPhysicsInventory result{source.physics_requested_count, source.physics_effective_count,
                                    source.physics_suppressed_count, source.physics_locations};
    if (settings.has_physics) { ++result.requested; result.locations.push_back("settings.physics"); if (settings.physics_enabled) ++result.effective; }
    result.suppressed = result.requested >= result.effective ? result.requested - result.effective : 0;
    if (!physics_enabled) result.suppressed += result.effective;
    return result;
}

} // namespace dtglb::app
