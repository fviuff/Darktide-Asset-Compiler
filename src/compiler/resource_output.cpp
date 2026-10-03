#include "stingray/resource_name.h"
#include "compiler/resource_output.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "validation/unit_validator.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <string_view>
#include <utility>

namespace dtglb::compiler {
namespace {
std::filesystem::path artifact_path(const std::filesystem::path& directory, const std::string& relative) {
    auto path = directory / relative;
#ifdef _WIN32
    path.make_preferred();
#endif
    return path;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}
    bool artifacts(std::vector<std::string>& paths) {
        skip();
        if (!object(paths)) return false;
        skip();
        return pos_ == text_.size() && schema_ && schema_value_ == 1;
    }
private:
    std::string_view text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    bool schema_ = false;
    int schema_value_ = 0;
    void skip() { while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
    bool string(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_++] != '"') return false;
        out.clear();
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            if (e == '"' || e == '\\' || e == '/') out.push_back(e);
            else if (e == 'b') out.push_back('\b'); else if (e == 'f') out.push_back('\f');
            else if (e == 'n') out.push_back('\n'); else if (e == 'r') out.push_back('\r'); else if (e == 't') out.push_back('\t');
            else if (e == 'u') {
                if (pos_ + 4 > text_.size()) return false;
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[pos_++];
                    const int digit = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (digit < 0) return false;
                    value = value * 16u + static_cast<unsigned>(digit);
                }
                out.push_back(value <= 0x7f ? static_cast<char>(value) : '?');
            } else return false;
        }
        return false;
    }
    bool value() {
        skip();
        if (pos_ >= text_.size() || ++depth_ > 128) return false;
        const char c = text_[pos_]; bool ok = false;
        if (c == '"') { std::string ignored; ok = string(ignored); }
        else if (c == '{') ok = object_ignored();
        else if (c == '[') ok = array_ignored();
        else ok = primitive();
        --depth_; return ok;
    }
    bool primitive() {
        const auto start = pos_;
        for (const char* token : {"true", "false", "null"}) {
            const std::size_t length = std::char_traits<char>::length(token);
            if (text_.substr(pos_, length) == token) { pos_ += length; return true; }
        }
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return false;
        if (text_[pos_] == '0') ++pos_;
        else { if (text_[pos_] < '1' || text_[pos_] > '9') return false; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; }
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; const auto digit = pos_; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; if (pos_ == digit) return false; }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) { ++pos_; if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_; const auto digit = pos_; while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_; if (pos_ == digit) return false; }
        return pos_ > start;
    }
    bool object_ignored() {
        ++pos_; skip(); if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        while (true) { std::string key; if (!string(key)) return false; skip(); if (pos_ >= text_.size() || text_[pos_++] != ':') return false; if (!value()) return false; skip(); if (pos_ >= text_.size()) return false; if (text_[pos_] == '}') { ++pos_; return true; } if (text_[pos_++] != ',') return false; skip(); }
    }
    bool array_ignored() {
        ++pos_; skip(); if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        while (true) { if (!value()) return false; skip(); if (pos_ >= text_.size()) return false; if (text_[pos_] == ']') { ++pos_; return true; } if (text_[pos_++] != ',') return false; skip(); }
    }
    bool artifact_array(std::vector<std::string>& paths) {
        ++pos_; skip(); if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            skip(); if (pos_ >= text_.size() || text_[pos_] != '{') return false; ++pos_; skip();
            bool has_path = false; std::string path;
            if (pos_ < text_.size() && text_[pos_] == '}') return false;
            while (true) {
                std::string key; if (!string(key)) return false; skip(); if (pos_ >= text_.size() || text_[pos_++] != ':') return false; skip();
                if (key == "path") { if (has_path || !string(path)) return false; has_path = true; } else if (!value()) return false;
                skip(); if (pos_ >= text_.size()) return false; if (text_[pos_] == '}') { ++pos_; break; } if (text_[pos_++] != ',') return false; skip();
            }
            if (!has_path) return false; paths.push_back(std::move(path)); skip();
            if (pos_ >= text_.size()) return false; if (text_[pos_] == ']') { ++pos_; return true; } if (text_[pos_++] != ',') return false;
        }
    }
    bool object(std::vector<std::string>& paths) {
        if (pos_ >= text_.size() || text_[pos_++] != '{') return false; skip(); bool found = false;
        if (pos_ < text_.size() && text_[pos_] == '}') return false;
        while (true) {
            std::string key; if (!string(key)) return false; skip(); if (pos_ >= text_.size() || text_[pos_++] != ':') return false; skip();
            if (key == "schema") {
                if (schema_) return false;
                const auto start = pos_; if (!primitive() || text_.substr(start, pos_ - start) != "1") return false;
                schema_ = true; schema_value_ = 1;
            } else if (key == "artifacts") { if (found || pos_ >= text_.size() || text_[pos_] != '[' || !artifact_array(paths)) return false; found = true; } else if (!value()) return false;
            skip(); if (pos_ >= text_.size()) return false; if (text_[pos_] == '}') { ++pos_; return found; } if (text_[pos_++] != ',') return false; skip();
        }
    }
};

