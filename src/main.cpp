#include "app/compiler.h"
#include "app/asset_settings.h"
#include "app/source_inspection.h"
#include "validation/unit_validator.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/particles/particles_resource.h"
#include "stingray/flow/flow_resource.h"
#include "stingray/flow/flow_authoring.h"
#include "stingray/unit/script_data.h"
#include "stingray/material/shader_compiler.h"
#include "stingray/material/shader_section.h"
#include "stingray/physics/physics_collection.h"
#include "third_party_licenses.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

static void usage() {
    std::cout <<
        "DarktideGLBCompiler - GLB/glTF 2.0 to Darktide Stingray UNIT v115\n\n"
        "Usage:\n"
        "  DarktideGLBCompiler <input.glb|input.gltf> [--inspect] [-o output_dir] [--asset-path PATH] [--output-kind all|model|animations] [--scene INDEX] [--clip INDEX] [--simple-clip INDEX|--no-simple-animation] [--loop-clip INDEX|--once-clip INDEX] [--sm-state NAME CLIP_INDEX loop|once]... [--sm-variable NAME INITIAL MIN MAX]... [--sm-transition FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS]... [--sm-range-transition FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS VARIABLE_INDEX LOWER UPPER inclusive|exclusive inclusive|exclusive]... [--material-variant NAME] [--scale FACTOR] [--animation-translation-tolerance VALUE] [--animation-scale-tolerance VALUE] [--animation-rotation-tolerance-radians VALUE] [--animation-max-controls COUNT] [--animation-max-refinements COUNT] [--animation-max-evaluations COUNT] [--material RESOURCE_PATH] [--reference-bones FILE.bones --skeleton-resource RESOURCE_PATH] [--physics-shape geometry|convex|box|sphere|capsule] [--physics-actor static|dynamic|keyframed] [--physics-mass KG] [--physics-material default|iron|rubber] [--no-materials] [--no-images] [--no-physics] [--no-validate]\n"
        "  DarktideGLBCompiler --validate <file.unit>\n"
        "  DarktideGLBCompiler --particles <file.particles> [out.json]   (a particle effect as its JSON description)\n"
        "  DarktideGLBCompiler --particles-schema   (the particle components and their fields, as JSON)\n"
        "  DarktideGLBCompiler --flow <unit.flow> [out.json]   (a unit flow graph as JSON; reads <unit>.flowdyn beside it)\n"
        "  DarktideGLBCompiler --flow-schema   (the flow node types with their inputs, outputs and events, as JSON)\n"
        "  DarktideGLBCompiler --physics-joints <file.unit>   (the D6 joints of a unit's physics collection, one per line)\n"
        "  DarktideGLBCompiler --physics-collection <file.unit>   (a unit's physics bodies, shapes and joints, as JSON)\n"
        "  DarktideGLBCompiler --physics-meshes <file>   (cooked PhysX shapes as vertices and triangles, JSON; the file holds\n"
        "      records of u32 type 3 triangle mesh / 4 convex, u32 size, cooked bytes)\n"
        "  DarktideGLBCompiler --flow-build <flow.json>   (an authored flow graph as the --flow description)\n"
        "  DarktideGLBCompiler --unit-data <file>   (a unit's script data bytes as JSON)\n"
        "  DarktideGLBCompiler --shader <material stream>   (the compiled shaders of a shader material, summarized as JSON)\n"
        "  DarktideGLBCompiler --shader-reflect <stage.dxbc>...   (binding records of compiled stages of one shader group, in order)\n"
        "  DarktideGLBCompiler --shader-stages <material stream> <out dir>   (unpacked vertex and pixel stages of a shader material, with index.json)\n"
        "  DarktideGLBCompiler --texture-image <file.texture> <out.png>   (the largest mip a game texture holds itself, as a PNG; prints its size and srgb or linear)\n"
        "  DarktideGLBCompiler --configure-oodle <game directory or oo2core_9_win64.dll>\n"
        "  DarktideGLBCompiler --capabilities\n\n"
        "  DarktideGLBCompiler --licenses\n\n"
        "--inspect reads and summarizes source content without compiling or writing output; only --scene may be combined with it.\n"
        "The compiler converts what has a direct Darktide equivalent and records every approximation it makes\n"
        "in build.log and compile_manifest.json. Output is UNIT, BONES, ANIMATION, STATE_MACHINE, MATERIAL and\n"
        "TEXTURE resources plus a build.json for the Custom Assets patcher.\n"
        "--output-kind selects a resource closure; all is the default. Model excludes animation clips;\n"
        "animations emits clips and their BONES dependency without model/material resources.\n"
        "--material explicitly records an existing Darktide material resource as an approximation for GLB materials.\n"
        "--material-variant selects a named KHR_materials_variants variant from the source glTF.\n"
        "Without a state machine, the first emitted clip (or --simple-clip INDEX) is embedded as the UNIT's\n"
        "simple animation, played from Lua with Unit.play_simple_animation; --no-simple-animation omits it.\n"
        "--loop-clip emits a minimal looping STATE_MACHINE for an emitted clip in all-output mode.\n"
        "--once-clip emits a minimal one-shot STATE_MACHINE for an emitted clip in all-output mode.\n"
        "--sm-state and --sm-transition author direct-event STATE_MACHINE states and transitions in all-output mode.\n"
        "@FILE anywhere stands for the arguments in FILE, one per line.\n"
        "--sm-declare-events A,B,... lists further events scripts may send that no transition reacts to (the engine\n"
        "stops on an event its state machine doesn't list); #1234abcd names an event by its hash.\n"
        "--in-place removes the horizontal root travel from every clip so walk/run cycles loop on the spot;\n"
        "build.log notes each clip's speed for moving the unit from Lua.\n"
        "--root-motion moves that travel onto the skeleton root (root_point) instead, where the engine reads it\n"
        "as root motion (Unit.animation_wanted_root_pose), like the game's character clips.\n"
        "--package-name NAME names the package the asset is loaded with (default: Custom Assets picks one). Give an\n"
        "enemy body its own resource path: the game loads a breed's base_unit as a package of that name.\n"
        "--ragdoll-event NAME adds a ragdoll state entered on NAME from every state; dynamic node bodies stay\n"
        "uncreated until then (retail minion ragdoll setup).\n"
        "--scale uniformly converts all spatial data before Stingray coordinate conversion (1 keeps glTF units).\n"
        "--asset-path sets a stable extensionless resource identity, e.g. content/mods/my_mod/trolley.\n"
        "It also names generated dependencies; keep one asset build per output directory.\n"
        "A plain glTF without authored colliders or a physics shape builds without a physics actor.\n"
        "When requested, geometry physics uses the exact triangle surface for static/keyframed actors;\n"
        "dynamic actors use a PhysX convex hull per source object/instance. Convex, fitted\n"
        "box, sphere, and capsule remain explicit fitted alternatives.\n"
        "Actor, mass, and material controls require a shape. --no-physics wins regardless of argument order.\n"
        "--no-images is accepted as a legacy no-op; resource-only output is unconditional.\n"
        "--allow-fixed-morph-bake consents to keeping only the initial morph pose.\n"
        "Continuous animation is adaptively fitted to measured tolerances; animation tolerance and work-limit flags configure the fit.\n"
        "--allow-animation-resampling and --allow-native-animation-interpolation are accepted legacy flags; they do not bypass fitting.\n";
}

