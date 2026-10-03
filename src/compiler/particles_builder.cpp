#include "compiler/particles_builder.h"

#include "compiler/material_builder.h"
#include "stingray/cooked_resource.h"
#include "stingray/material/material_v61.h"
#include "stingray/murmur_hash.h"
#include "stingray/particles/particles_resource.h"
#include "stingray/resource_name.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace dtglb::compiler {
namespace {
namespace fs = std::filesystem;

std::string hex16(std::uint64_t value) {
    std::ostringstream text;
    text << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    std::ifstream input(path, std::ios::binary);
    out.resize(ec ? 0 : static_cast<std::size_t>(size));
    if (ec || !input || !input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}

// The game's resources as a limn extract holds them (<extract>/<16 hex>.<type>). Materials and most textures
// there are stubs whose body names a loose file under <game>/bundle.
struct GameFiles {
    fs::path extract, bundle;
    std::set<std::uint64_t> materials, units, other; // other: resource types a copied unit cannot bring along

    bool load(const std::string& folder, std::string& error) {
        extract = fs::path(folder);
        bundle = stingray::texture::game_root() / "bundle";
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(extract, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() < 18 || name[16] != '.') continue;
            const auto type = name.substr(17);
            auto& target = type == "material" ? materials : type == "unit" ? units : other;
            if (&target == &other && type != "bones" && type != "state_machine" && type != "particles" &&
                type != "texture" && type != "animation") continue;
            try {
                target.insert(std::stoull(name.substr(0, 16), nullptr, 16));
            } catch (const std::exception&) {}
        }
        if (ec) { error = "cannot list the extract folder " + folder; return false; }
        if (materials.empty()) {
            error = "the extract folder has no material files; extract with limn's 'particles material texture' filter";
            return false;
        }
        return true;
    }

    // body of a cooked resource; a stub's body is replaced by the loose file it names
    bool resource(std::uint64_t hash, const char* type, std::vector<std::uint8_t>& body, std::string& stream,
                  std::string& error) const {
        std::vector<std::uint8_t> blob;
        if (!read_file(extract / (hex16(hash) + "." + type), blob, error)) return false;
        if (blob.size() < 38) { error = std::string("the game's ") + type + " " + hex16(hash) + " is truncated"; return false; }
        const auto body_size = u32(blob, 29), stream_size = u32(blob, 34);
        if (38ull + body_size + stream_size != blob.size()) {
            error = std::string("the game's ") + type + " " + hex16(hash) + " has an unknown layout";
            return false;
        }
        body.assign(blob.begin() + 38, blob.begin() + 38 + body_size);
        stream.assign(reinterpret_cast<const char*>(blob.data()) + 38 + body_size, stream_size);
        if (body.size() >= 5 && std::memcmp(body.data(), "data/", 5) == 0) {
            const std::string loose(reinterpret_cast<const char*>(body.data()),
                                    strnlen(reinterpret_cast<const char*>(body.data()), body.size()));
            stream.clear();
            if (!fs::is_regular_file(bundle / loose)) {
                error = std::string("the game's ") + type + " " + hex16(hash) + " names " + loose +
                    ", which this game version does not have; extract the game files again";
                return false;
            }
            return read_file(bundle / loose, body, error);
        }
        return true;
    }

    // path of a material's loose stream
    bool material_stream(std::uint64_t hash, fs::path& path, std::string& error) const {
        std::vector<std::uint8_t> blob;
        if (!read_file(extract / (hex16(hash) + ".material"), blob, error)) return false;
        if (blob.size() < 43 || std::memcmp(blob.data() + 38, "data/", 5) != 0) {
            error = "the game's material " + hex16(hash) + " is not a stub naming its stream";
            return false;
        }
        const std::string loose(reinterpret_cast<const char*>(blob.data()) + 38,
                                strnlen(reinterpret_cast<const char*>(blob.data()) + 38, blob.size() - 38));
        path = bundle / loose;
        if (!fs::is_regular_file(path)) {
            error = "the game's material " + hex16(hash) + " names " + loose +
                ", which this game version does not have; extract the game files again";
            return false;
        }
        return true;
    }
};

bool own_texture(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game, std::uint64_t hash,
                 ResourceKey& out, std::string& error) {
    const auto stem = context.file_base() + "_fx_tex_" + hex16(hash);
    out = context.generated_key("texture", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    std::vector<std::uint8_t> body, kind1, stream_bytes;
    std::string stream;
    if (!game.resource(hash, "texture", body, stream, error)) return false;
    if (!stream.empty() && !read_file(game.bundle / stream, stream_bytes, error)) return false;
    if (!stingray::texture::texture_body_as_kind1(body, kind1, error)) {
        error = "the game's texture " + hex16(hash) + ": " + error;
        return false;
    }
    graph.owned.push_back({out, stem + ".texture", PreservedTexture{std::move(kind1), std::move(stream_bytes)}, {}});
    return true;
}

// a shader provider/parent and the parents above it, as owned copies
bool own_shader_chain(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game,
                      std::uint64_t hash, ResourceKey& out, std::string& error) {
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, error) || !read_file(path, bytes, error)) return false;
    if (bytes.size() < 28 || u32(bytes, 4) + 20 > bytes.size()) { error = "the game's shader material " + hex16(hash) + " is truncated"; return false; }
    std::uint64_t parent = 0;
    std::memcpy(&parent, bytes.data() + u32(bytes, 4) + 12, 8);
    ResourceKey owned_parent;
    if (parent && !own_shader_chain(context, graph, game, parent, owned_parent, error)) return false;
    return own_game_shader_material(context, graph, hash, path, parent, parent ? &owned_parent : nullptr, out, error,
        [&](std::uint64_t texture, ResourceKey& key, std::string& texture_error) {
            return own_texture(context, graph, game, texture, key, texture_error);
        });
}

