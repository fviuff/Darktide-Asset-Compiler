#include "app/compiler.h"
#include "app/asset_settings.h"
#include "app/source_inspection.h"
#include "validation/unit_validator.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/particles/particles_resource.h"
#include "third_party_licenses.h"
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
        "--in-place removes the horizontal root travel from every clip so walk/run cycles loop on the spot;\n"
        "build.log notes each clip's speed for moving the unit from Lua.\n"
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