static void capabilities() {
    // Keep this machine-readable so front ends do not duplicate compiler policy.
    // other tools might parse this so dont go renaming keys
    std::cout << "{\"schema\":1,\"output_kinds\":[\"all\",\"model\",\"animations\"],\"inspect\":true,\"inspect_options\":[\"--scene INDEX\"],"
                 "\"asset_path\":true,"
                 "\"simple_animation\":{\"options\":[\"--simple-clip INDEX\",\"--no-simple-animation\"],\"default\":\"first emitted clip when no state machine\",\"lua\":\"Unit.play_simple_animation(unit, from, to, loop, speed)\"},"
                 "\"material_variants\":{\"option\":\"--material-variant NAME\",\"settings_key\":\"material_variant\",\"extension\":\"KHR_materials_variants\"},"
                 "\"reference_bones\":{\"options\":[\"--reference-bones\",\"--skeleton-resource\"],\"mapping\":\"exact_joint_set\"},"
                 "\"physics_shapes\":[\"geometry\",\"convex\",\"box\",\"sphere\",\"capsule\"],"
                 "\"physics_actors\":[\"static\",\"dynamic\",\"keyframed\"],"
                 "\"physics_materials\":[\"default\",\"iron\",\"rubber\"],"
                 "\"physics_rules\":{\"geometry\":\"exact triangle mesh for static/keyframed; per-object/instance convex hull for dynamic\",\"convex\":\"per-object/instance PhysX convex hull\",\"box\":\"single fitted box\",\"sphere\":\"single fitted sphere\",\"capsule\":\"single fitted capsule along longest bounds axis\"},"
                 "\"actor_rules\":{\"static\":\"fixed world collision; mass is ignored\",\"dynamic\":\"simulated rigid body; geometry is cooked as convex hulls\",\"keyframed\":\"externally moved collision; exact geometry is allowed\"},"
                 "\"material_modes\":[\"automatic\",\"external\",\"none\"],"
                 "\"material_slot_extras\":{\"name\":\"darktide_material\",\"version\":1,\"modes\":[\"generated\",\"external\"],\"template\":\"compiler_gap\"},"
                 "\"material_surface_options\":[\"default\",\"metal_solid\",\"metal_sheet\",\"cloth\",\"concrete\",\"brick\",\"bone\",\"plastic\"],"
                 "\"material_rules\":{\"automatic\":\"convert each glTF material and its referenced textures\",\"external\":\"bind every mesh slot to an existing Darktide material resource\",\"none\":\"emit geometry without material resources\"},"
                 "\"scale_presets\":[\"1\",\"0.1\",\"0.01\",\"0.001\"],"
                 "\"external_material_presets\":[\"content/parent_materials/substance_basic\",\"content/parent_materials/substance_basic_transparent\"]}\n";
}

// A particles resource as its JSON description (stdout or a file); the description is written back to binary
// and must give the same bytes.
// A unit flow blob (and its dynamic data, <name>.flowdyn) as its JSON description; it must encode back to the
// same bytes.
static bool flow_description(const fs::path& input, const fs::path& output, std::string& error) {
    const auto read = [](const fs::path& path, std::vector<std::uint8_t>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    };
    std::vector<std::uint8_t> flow, dynamic_data, flow_again, dynamic_again;
    if (!read(input, flow)) { error = "cannot read the file"; return false; }
    auto dynamic_path = input;
    dynamic_path.replace_extension(".flowdyn");
    if (fs::exists(dynamic_path) && !read(dynamic_path, dynamic_data)) { error = "cannot read " + dynamic_path.string(); return false; }
    dtglb::json::Value graph;
    if (!dtglb::stingray::flow::decode(flow, dynamic_data, graph, error)) return false;
    if (!dtglb::stingray::flow::encode(graph, flow_again, dynamic_again, error)) { error = "description does not encode: " + error; return false; }
    if (flow_again != flow || dynamic_again != dynamic_data) {
        std::size_t at = 0;
        while (at < flow_again.size() && at < flow.size() && flow_again[at] == flow[at]) ++at;
        error = "description encodes to different bytes (flow first difference at byte " + std::to_string(at) + ")";
        return false;
    }
    const auto text = dtglb::json::dump(graph) + "\n";
    if (output.empty()) { std::cout << text; return true; }
    std::ofstream out(output, std::ios::binary);
    out << text;
    if (!out) { error = "cannot write " + output.string(); return false; }
    return true;
}