bool own_particle_material(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game,
                           std::uint64_t hash, const ParticleMaterialEdit* edit, ResourceKey& out, std::string& error) {
    const auto stem = context.file_base() + "_fx_" + hex16(hash);
    out = context.generated_key("material", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, error) || !read_file(path, bytes, error)) return false;
    stingray::material::MaterialStream parsed;
    if (!stingray::material::parse_material_stream(bytes, parsed, error)) {
        error = "the game's material " + hex16(hash) + " is not a v61 material stream: " + error;
        return false;
    }
    const auto where = "particle material " + hex16(hash);
    if (edit) {
        for (const auto& [name, values] : edit->variables) {
            const auto name_hash = stingray::name_id32(name);
            const auto found = std::find_if(parsed.variables.begin(), parsed.variables.end(),
                                            [&](const auto& variable) { return variable.name_hash == name_hash; });
            if (found == parsed.variables.end()) { error = where + " has no variable '" + name + "'"; return false; }
            const std::size_t floats = found->klass + 1u;
            if (found->klass > 3 || values.size() != floats || found->data_offset + floats * 4 > parsed.variable_data.size()) {
                error = where + ": variable '" + name + "' takes " + std::to_string(floats) + " number(s)";
                return false;
            }
            std::vector<std::uint8_t> expected(floats * 4), replacement(floats * 4), edited;
            std::memcpy(expected.data(), parsed.variable_data.data() + found->data_offset, floats * 4);
            for (std::size_t i = 0; i < floats; ++i) {
                if (!std::isfinite(values[i])) { error = where + ": variable '" + name + "' must be finite"; return false; }
                std::memcpy(replacement.data() + i * 4, &values[i], 4);
            }
            if (expected == replacement) continue;
            if (!stingray::material::clone_material_v61_with_variable_data_edit(
                    bytes, found->data_offset, expected, replacement, edited, error)) return false;
            bytes = std::move(edited);
            if (!stingray::material::parse_material_stream(bytes, parsed, error)) return false;
        }
        for (const auto& entry : edit->textures) {
            const auto channel = stingray::name_id32(entry.first);
            if (std::none_of(parsed.textures.begin(), parsed.textures.end(),
                             [&](const auto& texture) { return texture.channel_hash == channel; })) {
                error = where + " has no texture channel '" + entry.first + "'";
                return false;
            }
        }
    }
    std::vector<ResourceKey> dependencies;
    std::vector<stingray::material::TextureResourceHashEdit> texture_edits;
    for (const auto& texture : parsed.textures) {
        if (!texture.resource_hash) continue;
        ResourceKey key;
        const std::pair<const std::string, std::string>* replaced = nullptr;
        if (edit)
            for (const auto& entry : edit->textures)
                if (stingray::name_id32(entry.first) == texture.channel_hash) replaced = &entry;
        if (replaced) {
            // the image takes the place of the game's texture, so it is written the way that one is
            std::vector<std::uint8_t> file, game_body, body;
            std::string game_stream;
            stingray::texture::ImageRGBA image;
            if (!read_file(replaced->second, file, error) || !stingray::texture::decode_image_rgba(file, "", image, error) ||
                !game.resource(texture.resource_hash, "texture", game_body, game_stream, error) ||
                !stingray::texture::encode_texture_like(image, game_body, body, error)) {
                error = where + ", texture '" + replaced->first + "': " + error;
                return false;
            }
            std::ostringstream suffix;
            suffix << std::hex << std::setfill('0') << std::setw(8) << texture.channel_hash;
            const auto texture_stem = stem + "_" + suffix.str();
            key = context.generated_key("texture", texture_stem);
            graph.owned.push_back({key, texture_stem + ".texture", PreservedTexture{std::move(body), {}}, {}});
        } else if (!own_texture(context, graph, game, texture.resource_hash, key, error)) {
            return false;
        }
        texture_edits.push_back({texture.channel_hash, texture.resource_hash, stingray::resource_name_hash(key.name)});
        dependencies.push_back(key);
    }
    if (!texture_edits.empty()) {
        std::vector<std::uint8_t> edited;
        if (!stingray::material::clone_material_v61_with_texture_hash_edits(bytes, texture_edits, edited, error)) return false;
        bytes = std::move(edited);
    }
    stingray::material::MaterialDependencyHashes hashes;
    if (!stingray::material::validate_preserved_material_v61(bytes, hashes, error)) return false;
    ResourceKey owned_provider, owned_parent;
    if (hashes.shader_provider_material_hash &&
        !own_shader_chain(context, graph, game, hashes.shader_provider_material_hash, owned_provider, error)) return false;
    if (hashes.parent_material_hash &&
        !own_shader_chain(context, graph, game, hashes.parent_material_hash, owned_parent, error)) return false;
    const std::size_t section = u32(bytes, 4);
    const std::uint64_t provider = hashes.shader_provider_material_hash ? stingray::resource_name_hash(owned_provider.name) : 0;
    const std::uint64_t parent = hashes.parent_material_hash ? stingray::resource_name_hash(owned_parent.name) : 0;
    std::memcpy(bytes.data() + section + 4, &provider, 8);
    std::memcpy(bytes.data() + section + 12, &parent, 8);
    if (hashes.shader_provider_material_hash) dependencies.push_back(owned_provider);
    if (hashes.parent_material_hash) dependencies.push_back(owned_parent);
    graph.owned.push_back({out, stem + ".material", stingray::material::PreservedMaterialStream{std::move(bytes)},
                           std::move(dependencies)});
    return true;
}
// the unit a mesh particle draws: its body copied, its materials pointed at owned copies
bool own_mesh_unit(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game, std::uint64_t hash,
                   ResourceKey& out, std::string& error) {
    const auto stem = context.file_base() + "_fx_unit_" + hex16(hash);
    out = context.generated_key("unit", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    std::vector<std::uint8_t> body;
    std::string stream;
    if (!game.resource(hash, "unit", body, stream, error)) return false;
    if (!stream.empty()) { error = "the game's unit " + hex16(hash) + " streams its meshes; those are not supported yet"; return false; }
    std::map<std::uint64_t, std::vector<std::size_t>> references;
    for (std::size_t offset = 0; offset + 8 <= body.size(); ++offset) {
        std::uint64_t value;
        std::memcpy(&value, body.data() + offset, 8);
        if (game.units.count(value) || game.other.count(value)) {
            error = "the game's unit " + hex16(hash) + " uses more than materials (bones, animation, other units...); not supported yet";
            return false;
        }
        if (game.materials.count(value)) references[value].push_back(offset);
    }
    std::vector<ResourceKey> dependencies;
    for (const auto& [material_hash, offsets] : references) {
        ResourceKey material;
        if (!own_particle_material(context, graph, game, material_hash, nullptr, material, error)) return false;
        const auto owned = stingray::resource_name_hash(material.name);
        for (const auto offset : offsets) std::memcpy(body.data() + offset, &owned, 8);
        dependencies.push_back(material);
    }
    graph.owned.push_back({out, stem + ".unit", PreservedUnit{std::move(body)}, std::move(dependencies)});
    return true;
}

// materials and units may be named by resource path ("content/fx/materials/..."); use their ids
void reference_ids(json::Value& value) {
    if (value.is_object()) {
        for (auto& [name, member] : value.members) {
            if (member.is_string() && !member.string.empty() && member.string[0] != '#' &&
                (name == "unit" || name == "material" || name.rfind("material_", 0) == 0))
                member.string = "#" + hex16(stingray::resource_name_hash(member.string));
            reference_ids(member);
        }
    } else if (value.is_array()) {
        for (auto& item : value.items) reference_ids(item);
    }
}

// point the description's material and unit ids at the owned copies
void retarget_references(json::Value& value, const std::map<std::uint64_t, ResourceKey>& owned) {
    if (value.is_object()) {
        for (auto& [name, member] : value.members) {
            if (member.is_string() && member.string.size() == 17 && member.string[0] == '#' &&
                (name == "unit" || name.rfind("material", 0) == 0)) {
                const auto found = owned.find(std::stoull(member.string.substr(1), nullptr, 16));
                if (found != owned.end()) member.string = "#" + hex16(stingray::resource_name_hash(found->second.name));
            }
            retarget_references(member, owned);
        }
    } else if (value.is_array()) {
        for (auto& item : value.items) retarget_references(item, owned);
    }
}
} // namespace