bool save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "cannot create payload directory: " + ec.message(); return false; }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) { error = "cannot write resource payload: " + path.string(); return false; }
    return true;
}

bool artifact_size(const std::filesystem::path& path, std::uint64_t& size, std::string& error) {
    std::error_code ec;
    const auto value = std::filesystem::file_size(path, ec);
    if (ec) { error = "cannot inspect resource artifact: " + path.string() + ": " + ec.message(); return false; }
    size = static_cast<std::uint64_t>(value);
    return true;
}
std::string quote(std::string_view value) {
    std::ostringstream out; out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"'; return out.str();
}
std::string key_json(const ResourceKey& key) {
    return "{\"type\":" + quote(key.type) + ",\"name\":" + quote(key.name) + "}";
}
const char* artifact_role_name(ArtifactRole role) {
    switch (role) {
        case ArtifactRole::Primary: return "primary";
        case ArtifactRole::Stream: return "stream";
        case ArtifactRole::Binary: return "binary";
        case ArtifactRole::Bulk: return "bulk";
        case ArtifactRole::Metadata: return "metadata";
        case ArtifactRole::Auxiliary: return "auxiliary";
    }
    return "unknown";
}
const ResourceArtifact* find_artifact(const SerializedResources& resources,
                                      const ResourceKey& resource, ArtifactRole role) {
    const auto found = std::find_if(resources.artifacts.begin(), resources.artifacts.end(),
        [&](const ResourceArtifact& artifact) {
            return artifact.logical_resource == resource && artifact.role == role;
        });
    return found == resources.artifacts.end() ? nullptr : &*found;
}
std::string hex(std::uint64_t value, int width) {
    std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(width) << value; return out.str();
}
bool file_json(const std::filesystem::path& directory, const std::string& path,
               std::string& json, std::string& error) {
    std::ifstream input(artifact_path(directory, path), std::ios::binary);
    if (!input) { error = "missing manifest payload: " + path; return false; }
    std::array<char, 65536> buffer{};
    std::uint64_t size = 0; std::uint32_t crc = 0xffffffffu;
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
            values[i] = c;
        }
        return values;
    }();
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        size += static_cast<std::uint64_t>(input.gcount());
        for (std::streamsize i = 0; i < input.gcount(); ++i)
            crc = table[(crc ^ static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)])) & 255] ^ (crc >> 8);
    }
    if (!input.eof()) { error = "cannot checksum payload: " + path; return false; }
    json = "{\"path\":" + quote(path) + ",\"size\":" + std::to_string(size) +
           ",\"crc32\":" + quote(hex(crc ^ 0xffffffffu, 8)) + "}";
    return true;
}
}

bool read_build_artifact_paths(const std::filesystem::path& directory,
                               std::vector<std::string>& paths, std::string& error) {
    std::ifstream input(directory / "build.json", std::ios::binary);
    if (!input) { error = "cannot open prior build manifest"; return false; }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) { error = "cannot read prior build manifest"; return false; }
    JsonReader reader(text);
    paths.clear();
    if (!reader.artifacts(paths)) { paths.clear(); error = "invalid prior build manifest"; return false; }
    std::set<std::string> unique_paths;
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const std::string& path) {
        return !unique_paths.insert(path).second;
    }), paths.end());
    return true;
}