static bool particles_description(const fs::path& input, const fs::path& output, std::string& error) {
    std::ifstream file(input, std::ios::binary);
    const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> body, again;
    std::string stream;
    dtglb::json::Value effect;
    if (!file.good() && !file.eof()) { error = "cannot read the file"; return false; }
    if (!dtglb::stingray::parse_cooked_resource_envelope(blob, "particles", body, stream, error) ||
        !dtglb::stingray::particles::decode(body, effect, error)) return false;
    if (!dtglb::stingray::particles::encode(effect, again, error)) { error = "description does not encode: " + error; return false; }
    if (again != body) {
        std::size_t at = 0;
        while (at < again.size() && at < body.size() && again[at] == body[at]) ++at;
        error = "description encodes to different bytes (first difference at byte " + std::to_string(at) + ", " +
            std::to_string(again.size()) + " bytes instead of " + std::to_string(body.size()) + ")";
        return false;
    }
    const auto text = dtglb::json::dump(effect) + "\n";
    if (output.empty()) { std::cout << text; return true; }
    std::ofstream out(output, std::ios::binary);
    out << text;
    if (!out) { error = "cannot write " + output.string(); return false; }
    return true;
}

static bool parse_finite_float(const std::string& value, float& result, bool allow_zero) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed > std::numeric_limits<float>::max() ||
        (allow_zero ? parsed < 0.0 : parsed <= 0.0)) return false;
    result = static_cast<float>(parsed);
    return true;
}

static bool parse_signed_finite_float(const std::string& value, float& result) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < std::numeric_limits<float>::lowest() ||
        parsed > std::numeric_limits<float>::max()) return false;
    result = static_cast<float>(parsed);
    return true;
}

static bool parse_positive_double(const std::string& value, double& result) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed <= 0.0) return false;
    result = parsed;
    return true;
}