bool build_particle_effects(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                            std::vector<ResourceKey>& effects, std::string& error) {
    std::map<std::string, GameFiles> extracts;
    std::set<std::string> names;
    for (const auto& effect : scene.particle_effects) {
        const auto where = "particle effect '" + effect.name + "'";
        if (effect.name.empty() || !std::all_of(effect.name.begin(), effect.name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }) ||
            !names.insert(effect.name).second) {
            error = where + ": names are unique and use lowercase letters, digits, _ or -";
            return false;
        }
        auto [found, added] = extracts.try_emplace(effect.extract);
        if (added && !found->second.load(effect.extract, error)) return false;
        const auto& game = found->second;
        json::Value description;
        if (!effect.description.empty()) {
            if (!json::parse(effect.description, description, error)) {
                error = where + ": the effect description is not valid JSON: " + error;
                return false;
            }
        } else {
            std::vector<std::uint8_t> blob, cooked;
            std::string stream;
            if (!read_file(effect.source, blob, error) ||
                !stingray::parse_cooked_resource_envelope(blob, "particles", cooked, stream, error) ||
                !stingray::particles::decode(cooked, description, error)) {
                error = where + ": " + error;
                return false;
            }
        }
        // every game material and unit the effect names is shipped as an owned copy
        reference_ids(description);
        std::map<std::uint64_t, ResourceKey> owned;
        std::vector<ResourceKey> dependencies;
        for (const auto& reference : stingray::particles::resource_references(description)) {
            if (owned.count(reference.id)) continue;
            ResourceKey key;
            if (reference.type == "unit") {
                if (!game.units.count(reference.id) || !own_mesh_unit(context, graph, game, reference.id, key, error)) {
                    if (!game.units.count(reference.id)) error = "the game has no unit " + hex16(reference.id);
                    error = where + ": " + error;
                    return false;
                }
            } else {
                const auto edit = effect.materials.find(reference.id);
                if (!game.materials.count(reference.id) ||
                    !own_particle_material(context, graph, game, reference.id,
                                           edit == effect.materials.end() ? nullptr : &edit->second, key, error)) {
                    if (!game.materials.count(reference.id)) error = "the game has no material " + hex16(reference.id);
                    error = where + ": " + error;
                    return false;
                }
            }
            owned.emplace(reference.id, key);
            dependencies.push_back(key);
        }
        for (const auto& edited : effect.materials)
            if (!owned.count(edited.first)) {
                error = where + " does not use material " + hex16(edited.first);
                return false;
            }
        retarget_references(description, owned);
        std::vector<std::uint8_t> body;
        if (!stingray::particles::encode(description, body, error)) {
            error = where + ": " + error;
            return false;
        }
        const auto key = context.generated_key("particles", effect.name);
        graph.owned.push_back({key, effect.name + ".particles", ParticlesResource{std::move(body)}, std::move(dependencies)});
        effects.push_back(key);
    }
    return true;
}
}