bool serialize_graph(const ResourceGraph& graph, const std::filesystem::path& directory,
                     SerializedResources& out, std::string& error, const TargetProfile& target_profile) {
    out = {};
    if (!validate_graph(graph, out.closure, error, target_profile)) return false;
    std::set<std::string> artifact_paths;
    for (const auto index : out.closure.owned) {
        const auto& node = graph.owned[index];
        const auto stream = resource_stream_name(node, target_profile);
        if (!artifact_paths.insert(node.file).second || (!stream.empty() && !artifact_paths.insert(stream).second)) {
            error = "resource artifact path collision";
            return false;
        }
    }
    for (const auto index : out.closure.owned) {
        const auto& node = graph.owned[index];
        const auto path = artifact_path(directory, node.file);
        const auto stream = resource_stream_name(node, target_profile);
        const auto stream_path = artifact_path(directory, stream);
        bool ok = false;
        if (const auto* unit = std::get_if<stingray::unit::UnitResource>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("unit", node.key.name, unit->body), error);
            std::string report;
            std::string state_machine_resource;
            for (const auto& dependency : node.dependencies)
                if (dependency.type == "state_machine") state_machine_resource = dependency.name;
            if (ok && !validation::validate_unit_v115(path, report, state_machine_resource)) { error = report; ok = false; }
        } else if (const auto* state_machine = std::get_if<std::vector<std::uint8_t>>(&node.resource)) {
            ok = save(path, *state_machine, error);
            if (ok) {
                std::vector<std::string> animations;
                for (const auto& dependency : node.dependencies)
                    if (dependency.type == "animation") animations.push_back(dependency.name);
                std::string validation_error;
                bool valid = animations.size() == 1 &&
                    stingray::validate_minimal_single_clip_state_machine(
                        *state_machine, node.key.name, animations.front(), validation_error);
                if (!valid) valid = stingray::validate_direct_event_state_machine_dependencies(
                    *state_machine, node.key.name, animations, validation_error);
                if (!valid) {
                    error = validation_error;
                    ok = false;
                }
            }
        } else if (const auto* bones = std::get_if<stingray::bones::BonesResource>(&node.resource)) {
            ok = stingray::bones::write_cooked_bones(*bones, path, error);
        } else if (const auto* animation = std::get_if<stingray::animation::BuiltAnimation>(&node.resource)) {
            std::vector<std::uint8_t> body; std::string stream_name;
            ok = stingray::parse_cooked_resource_envelope(animation->cooked, "animation", body, stream_name, error);
            std::uint64_t name_hash = 0; std::uint32_t bone_count = 0;
            if (ok && body.size() >= 12) {
                std::memcpy(&name_hash, animation->cooked.data() + 8, 8);
                std::memcpy(&bone_count, body.data() + 8, 4);
                ok = name_hash == stingray::resource_name_hash(node.key.name) && stingray::animation::validate_skeletal_animation(animation->cooked, bone_count, error);
            } else ok = false;
            if (!ok && error.empty()) error = "ANIMATION body or identity mismatch";
            if (ok) ok = stingray::animation::write_skeletal_animation(*animation, path, error);
        } else if (const auto* material = std::get_if<stingray::material::MaterialStream>(&node.resource)) {
            ok = stingray::material::write_material_pair(node.key.name, *material, path, stream_path, stream, error);
        } else if (const auto* preserved = std::get_if<stingray::material::PreservedMaterialStream>(&node.resource)) {
            ok = stingray::material::write_preserved_material_pair(node.key.name, *preserved, path, stream_path, stream, error);
        } else if (const auto* texture = std::get_if<PreservedTexture>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("texture", node.key.name, texture->body, stream), error) &&
                 (stream.empty() || save(stream_path, texture->stream, error));
        } else if (const auto* unit = std::get_if<PreservedUnit>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("unit", node.key.name, unit->body), error);
        } else if (const auto* particles = std::get_if<ParticlesResource>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("particles", node.key.name, particles->body), error);
        } else if (const auto* texture = std::get_if<TextureResource>(&node.resource)) {
            stingray::texture::CookedTexture cooked;
            ok = stingray::texture::write_native_texture_rgba(texture->image, texture->profile, node.key.name,
                path, stream_path, cooked, error);
            if (ok && cooked.dimensions_normalized) {
                out.texture_normalizations.push_back({node.key, cooked.profile, cooked.source_width,
                    cooked.source_height, cooked.width, cooked.height});
            }
        }
        if (!ok) return false;
        out.files.push_back(node.file);
        std::uint64_t primary_size = 0;
        if (!artifact_size(path, primary_size, error)) return false;
        out.artifacts.push_back({node.key, ArtifactRole::Primary, node.file, primary_size});
        if (!stream.empty()) {
            out.files.push_back(stream);
            std::uint64_t stream_size = 0;
            if (!artifact_size(stream_path, stream_size, error)) return false;
            out.artifacts.push_back({node.key, ArtifactRole::Stream, stream, stream_size});
        }
    }
    return true;
}