static bool parse_scene_index(const std::string& value, int& result) {
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

static bool parse_positive_int(const std::string& value, int& result) {
    if (!parse_scene_index(value, result)) return false;
    return result > 0;
}

int main(int argc, char** argv) {
    // @file stands for the arguments in that file, one per line (a big state machine doesn't fit a command line)
    std::vector<std::string> expanded;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i == 0 || arg.size() < 2 || arg[0] != '@') { expanded.push_back(arg); continue; }
        std::ifstream file(fs::u8path(arg.substr(1)));
        if (!file) { std::cerr << "cannot read argument file " << arg.substr(1) << "\n"; return 1; }
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            expanded.push_back(line);
        }
    }
    std::vector<char*> expanded_argv;
    for (auto& arg : expanded) expanded_argv.push_back(arg.data());
    expanded_argv.push_back(nullptr);
    argc = static_cast<int>(expanded.size());
    argv = expanded_argv.data();
    if (argc < 2) { usage(); return 1; }
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { usage(); return 0; }
    if (std::string(argv[1]) == "--licenses") { if (argc != 2) { usage(); return 1; } std::cout << darktide_third_party_licenses; return 0; }
    if (std::string(argv[1]) == "--capabilities") { if (argc != 2) { usage(); return 1; } capabilities(); return 0; }
    if (std::string(argv[1]) == "--configure-oodle") {
        fs::path dll_or_game_directory;
#ifdef _WIN32
        int wide_argc = 0;
        LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
        if (!wide_argv || wide_argc != 3) {
            if (wide_argv) LocalFree(wide_argv);
            usage();
            return 1;
        }
        dll_or_game_directory = fs::path(wide_argv[2]);
        LocalFree(wide_argv);
#else
        if (argc != 3) { usage(); return 1; }
        dll_or_game_directory = fs::u8path(argv[2]);
#endif
        std::string error;
        if (!dtglb::stingray::texture::configure_oodle(dll_or_game_directory, error)) {
            std::cerr << "Oodle configuration failed: " << error << '\n';
            return 2;
        }
        std::cout << "Oodle configured.\n";
        return 0;
    }
    if (std::string(argv[1]) == "--validate") {
        if (argc < 3) { usage(); return 1; }
        std::string report;
        const bool ok = dtglb::validation::validate_unit_v115(argv[2], report);
        std::cout << report << "\n";
        return ok ? 0 : 2;
    }
    if (std::string(argv[1]) == "--particles-schema") {
        if (argc != 2) { usage(); return 1; }
        std::cout << dtglb::json::dump(dtglb::stingray::particles::schema()) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--unit-data") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        dtglb::json::Value data;
        std::vector<std::uint8_t> again;
        std::string error;
        if (!file.good() && !file.eof()) error = "cannot read the file";
        else if (dtglb::stingray::unit::decode_script_data(bytes, data, error) &&
                 dtglb::stingray::unit::encode_script_data(data, again, error) && again != bytes)
            error = "data encodes to different bytes";
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << dtglb::json::dump(data) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--shader") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        std::uint32_t header[7] = {};
        if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
        dtglb::stingray::material::ShaderSection section;
        if (bytes.size() < sizeof(header)) error = "not a material stream";
        else if (!header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) error = "the stream has no shader section";
        else dtglb::stingray::material::decode_shader_section(
            std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << dtglb::json::dump(dtglb::stingray::material::describe_shader_section(section)) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--shader-stages") {
        if (argc != 4) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        std::uint32_t header[7] = {};
        if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
        dtglb::stingray::material::ShaderSection section;
        if (bytes.size() < sizeof(header)) error = "not a material stream";
        else if (!header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) error = "the stream has no shader section";
        else dtglb::stingray::material::decode_shader_section(
            std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        // index.json: contexts (shader names + condition offsets), the condition programs, and per shader its
        // passes with layer, render states, sampler states and the stage files written next to it
        using dtglb::json::Value;
        const fs::path out = fs::u8path(argv[3]);
        std::error_code ec;
        fs::create_directories(out, ec);
        auto root = Value::object();
        auto contexts = Value::array();
        for (const auto& c : section.contexts) {
            auto context = Value::object();
            context.set("name", Value::of(static_cast<double>(c.name)));
            auto shaders = Value::array();
            for (const auto& [shader, condition] : c.shaders) {
                auto pair = Value::array();
                pair.push(Value::of(static_cast<double>(shader)));
                pair.push(Value::of(static_cast<double>(condition)));
                shaders.push(std::move(pair));
            }
            context.set("shaders", std::move(shaders));
            contexts.push(std::move(context));
        }
        root.set("contexts", std::move(contexts));
        static const char digits[] = "0123456789abcdef";
        std::string conditions;
        for (const auto byte : section.conditions) { conditions += digits[byte >> 4]; conditions += digits[byte & 15]; }
        root.set("conditions", Value::of(conditions));
        auto states = [](const std::vector<dtglb::stingray::material::RenderState>& list) {
            auto array = Value::array();
            for (const auto& state : list) {
                auto pair = Value::array();
                pair.push(Value::of(static_cast<double>(state.key)));
                pair.push(Value::of(static_cast<double>(state.value)));
                array.push(std::move(pair));
            }
            return array;
        };
        static const char* stage_names[] = {"vertex", "domain", "hull", "geometry", "pixel", "compute"};
        auto shaders = Value::array();
        for (std::size_t i = 0; i < section.shader_data.size() && error.empty(); ++i) {
            const auto& data = section.shader_data[i];
            auto shader = Value::object();
            shader.set("name", Value::of(static_cast<double>(data.name)));
            auto passes = Value::array();
            for (std::size_t p = 0; p < data.passes.size() && error.empty(); ++p) {
                auto pass = Value::object();
                pass.set("layer", Value::of(static_cast<double>(data.passes[p].layer)));
                const dtglb::stingray::material::ShaderPass* device = nullptr;
                for (const auto& d : section.device_data)
                    if (i < d.groups.size() && p < d.groups[i].size()) device = &d.groups[i][p];
                if (device) {
                    pass.set("render_states", states(device->render_states));
                    auto samplers = Value::object();
                    for (const auto& sampler : device->sampler_states) samplers.set(std::to_string(sampler.name), states(sampler.states));
                    pass.set("sampler_states", std::move(samplers));
                    for (const std::size_t k : {std::size_t{0}, std::size_t{4}}) {
                        if (device->stages[k].empty()) continue;
                        const auto& variant = device->stages[k].front();
                        std::vector<std::uint8_t> unpacked(variant.size);
                        if (!dtglb::stingray::texture::oodle_decompress(variant.bytecode, unpacked, error)) break;
                        const auto name = "s" + std::to_string(i) + "_p" + std::to_string(p) + "_" + stage_names[k] + ".dxbc";
                        std::ofstream stage(out / name, std::ios::binary);
                        stage.write(reinterpret_cast<const char*>(unpacked.data()), static_cast<std::streamsize>(unpacked.size()));
                        if (!stage) { error = "cannot write " + (out / name).string(); break; }
                        pass.set(stage_names[k], Value::of(name));
                    }
                }
                passes.push(std::move(pass));
            }
            shader.set("passes", std::move(passes));
            shaders.push(std::move(shader));
        }
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        root.set("shaders", std::move(shaders));
        std::ofstream index(out / "index.json", std::ios::binary);
        index << dtglb::json::dump(root) << '\n';
        if (!index) { std::cerr << "cannot write " << (out / "index.json").string() << '\n'; return 2; }
        return 0;
    }
    if (std::string(argv[1]) == "--texture-image") {
        if (argc != 4) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // a limn extract's texture: 38-byte envelope, body size at 29; most bodies are stubs naming a loose file
        // under <game>/bundle that holds the real body
        std::string error;
        std::uint32_t body_size = 0;
        if (bytes.size() >= 38) std::memcpy(&body_size, bytes.data() + 29, 4);
        std::vector<std::uint8_t> body;
        if (bytes.size() < 38 || 38ull + body_size > bytes.size()) error = "not a texture resource";
        else body.assign(bytes.begin() + 38, bytes.begin() + 38 + body_size);
        if (error.empty() && body.size() >= 5 && std::memcmp(body.data(), "data/", 5) == 0) {
            const std::string loose(reinterpret_cast<const char*>(body.data()), strnlen(reinterpret_cast<const char*>(body.data()), body.size()));
            const fs::path path = dtglb::stingray::texture::game_root() / "bundle" / loose;
            std::ifstream source(path, std::ios::binary);
            if (!source) error = "names " + loose + ", which is not in the game folder";
            else body.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
        }
        dtglb::stingray::texture::ImageRGBA image;
        bool srgb = false;
        if (error.empty() && dtglb::stingray::texture::texture_body_image(body, image, error, &srgb))
            dtglb::stingray::texture::write_png(image, fs::u8path(argv[3]), error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << image.width << "x" << image.height << (srgb ? " srgb" : " linear") << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--physics-meshes") {
        // records of u32 type (3 triangle mesh, 4 convex), u32 size, cooked bytes
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(fs::u8path(argv[2]), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> meshes;
        std::string error, text;
        for (std::size_t at = 0; at < bytes.size();) {
            std::uint32_t type = 0, size = 0;
            if (bytes.size() - at < 8) { error = "the mesh file is truncated"; break; }
            std::memcpy(&type, bytes.data() + at, 4);
            std::memcpy(&size, bytes.data() + at + 4, 4);
            at += 8;
            if (size > bytes.size() - at) { error = "the mesh file is truncated"; break; }
            meshes.emplace_back(type, std::vector<std::uint8_t>(bytes.begin() + at, bytes.begin() + at + size));
            at += size;
        }
        if (error.empty()) dtglb::stingray::physics::describe_cooked_meshes(meshes, text, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << text;
        return 0;
    }
    if (std::string(argv[1]) == "--physics-joints" || std::string(argv[1]) == "--physics-collection") {
        // the unit's physics scene: u32 size, then the dependency and object collections (each starting "SEBD")
        // with a 128-byte header at the end giving their offsets and sizes
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const char magic[] = {'S', 'E', 'B', 'D'};
        const auto start = std::search(bytes.begin(), bytes.end(), std::begin(magic), std::end(magic));
        std::string error, text;
        const std::size_t at = static_cast<std::size_t>(start - bytes.begin());
        std::uint32_t size = 0;
        if (start == bytes.end() || at < 4) error = "the unit has no physics collections";
        else {
            std::memcpy(&size, bytes.data() + at - 4, 4);
            if (size < 128 || size > bytes.size() - at) error = "the physics scene is truncated";
        }
        if (error.empty()) {
            const auto* scene = bytes.data() + at;
            const auto word = [&](std::size_t offset) { std::uint32_t v; std::memcpy(&v, scene + size - 128 + offset, 4); return v; };
            const auto slice = [&](std::uint32_t offset, std::uint32_t length) {
                return offset <= size && length <= size - offset ? std::vector<std::uint8_t>(scene + offset, scene + offset + length)
                                                                 : std::vector<std::uint8_t>();
            };
            const auto dependencies = slice(word(8), word(12)), objects = slice(word(80), word(84));
            if (std::string(argv[1]) == "--physics-collection")
                dtglb::stingray::physics::describe_physics_collection(std::vector<std::uint8_t>(scene, scene + size), text, error);
            else if (dependencies.empty() || objects.empty()) error = "the physics scene header does not match";
            else dtglb::stingray::physics::describe_physics_joints(dependencies, objects, text, error);
        }
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << text;
        return 0;
    }
    if (std::string(argv[1]) == "--shader-reflect") {
        if (argc < 3) { usage(); return 1; }
        dtglb::stingray::material::RootTable roots;
        auto out = dtglb::json::Value::array();
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            const std::vector<std::uint8_t> dxbc((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            dtglb::stingray::material::ShaderVariant variant;
            std::string error;
            if (dxbc.empty()) error = "cannot read the file";
            else dtglb::stingray::material::make_shader_variant(dxbc, roots, variant, error);
            if (!error.empty()) { std::cerr << argv[i] << ": " << error << '\n'; return 2; }
            out.push(dtglb::stingray::material::describe_shader_variant(variant));
        }
        std::cout << dtglb::json::dump(out) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-build") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        dtglb::json::Value authored, graph;
        std::string error;
        if (!file || !dtglb::json::parse(text, authored, error) || !dtglb::stingray::flow::build_graph(authored, graph, error)) {
            std::cerr << argv[2] << ": " << (error.empty() ? "cannot read the file" : error) << '\n';
            return 2;
        }
        std::cout << dtglb::json::dump(graph) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-schema") {
        if (argc != 2) { usage(); return 1; }
        std::cout << dtglb::json::dump(dtglb::stingray::flow::authoring_schema()) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow") {
        if (argc < 3 || argc > 4) { usage(); return 1; }
        std::string error;
        if (!flow_description(fs::path(argv[2]), argc == 4 ? fs::path(argv[3]) : fs::path(), error)) {
            std::cerr << argv[2] << ": " << error << '\n';
            return 2;
        }
        return 0;
    }
    if (std::string(argv[1]) == "--particles") {
        if (argc < 3 || argc > 4) { usage(); return 1; }
        std::string error;
        if (!particles_description(fs::path(argv[2]), argc == 4 ? fs::path(argv[3]) : fs::path(), error)) {
            std::cerr << argv[2] << ": " << error << '\n';
            return 2;
        }
        return 0;
    }

    dtglb::app::CompileOptions opt;
    opt.input = fs::path(argv[1]);
    opt.output_dir = opt.input.parent_path() / (opt.input.stem().string() + "_compiled");
    bool inspect_mode = false;
    for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--inspect") inspect_mode = true;
    if (inspect_mode) {
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--inspect") continue;
            if (option == "--scene") {
                if (i + 1 < argc) ++i;
                continue;
            }
            std::cerr << "--inspect supports only --scene; compile option cannot be used with inspection: "
                      << option << '\n';
            return 1;
        }
    }
    bool physics_shape_set = false;
    bool physics_modifier_set = false;
    bool no_physics_requested = false;
    bool physics_actor_set = false;
    bool physics_mass_set = false;
    bool physics_material_set = false;
    const auto fitted_physics = [&]() -> dtglb::stingray::physics::FittedActorOptions& {
        opt.physics = true;
        if (!opt.fitted_physics) opt.fitted_physics.emplace();
        return *opt.fitted_physics;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--inspect") { /* handled above */ }
        else if ((a == "-o" || a == "--output") && i + 1 < argc) opt.output_dir = fs::path(argv[++i]);
        else if (a == "--material" && i + 1 < argc) opt.material_override = argv[++i];
        else if (a == "--material-variant") {
            if (i + 1 >= argc) { std::cerr << "Missing material variant name\n"; return 1; }
            opt.material_variant = argv[++i];
            if (opt.material_variant->empty()) { std::cerr << "Material variant name must not be empty\n"; return 1; }
        }
        else if (a == "--settings" && i + 1 < argc) opt.settings_path = fs::path(argv[++i]);
        else if (a == "--asset-path") {
            if (i + 1 >= argc) { std::cerr << "Missing asset path\n"; return 1; }
            opt.asset_path = argv[++i];
        }
        else if (a == "--reference-bones") {
            if (i + 1 >= argc) { std::cerr << "Missing reference BONES file\n"; return 1; }
            opt.reference_bones_file = fs::path(argv[++i]);
        }
        else if (a == "--skeleton-resource") {
            if (i + 1 >= argc) { std::cerr << "Missing skeleton resource identity\n"; return 1; }
            opt.skeleton_resource = argv[++i];
        }
        else if (a == "--output-kind" && i + 1 < argc) {
            const std::string kind = argv[++i];
            if (kind == "all") opt.output_kind = dtglb::app::OutputKind::All;
            else if (kind == "model") opt.output_kind = dtglb::app::OutputKind::Model;
            else if (kind == "animations") opt.output_kind = dtglb::app::OutputKind::Animations;
            else { std::cerr << "Invalid output kind: " << kind << "\n"; return 1; }
        }
        else if (a == "--no-materials") opt.auto_materials = false;
        else if (a == "--no-images") { /* legacy compatibility; images are never emitted */ }
        else if (a == "--no-physics") { no_physics_requested = true; }
        else if (a == "--no-validate") opt.validate = false;
        else if (a == "--allow-fixed-morph-bake") opt.allow_fixed_morph_bake = true;
        else if (a == "--allow-animation-resampling") opt.allow_animation_resampling = true;
        else if (a == "--allow-native-animation-interpolation") opt.allow_native_animation_interpolation = true;
        else if (a == "--scale") {
            if (i + 1 >= argc) { std::cerr << "Missing scale factor\n"; return 1; }
            float value = 0.0f;
            if (!parse_finite_float(argv[++i], value, false)) { std::cerr << "Invalid scale factor: " << argv[i] << "\n"; return 1; }
            opt.asset_scale = value;
        }
        else if (a == "--animation-translation-tolerance" || a == "--animation-scale-tolerance" ||
                 a == "--animation-rotation-tolerance-radians") {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << a << "\n"; return 1; }
            double value = 0.0;
            if (!parse_positive_double(argv[++i], value)) { std::cerr << "Invalid positive finite value for " << a << ": " << argv[i] << "\n"; return 1; }
            if (a == "--animation-translation-tolerance") opt.animation_fit_overrides.translation_tolerance = value;
            else if (a == "--animation-scale-tolerance") opt.animation_fit_overrides.scale_tolerance = value;
            else opt.animation_fit_overrides.rotation_tolerance_radians = value;
        }
        else if (a == "--animation-max-controls" || a == "--animation-max-refinements" ||
                 a == "--animation-max-evaluations") {
            if (i + 1 >= argc) { std::cerr << "Missing count for " << a << "\n"; return 1; }
            int value = 0;
            if (!parse_positive_int(argv[++i], value)) { std::cerr << "Invalid positive integer for " << a << ": " << argv[i] << "\n"; return 1; }
            if (a == "--animation-max-controls") opt.animation_fit_overrides.max_controls = static_cast<std::size_t>(value);
            else if (a == "--animation-max-refinements") opt.animation_fit_overrides.max_refinements = static_cast<std::size_t>(value);
            else opt.animation_fit_overrides.max_evaluations = static_cast<std::size_t>(value);
        }
        else if (a == "--physics-shape") {
            if (i + 1 >= argc) { std::cerr << "Missing physics shape\n"; return 1; }
            const std::string shape = argv[++i];
            if (shape == "geometry" || shape == "mesh") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::TriangleMesh;
            else if (shape == "convex") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Convex;
            else if (shape == "box") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Box;
            else if (shape == "sphere") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Sphere;
            else if (shape == "capsule") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Capsule;
            else { std::cerr << "Invalid physics shape: " << shape << "\n"; return 1; }
            physics_shape_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-actor") {
            if (i + 1 >= argc) { std::cerr << "Missing physics actor type\n"; return 1; }
            const std::string actor = argv[++i];
            if (actor != "static" && actor != "dynamic" && actor != "keyframed") {
                std::cerr << "Invalid physics actor type: " << actor << "\n"; return 1;
            }
            fitted_physics().actor_template = actor;
            physics_actor_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-mass") {
            if (i + 1 >= argc) { std::cerr << "Missing physics mass\n"; return 1; }
            float value = 0.0f;
            if (!parse_finite_float(argv[++i], value, true)) { std::cerr << "Invalid physics mass: " << argv[i] << "\n"; return 1; }
            fitted_physics().mass = value;
            physics_mass_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-material") {
            if (i + 1 >= argc) { std::cerr << "Missing physics material\n"; return 1; }
            const std::string material = argv[++i];
            if (material != "default" && material != "iron" && material != "rubber") {
                std::cerr << "Invalid physics material: " << material << "\n"; return 1;
            }
            fitted_physics().material = material;
            physics_material_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--scene") {
            if (i + 1 >= argc) { std::cerr << "Missing scene index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid scene index: " << argv[i] << "\n"; return 1; }
            opt.scene_index = index;
        }
        else if (a == "--clip") {
            if (i + 1 >= argc) { std::cerr << "Missing clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid clip index: " << argv[i] << "\n"; return 1; }
            opt.animation_index = index;
        }
        else if (a == "--loop-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing loop clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid loop clip index: " << argv[i] << "\n"; return 1; }
            opt.loop_clip_index = index;
        }
        else if (a == "--simple-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing simple-animation clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid simple-animation clip index: " << argv[i] << "\n"; return 1; }
            opt.simple_clip_index = index;
        }
        else if (a == "--no-simple-animation") { opt.simple_animation = false; }
        else if (a == "--in-place") { opt.in_place = true; }
        else if (a == "--root-motion") { opt.root_motion = true; }
        else if (a == "--package-name") {
            if (i + 1 >= argc || !*argv[i + 1]) { std::cerr << "--package-name requires a resource name\n"; return 1; }
            opt.package_name = argv[++i];
        }
        else if (a == "--ragdoll-event") {
            if (i + 1 >= argc || !*argv[i + 1]) { std::cerr << "--ragdoll-event requires an event name\n"; return 1; }
            opt.ragdoll_event = argv[++i];
        }
        else if (a == "--once-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing one-shot clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid one-shot clip index: " << argv[i] << "\n"; return 1; }
            opt.once_clip_index = index;
        }
        else if (a == "--sm-state") {
            if (i + 3 >= argc) { std::cerr << "--sm-state requires NAME CLIP_INDEX loop|once\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            if (!parse_scene_index(argv[++i], state.clip_index)) {
                std::cerr << "Invalid --sm-state clip index: " << argv[i] << "\n"; return 1;
            }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") {
                std::cerr << "Invalid --sm-state mode: " << mode << " (expected loop or once)\n"; return 1;
            }
            state.looping = mode == "loop";
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-blend") {
            if (i + 4 >= argc) { std::cerr << "--sm-blend requires NAME VARIABLE_INDEX[,VARIABLE_INDEX] loop|once CLIP:VALUE[:VALUE2][,...]\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            // one variable index, or two (X,Y) for a 2D blend whose clips are CLIP:X:Y
            const std::string variables = argv[++i];
            const auto comma = variables.find(',');
            if (!parse_scene_index(variables.substr(0, comma), state.blend_variable) ||
                (comma != std::string::npos && !parse_scene_index(variables.substr(comma + 1), state.blend_variable2))) {
                std::cerr << "Invalid --sm-blend variable index: " << variables << "\n"; return 1;
            }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") {
                std::cerr << "Invalid --sm-blend mode: " << mode << " (expected loop or once)\n"; return 1;
            }
            state.looping = mode == "loop";
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                const auto second = colon == std::string::npos ? colon : entry.find(':', colon + 1);
                const bool two_d = state.blend_variable2 >= 0;
                int clip = -1;
                float value = 0.0f, value2 = 0.0f;
                if (colon == std::string::npos || (second != std::string::npos) != two_d ||
                    !parse_scene_index(entry.substr(0, colon), clip) ||
                    !parse_signed_finite_float(entry.substr(colon + 1, second == std::string::npos ? std::string::npos : second - colon - 1), value) ||
                    (two_d && !parse_signed_finite_float(entry.substr(second + 1), value2))) {
                    std::cerr << "Invalid --sm-blend clip entry: " << entry << (two_d ? " (expected CLIP:X:Y)\n" : " (expected CLIP:VALUE)\n"); return 1;
                }
                state.blend.push_back({clip, value});
                state.blend_value2.push_back(value2);
            }
            if (state.blend.empty()) { std::cerr << "--sm-blend needs at least one CLIP:VALUE\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-random") {
            if (i + 4 >= argc) { std::cerr << "--sm-random requires NAME on_entry|every_loop|no_repeat loop|once CLIP:WEIGHT[,CLIP:WEIGHT...]\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            const std::string pick = argv[++i];
            if (pick == "on_entry") state.randomization = 0;
            else if (pick == "every_loop") state.randomization = 1;
            else if (pick == "no_repeat") state.randomization = 2;
            else { std::cerr << "Invalid --sm-random pick mode: " << pick << " (on_entry, every_loop or no_repeat)\n"; return 1; }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") { std::cerr << "Invalid --sm-random mode: " << mode << " (expected loop or once)\n"; return 1; }
            state.looping = mode == "loop";
            state.random = true;
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                int clip = -1;
                float weight = 1.0f;
                if (!parse_scene_index(entry.substr(0, colon), clip) ||
                    (colon != std::string::npos && !parse_signed_finite_float(entry.substr(colon + 1), weight)) || !(weight > 0.0f)) {
                    std::cerr << "Invalid --sm-random clip entry: " << entry << " (expected CLIP or CLIP:WEIGHT, weight above 0)\n"; return 1;
                }
                state.blend.push_back({clip, weight});
            }
            if (state.blend.size() < 2) { std::cerr << "--sm-random needs at least two clips\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-speed") {
            if (i + 2 >= argc) { std::cerr << "--sm-speed requires STATE_NAME SPEED|var:VARIABLE_INDEX\n"; return 1; }
            const std::string name = argv[++i];
            const std::string value = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << "--sm-speed names no earlier --sm-state or --sm-blend: " << name << "\n"; return 1; }
            if (value.rfind("var:", 0) == 0) {
                if (!parse_scene_index(value.substr(4), state->speed_variable)) { std::cerr << "Invalid --sm-speed variable index: " << value << "\n"; return 1; }
            } else if (!parse_signed_finite_float(value, state->speed)) {
                std::cerr << "Invalid --sm-speed value: " << value << "\n"; return 1;
            }
        }
        else if (a == "--sm-empty") {
            if (i + 2 >= argc) { std::cerr << "--sm-empty requires NAME LAYER\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            state.empty = true;
            if (!parse_scene_index(argv[++i], state.layer)) { std::cerr << "Invalid --sm-empty layer: " << argv[i] << "\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-events" || a == "--sm-exit") {
            const bool exit = a == "--sm-exit";
            if (i + (exit ? 3 : 2) >= argc) {
                std::cerr << a << (exit ? " requires STATE_NAME EVENT SECONDS_LEFT\n" : " requires STATE_NAME SECONDS:EVENT[,SECONDS:EVENT...]\n");
                return 1;
            }
            const std::string name = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << a << " names no earlier state: " << name << "\n"; return 1; }
            if (exit) {
                state->exit_event = argv[++i];
                if (state->exit_event.empty() || !parse_signed_finite_float(argv[++i], state->exit_blend) || state->exit_blend < 0.0f) {
                    std::cerr << "Invalid --sm-exit event or time\n"; return 1;
                }
                continue;
            }
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                float time = 0.0f;
                if (colon == std::string::npos || colon + 1 == entry.size() ||
                    !parse_signed_finite_float(entry.substr(0, colon), time) || time < 0.0f) {
                    std::cerr << "Invalid --sm-events entry: " << entry << " (expected SECONDS:EVENT)\n"; return 1;
                }
                state->events_at.push_back({time, entry.substr(colon + 1)});
            }
        }
        else if (a == "--sm-layer" || a == "--sm-mask" || a == "--sm-additive") {
            const bool takes_value = a != "--sm-additive";
            if (i + (takes_value ? 2 : 1) >= argc) {
                std::cerr << a << (a == "--sm-layer" ? " requires STATE_NAME LAYER\n" : a == "--sm-mask" ? " requires STATE_NAME BONE[:WEIGHT][,BONE[:WEIGHT]...]\n" : " requires STATE_NAME\n");
                return 1;
            }
            const std::string name = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << a << " names no earlier state: " << name << "\n"; return 1; }
            if (a == "--sm-additive") { state->additive = true; continue; }
            const std::string value = argv[++i];
            if (a == "--sm-layer") {
                if (!parse_scene_index(value, state->layer)) { std::cerr << "Invalid --sm-layer layer: " << value << "\n"; return 1; }
                continue;
            }
            std::stringstream list(value);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                float weight = 1.0f;
                const auto bone = entry.substr(0, colon);
                if (bone.empty() || (colon != std::string::npos && !parse_signed_finite_float(entry.substr(colon + 1), weight)) ||
                    weight < 0.0f || weight > 1.0f) {
                    std::cerr << "Invalid --sm-mask entry: " << entry << " (expected BONE or BONE:WEIGHT, weight 0 to 1)\n"; return 1;
                }
                state->mask.push_back({bone, weight});
            }
            if (state->mask.empty()) { std::cerr << "--sm-mask needs at least one bone\n"; return 1; }
        }
        else if (a == "--sm-transition") {
            if (i + 4 >= argc) { std::cerr << "--sm-transition requires FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS\n"; return 1; }
            dtglb::app::StateMachineTransitionOption transition;
            int from = 0, to = 0;
            if (!parse_scene_index(argv[++i], from)) {
                std::cerr << "Invalid --sm-transition source state index: " << argv[i] << "\n"; return 1;
            }
            if (!parse_scene_index(argv[++i], to)) {
                std::cerr << "Invalid --sm-transition target state index: " << argv[i] << "\n"; return 1;
            }
            transition.from_state = static_cast<std::size_t>(from);
            transition.to_state = static_cast<std::size_t>(to);
            transition.event_name = argv[++i];
            if (!parse_finite_float(argv[++i], transition.blend_seconds, true)) {
                std::cerr << "Invalid --sm-transition blend seconds: " << argv[i] << "\n"; return 1;
            }
            opt.state_machine_transitions.push_back(std::move(transition));
        }
        else if (a == "--sm-declare-events") {
            if (i + 1 >= argc) { std::cerr << "--sm-declare-events requires a comma-separated list\n"; return 1; }
            const std::string list = argv[++i];
            std::size_t start = 0;
            while (start <= list.size()) {
                const auto comma = list.find(',', start);
                const auto name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!name.empty()) opt.state_machine_declared_events.push_back(name);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (a == "--sm-variable") {
            if (i + 4 >= argc) { std::cerr << "--sm-variable requires NAME INITIAL MIN MAX\n"; return 1; }
            dtglb::app::StateMachineVariableOption variable;
            variable.name = argv[++i];
            if (!parse_signed_finite_float(argv[++i], variable.initial_value) ||
                !parse_signed_finite_float(argv[++i], variable.minimum) ||
                !parse_signed_finite_float(argv[++i], variable.maximum)) {
                std::cerr << "Invalid --sm-variable value or bound\n"; return 1;
            }
            opt.state_machine_variables.push_back(std::move(variable));
        }
        else if (a == "--sm-range-transition") {
            if (i + 9 >= argc) {
                std::cerr << "--sm-range-transition requires FROM TO EVENT BLEND VARIABLE LOWER UPPER LOWER_EDGE UPPER_EDGE\n";
                return 1;
            }
            dtglb::app::StateMachineTransitionOption transition;
            int from = 0, to = 0, variable = 0;
            if (!parse_scene_index(argv[++i], from) || !parse_scene_index(argv[++i], to)) {
                std::cerr << "Invalid --sm-range-transition state index\n"; return 1;
            }
            transition.from_state = static_cast<std::size_t>(from);
            transition.to_state = static_cast<std::size_t>(to);
            transition.event_name = argv[++i];
            if (!parse_finite_float(argv[++i], transition.blend_seconds, true) ||
                !parse_scene_index(argv[++i], variable) ||
                !parse_signed_finite_float(argv[++i], transition.lower) ||
                !parse_signed_finite_float(argv[++i], transition.upper)) {
                std::cerr << "Invalid --sm-range-transition blend, variable, or bounds\n"; return 1;
            }
            transition.variable_index = static_cast<std::size_t>(variable);
            const std::string lower_edge = argv[++i], upper_edge = argv[++i];
            if ((lower_edge != "inclusive" && lower_edge != "exclusive") ||
                (upper_edge != "inclusive" && upper_edge != "exclusive")) {
                std::cerr << "Range edges must be inclusive or exclusive\n"; return 1;
            }
            transition.lower_exclusive = lower_edge == "exclusive";
            transition.upper_exclusive = upper_edge == "exclusive";
            opt.state_machine_transitions.push_back(std::move(transition));
        }
        else { std::cerr << "Unknown option: " << a << "\n"; return 1; }
    }
    if (opt.loop_clip_index && opt.once_clip_index) {
        std::cerr << "--loop-clip and --once-clip are mutually exclusive\n";
        return 1;
    }
    if ((!opt.state_machine_states.empty() || !opt.state_machine_variables.empty() ||
         !opt.state_machine_transitions.empty()) &&
        (opt.loop_clip_index || opt.once_clip_index)) {
        std::cerr << "--sm-state/--sm-transition cannot be combined with --loop-clip or --once-clip\n";
        return 1;
    }
    if (inspect_mode) return dtglb::app::inspect_source(opt.input, opt.scene_index);
    if (no_physics_requested) {
        opt.physics = false;
        opt.fitted_physics.reset();
    } else if (physics_modifier_set) {
        // CLI modifiers are an overlay on a settings-file actor. Keep the
        // explicit shape requirement for CLI-only invocations.
        bool settings_shape_available = false;
        if (!opt.settings_path.empty()) {
            dtglb::app::AssetSettings settings;
            std::string error;
            if (!dtglb::app::load_asset_settings(opt.settings_path, settings, error)) {
                std::cerr << "settings failed: " << error << "\n";
                return 2;
            }
            if (settings.fitted_physics) {
                settings_shape_available = true;
                auto merged = *settings.fitted_physics;
                if (physics_shape_set) merged.shape = opt.fitted_physics->shape;
                if (physics_actor_set) merged.actor_template = opt.fitted_physics->actor_template;
                if (physics_mass_set) merged.mass = opt.fitted_physics->mass;
                if (physics_material_set) merged.material = opt.fitted_physics->material;
                opt.fitted_physics = merged;
            }
        }
        if (!physics_shape_set && !settings_shape_available) {
            std::cerr << "A physics shape must be provided by --physics-shape or the settings file when using physics actor, mass, or material controls\n";
            return 1;
        }
    }
    if (opt.reference_bones_file.has_value() != opt.skeleton_resource.has_value()) {
        std::cerr << "--reference-bones and --skeleton-resource must be supplied together\n";
        return 1;
    }
    return dtglb::app::compile(opt);
}