bool write_build_manifest(const ResourceGraph& graph, const SerializedResources& resources,
                          const std::filesystem::path& directory,
                          std::string& error, const BuildMetadata& metadata) {
    std::ostringstream json;
    json << "{\n  \"schema\":1,\n  \"requested_output\":" << quote(metadata.requested_output)
         << ",\n  \"selected_scene\":" << (metadata.selected_scene ? std::to_string(*metadata.selected_scene) : "null")
         << ",\n  \"requested_clip\":" << (metadata.requested_clip ? std::to_string(*metadata.requested_clip) : "null")
         << ",\n  \"emitted_clips\":[";
    for (std::size_t i = 0; i < metadata.emitted_clips.size(); ++i) {
        if (i) json << ',';
        json << metadata.emitted_clips[i];
    }
    json << ']'
         << ",\n  \"applied_approximations\":[";
    for (std::size_t i = 0; i < metadata.applied_approximations.size(); ++i) {
        if (i) json << ',';
        json << quote(metadata.applied_approximations[i]);
    }
    json << "],\n  \"texture_dimension_normalizations\":[";
    for (std::size_t i = 0; i < resources.texture_normalizations.size(); ++i) {
        if (i) json << ',';
        const auto& normalization = resources.texture_normalizations[i];
        json << "{\"resource\":" << key_json(normalization.resource)
             << ",\"profile\":" << quote(normalization.profile)
             << ",\"source_width\":" << normalization.source_width
             << ",\"source_height\":" << normalization.source_height
             << ",\"stored_width\":" << normalization.stored_width
             << ",\"stored_height\":" << normalization.stored_height << '}';
    }
    json << "],\n  \"compiler\":{\"name\":\"DarktideGLBCompiler\",\"version\":\"0.3.0-beta\"},"
         << "\n  \"target\":{\"id\":" << quote(metadata.target_profile.id)
         << ",\"name\":" << quote(metadata.target_profile.name)
         << ",\"unit_version\":" << metadata.target_profile.unit_version
         << ",\"material_version\":" << metadata.target_profile.material_version
         << "},\n  \"roots\":[";
    bool first = true;
    for (const auto& root : graph.roots) { if (!first) json << ','; first = false; json << key_json(root); }
    json << "],\n  \"artifacts\":[";
    for (std::size_t i = 0; i < resources.artifacts.size(); ++i) {
        if (i) json << ',';
        const auto& artifact = resources.artifacts[i];
        json << "{\"resource\":" << key_json(artifact.logical_resource)
             << ",\"role\":" << quote(artifact_role_name(artifact.role))
             << ",\"path\":" << quote(artifact.path)
             << ",\"size\":" << artifact.size << '}';
    }
    json << "],\n  \"resources\":[";
    first = true;
    for (const auto index : resources.closure.owned) {
        const auto& node = graph.owned[index];
        const auto* primary = find_artifact(resources, node.key, ArtifactRole::Primary);
        if (!primary) { error = "resource has no primary artifact: " + node.key.type + ":" + node.key.name; return false; }
        std::string file;
        if (!file_json(directory, primary->path, file, error)) return false;
        if (!first) json << ','; first = false;
        json << "\n    {\"key\":" << key_json(node.key) << ",\"name_hash\":" << quote(hex(stingray::resource_name_hash(node.key.name), 16))
             << ",\"file\":" << file << ",\"stream\":";
        const auto* stream = find_artifact(resources, node.key, ArtifactRole::Stream);
        if (!stream) json << "null";
        else { if (!file_json(directory, stream->path, file, error)) return false; json << "{\"name\":" << quote(stream->path) << ",\"file\":" << file << '}'; }
        json << ",\"dependencies\":[";
        for (std::size_t i = 0; i < node.dependencies.size(); ++i) { if (i) json << ','; json << key_json(node.dependencies[i]); }
        json << "]";
        if (const auto* animation = std::get_if<stingray::animation::BuiltAnimation>(&node.resource); animation && animation->fidelity) {
            const auto& report = *animation->fidelity;
            json << ",\"animation_fidelity\":{\"method\":\"sampled_joint_local\",\"is_error_bound\":false,\"status\":"
                 << quote(report.available ? "sampled" : "unavailable");
            if (report.available) {
                json << ",\"sample_times\":" << report.samples
                     << ",\"max_observed_translation_error\":" << report.translation
                     << ",\"max_observed_scale_error\":" << report.scale
                     << ",\"max_observed_rotation_error_radians\":" << report.rotation_radians;
            } else json << ",\"reason\":" << quote(report.error);
            json << '}';
        }
        if(const auto* animation=std::get_if<stingray::animation::BuiltAnimation>(&node.resource);animation && animation->fit_options) {
            const auto& options=*animation->fit_options;
            const bool best_effort = std::any_of(animation->fits.begin(), animation->fits.end(),
                [](const auto& fit) { return fit.best_effort; });
            const bool missing_measurement = std::any_of(animation->fits.begin(), animation->fits.end(),
                [](const auto& fit) { return !fit.measured_error_available; });
            json << std::setprecision(17) << ",\"animation_fit\":{\"method\":\"adaptive_native_hermite\",\"is_error_bound\":false,\"status\":"
                 << quote(missing_measurement ? "unmeasured_best_effort" :
                          (best_effort ? "measured_best_effort" : "measured_conformant"))
                 << ",\"translation_tolerance\":" << options.translation_tolerance
                 << ",\"scale_tolerance\":" << options.scale_tolerance
                 << ",\"rotation_tolerance_radians\":" << options.rotation_tolerance_radians
                 << ",\"max_controls_per_track\":" << options.max_controls
                 << ",\"max_refinements_per_track\":" << options.max_refinements
                 << ",\"max_evaluations_per_track\":" << options.max_evaluations << ",\"tracks\":[";
            for(std::size_t i=0;i<animation->fits.size();++i) {
                if(i)json<<',';const auto& fit=animation->fits[i];
                const char* path=fit.path==AnimationPath::Translation?"translation":fit.path==AnimationPath::Scale?"scale":"rotation";
                json << "{\"source_node\":" << fit.source_node << ",\"path\":" << quote(path)
                     << ",\"status\":" << quote(!fit.measured_error_available ? "best_effort_unmeasured" :
                                                   (fit.best_effort ? "best_effort" : "conformant"))
                     << ",\"conformant\":" << (fit.conformant ? "true" : "false")
                     << ",\"best_effort\":" << (fit.best_effort ? "true" : "false")
                     << ",\"tolerance\":" << fit.tolerance
                     << ",\"measurement_available\":" << (fit.measured_error_available ? "true" : "false");
                if (fit.measured_error_available)
                    json << ",\"measured_max_error\":" << fit.measured_max_error
                         << ",\"measured_max_error_time\":" << fit.measured_max_error_time;
                json
                     << ",\"controls\":" << fit.controls << ",\"refinements\":" << fit.refinements
                     << ",\"evaluations\":" << fit.evaluations
                     << ",\"planned_probes\":" << fit.planned_probe_count
                     << ",\"evaluated_probes\":" << fit.evaluated_probe_count << '}';
            }
            json << "]}";
        }
        json << '}';
    }
    json << "\n  ],\n  \"external_resources\":[";
    first = true;
    for (const auto& ref : resources.closure.external) {
        if (!first) json << ','; first = false;
        json << "{\"key\":" << key_json(ref.key) << ",\"package_member\":" << (ref.package_member ? "true" : "false") << '}';
    }
    json << "]\n}\n";
    const auto text = json.str();
    return save(directory / "build.json", {text.begin(), text.end()}, error);
}
}
