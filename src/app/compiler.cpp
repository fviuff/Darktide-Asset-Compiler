#include "app/compiler.h"
#include "compiler/source_queries.h"
#include "compiler/material_builder.h"
#include "compiler/resource_builder.h"
#include "compiler/compilation_plan.h"
#include "compiler/resource_output.h"
#include "app/asset_settings.h"
#include "compiler/compilation_context.h"
#include "gltf/glb_loader.h"
#include "stingray/murmur_hash.h"
#include "stingray/unit/unit_v115.h"
#include "stingray/bones/bones_writer.h"
#include "stingray/animation/animation_writer.h"
#include "stingray/material/material_v61.h"
#include "stingray/texture/texture_writer.h"
#include "processing/texture_processor.h"
#include "processing/scene_scaler.h"
#include "processing/material_canonicalizer.h"
#include "processing/attribute_canonicalizer.h"
#include "processing/topology_canonicalizer.h"
#include "processing/scene_feature_canonicalizer.h"
#include "processing/native_profile_canonicalizer.h"
#include "processing/animation_canonicalizer.h"
#include "processing/uv1_material_canonicalizer.h"
#include "validation/unit_validator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dtglb::app {
namespace {
using compiler::canonical_bone_names;
using compiler::used_skin_indices;
using compiler::used_material_indices;
using compiler::active_node_indices;
using compiler::active_node_skin_indices;
using compiler::core_material_gap;

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 32) {
                    char b[7];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}



std::string hex32(std::uint32_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return s.str();
}

std::string hex64(std::uint64_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << std::setw(16) << std::setfill('0') << v;
    return s.str();
}

std::string physics_feature_status(const ResolvedPhysicsInventory& physics, const CompileOptions& options,
                                   const Scene& scene) {
    if (options.output_kind == OutputKind::Animations) return "not_requested_output";
    if (!options.physics) return "disabled_by_user";
    if (options.fitted_physics) {
        switch (options.fitted_physics->shape) {
            case stingray::physics::PhysicsShapeType::TriangleMesh: return
                options.fitted_physics->actor_template == "dynamic"
                    ? "emitted_geometry_as_dynamic_convex" : "emitted_exact_geometry";
            case stingray::physics::PhysicsShapeType::Convex: return "emitted_convex_geometry";
            case stingray::physics::PhysicsShapeType::Sphere: return "emitted_fitted_sphere";
            case stingray::physics::PhysicsShapeType::Capsule: return "emitted_fitted_capsule";
            case stingray::physics::PhysicsShapeType::Box: return "emitted_fitted_box";
            default: return "unsupported_physics_shape";
        }
    }
    if (scene.asset_definition.body) return "emitted_authored_compound";
    if (!scene.asset_definition.node_bodies.empty()) return "emitted_authored_collections";
    if (physics.requested == 0) return "not_requested";
    if (physics.effective == 0) return "disabled_in_source";
    return "omitted_runtime_owned";
}



void write_string_array(std::ofstream& f, const std::vector<std::string>& values) {
    f << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) f << ",";
        f << "\"" << json_escape(values[i]) << "\"";
    }
    f << "]";
}

bool write_manifest(const Scene& s,
                    const CompileOptions& o,
                    const ResolvedPhysicsInventory& physics,
                    const std::filesystem::path& unit_path,
                    const std::string& status,
                    const std::vector<std::string>& gaps,
                    const std::vector<std::string>& resources = {},
                    const std::vector<std::string>& feature_status = {},
                    const std::filesystem::path& manifest_filename = "compile_manifest.json",
                    const std::vector<std::string>& applied_approximations = {},
                    const std::vector<int>& emitted_clips = {},
                    const std::vector<compiler::TextureDimensionNormalization>& texture_normalizations = {}) {
    const auto& q = s.source_features;
    std::ofstream f(o.output_dir / manifest_filename);
    f << "{\n"
      << "  \"source\": \"" << json_escape(o.input.string()) << "\",\n"
      << "  \"status\": \"" << status << "\",\n"
      << "  \"requested_output\": \"" << output_kind_name(o.output_kind) << "\",\n"
      << "  \"requested_scene\": " << (o.scene_index ? std::to_string(*o.scene_index) : "null") << ",\n"
      << "  \"requested_clip\": " << (o.animation_index ? std::to_string(*o.animation_index) : "null") << ",\n"
      << "  \"requested_material_variant\": " << (o.material_variant ? "\"" + json_escape(*o.material_variant) + "\"" : "null") << ",\n"
      << "  \"asset_scale\": " << o.asset_scale.value_or(1.0f) << ",\n"
      << "  \"asset_path\": " << (o.asset_path ? "\"" + json_escape(*o.asset_path) + "\"" : "null") << ",\n"
      << "  \"default_scene\": " << (s.default_scene >= 0 ? std::to_string(s.default_scene) : "null") << ",\n"
      << "  \"selected_scene\": " << (s.selected_scene >= 0 ? std::to_string(s.selected_scene) : "null") << ",\n"
      << "  \"allowed_approximations\": {\"fixed_morph_bake\":" << (o.allow_fixed_morph_bake ? "true" : "false")
      << ",\"animation_resampling\":" << (o.allow_animation_resampling ? "true" : "false")
      << ",\"native_animation_interpolation\":" << (o.allow_native_animation_interpolation ? "true" : "false") << "},\n"
      << std::setprecision(17) << "  \"animation_fit\": {\"translation_tolerance\":" << o.animation_fit.translation_tolerance
      << ",\"scale_tolerance\":" << o.animation_fit.scale_tolerance
      << ",\"rotation_tolerance_radians\":" << o.animation_fit.rotation_tolerance_radians
      << ",\"max_controls\":" << o.animation_fit.max_controls
      << ",\"max_refinements\":" << o.animation_fit.max_refinements
      << ",\"max_evaluations\":" << o.animation_fit.max_evaluations << "},\n" << std::setprecision(6)
      << "  \"applied_approximations\": ";
    write_string_array(f, applied_approximations);
    f << ",\n  \"emitted_clips\": [";
    for (std::size_t i = 0; i < emitted_clips.size(); ++i) {
        if (i) f << ',';
        f << emitted_clips[i];
    }
    f << ']';
    f << ",\n  \"texture_dimension_normalizations\":[";
    for (std::size_t i = 0; i < texture_normalizations.size(); ++i) {
        if (i) f << ',';
        const auto& normalization = texture_normalizations[i];
        f << "{\"resource\":{\"type\":\"" << json_escape(normalization.resource.type)
          << "\",\"name\":\"" << json_escape(normalization.resource.name)
          << "\"},\"profile\":\"" << json_escape(normalization.profile)
          << "\",\"source_width\":" << normalization.source_width
          << ",\"source_height\":" << normalization.source_height
          << ",\"stored_width\":" << normalization.stored_width
          << ",\"stored_height\":" << normalization.stored_height << '}';
    }
    f << ']';
    f << ",\n  \"unit\": ";
    if (unit_path.empty()) f << "null";
    else f << "\"" << json_escape(unit_path.filename().string()) << "\"";
    f << ",\n  \"unit_version\": 115,\n"
      << "  \"mesh_geometry_version\": 1,\n"
      << "  \"coordinate_conversion\": \"uniform asset scale, then glTF Y-up RH -> Stingray Z-up RH; "
      << (s.asset_definition.node_bodies.empty()
          ? "static renderer geometry remains world-baked while active source hierarchy is preserved as reference SceneGraphDT nodes"
          : "body-owned rigid render and collider geometry use body-local coordinates; skinned render geometry retains its skin-space profile, bound to authored SceneGraphDT nodes")
      << "\",\n"
      << "  \"source_features\": {\n"
      << "    \"scenes\": " << q.scene_count << ",\n"
      << "    \"nodes\": " << q.node_count << ",\n"
      << "    \"roots\": " << q.root_count << ",\n"
      << "    \"parent_edges\": " << q.parent_edge_count << ",\n"
      << "    \"meshes\": " << q.mesh_count << ",\n"
      << "    \"source_primitives\": " << q.source_primitive_count << ",\n"
      << "    \"active_primitive_instances\": " << q.active_source_primitive_count << ",\n"
      << "    \"decoded_source_primitive_instances\": " << q.decoded_source_primitive_count << ",\n"
      << "    \"decoded_primitive_instances\": " << q.decoded_primitive_count << ",\n"
      << "    \"approximated_primitives\": " << q.approximated_primitive_count << ",\n"
      << "    \"custom_attributes\": " << q.custom_attribute_count << ",\n"
      << "    \"texcoord_attributes\": " << q.texcoord_attribute_count << ",\n"
      << "    \"color_attributes\": " << q.color_attribute_count << ",\n"
      << "    \"joint_attributes\": " << q.joint_attribute_count << ",\n"
      << "    \"weight_attributes\": " << q.weight_attribute_count << ",\n"
      << "    \"max_texcoord_set\": " << q.max_texcoord_set << ",\n"
      << "    \"morph_targets\": " << q.morph_target_count << ",\n"
      << "    \"active_morph_targets\": " << q.active_morph_target_count << ",\n"
      << "    \"fixed_initial_morph_bakes\": " << q.fixed_initial_morph_bake_count << ",\n"
      << "    \"fixed_initial_morph_bake_succeeded\": " << (q.fixed_initial_morph_bake_succeeded ? "true" : "false") << ",\n"
      << "    \"materials\": " << q.material_count << ",\n"
      << "    \"material_texture_bindings\": " << q.material_texture_binding_count << ",\n"
      << "    \"pbr_specular_glossiness_materials\": " << q.pbr_specular_glossiness_material_count << ",\n"
      << "    \"ior_materials\": " << q.ior_material_count << ",\n"
      << "    \"material_variants\": " << q.material_variant_count << ",\n"
      << "    \"material_variant_mappings\": " << q.material_variant_mapping_count << ",\n"
      << "    \"active_material_variant_mappings\": " << q.active_material_variant_mapping_count << ",\n"
      << "    \"images\": " << q.image_count << ",\n"
      << "    \"textures\": " << q.texture_count << ",\n"
      << "    \"skins\": " << q.skin_count << ",\n"
      << "    \"joints\": " << q.joint_count << ",\n"
      << "    \"animations\": " << q.animation_count << ",\n"
      << "    \"animation_channels\": " << q.animation_channel_count << ",\n"
      << "    \"active_animations\": " << q.active_animation_count << ",\n"
      << "    \"active_animation_channels\": " << q.active_animation_channel_count << ",\n"
      << "    \"cameras\": " << q.camera_count << ",\n"
      << "    \"lights\": " << q.light_count << ",\n"
      << "    \"meshopt_buffer_views\": " << q.meshopt_buffer_view_count << ",\n"
      << "    \"draco_primitives\": " << q.draco_primitive_count << ",\n"
      << "    \"gpu_instancing_nodes\": " << q.gpu_instancing_node_count << ",\n"
      << "    \"physics_requested\": " << physics.requested << ",\n"
      << "    \"physics_effective\": " << physics.effective << ",\n"
      << "    \"physics_suppressed\": " << physics.suppressed << ",\n"
      << "    \"physics_locations\": ";
    write_string_array(f, physics.locations);
    f << ",\n    \"approximation_locations\": ";
    write_string_array(f, q.approximation_locations);
    f << "\n  },\n";

    std::size_t decoded_animation_tracks = 0;
    for (const auto& animation : s.animations) decoded_animation_tracks += animation.tracks.size();
    f << "  \"decoded_ir\": {\n"
      << "    \"scenes\": " << s.scenes.size() << ",\n"
      << "    \"nodes\": " << s.nodes.size() << ",\n"
      << "    \"primitives\": " << s.primitives.size() << ",\n"
      << "    \"materials\": " << s.materials.size() << ",\n"
      << "    \"images\": " << s.images.size() << ",\n"
      << "    \"textures\": " << s.textures.size() << ",\n"
      << "    \"skins\": " << s.skins.size() << ",\n"
      << "    \"animations\": " << s.animations.size() << ",\n"
      << "    \"animation_tracks\": " << decoded_animation_tracks << "\n"
      << "  },\n"
      << "  \"primitive_count\": " << s.primitives.size() << ",\n"
      << "  \"materials\": [\n";

    for (std::size_t i = 0; i < s.materials.size(); ++i) {
        const auto& m = s.materials[i];
        f << "    {\"name\":\"" << json_escape(m.name) << "\",\"id32\":\"" << hex32(stingray::id32_from_id64(m.name))
          << "\",\"baseColor\":[" << m.base_color[0] << "," << m.base_color[1] << "," << m.base_color[2] << "," << m.base_color[3]
          << "],\"metallic\":" << m.metallic << ",\"roughness\":" << m.roughness
          << ",\"doubleSided\":" << (m.double_sided ? "true" : "false") << "}"
          << (i + 1 < s.materials.size() ? "," : "") << "\n";
    }

    f << "  ],\n  \"material_resource_override\": ";
    if (o.material_override.empty()) f << "null";
    else f << "{\"name\":\"" << json_escape(o.material_override) << "\",\"id64\":\"" << hex64(stingray::id64(o.material_override)) << "\"}";

    f << ",\n  \"physics_output\": ";
    if (!o.physics || (!o.fitted_physics && !s.asset_definition.body && s.asset_definition.node_bodies.empty()) || o.output_kind == OutputKind::Animations) {
        f << "null";
    } else if (!o.fitted_physics && !s.asset_definition.node_bodies.empty()) {
        f << "{\"shape\":\"authored_collections\",\"frame\":\"unit_nodes\",\"body_count\":"
          << s.asset_definition.node_bodies.size() << ",\"joint_count\":" << s.asset_definition.joints.size()
          << ",\"collider_count\":" << s.asset_definition.colliders.size()
          << ",\"verification\":\"offline_structural\"}";
    } else if (!o.fitted_physics && s.asset_definition.body) {
        const auto& body = *s.asset_definition.body;
        f << "{\"shape\":\"authored_compound\",\"body\":\"" << json_escape(body.id)
          << "\",\"frame\":\"unit_root\",\"actor\":\"" << json_escape(body.actor)
          << "\",\"mass\":" << body.mass << ",\"material\":\"" << json_escape(body.material)
          << "\",\"collider_count\":" << s.asset_definition.colliders.size()
          << ",\"verification\":\"offline_structural\"}";
    } else {
        const auto& p = *o.fitted_physics;
        const char* shape_name = "unknown";
        switch (p.shape) {
            case stingray::physics::PhysicsShapeType::TriangleMesh: shape_name = "geometry"; break;
            case stingray::physics::PhysicsShapeType::Convex: shape_name = "convex"; break;
            case stingray::physics::PhysicsShapeType::Sphere: shape_name = "sphere"; break;
            case stingray::physics::PhysicsShapeType::Capsule: shape_name = "capsule"; break;
            case stingray::physics::PhysicsShapeType::Box: shape_name = "box"; break;
            default: break;
        }
        f << "{\"shape\":\""
          << shape_name
          << "\",\"actor\":\"" << json_escape(p.actor_template)
          << "\",\"mass\":" << p.mass
          << ",\"material\":\"" << json_escape(p.material)
          << "\",\"shape_template\":\"" << json_escape(p.shape_template)
          << "\",\"verification\":\"offline_structural\"}";
    }

    f << ",\n  \"images\": [";
    for (std::size_t i = 0; i < s.images.size(); ++i) {
        if (i) f << ",";
        f << "\"" << json_escape(s.images[i].name) << "\"";
    }
    f << "],\n  \"skins\": " << s.skins.size()
      << ",\n  \"animations\": " << s.animations.size()
      << ",\n  \"extensions_used\": ";
    write_string_array(f, s.extensions_used);
    f << ",\n  \"active_extensions_used\": ";
    write_string_array(f, s.active_extensions_used);
    f << ",\n  \"extensions_required\": ";
    write_string_array(f, s.extensions_required);
    f << ",\n  \"compiler_gaps\": ";
    write_string_array(f, gaps);
    f << ",\n  \"resources\": ";
    write_string_array(f, resources);
    f << ",\n  \"feature_status\": {";
    for (std::size_t i = 0; i + 1 < feature_status.size(); i += 2) {
        if (i) f << ",";
        const bool excluded = (o.output_kind == OutputKind::Model && feature_status[i] == "animation") ||
            (o.output_kind == OutputKind::Animations && (feature_status[i] == "unit" || feature_status[i] == "materials"));
        f << "\"" << json_escape(feature_status[i]) << "\":\""
          << (excluded ? "not_requested_output" : json_escape(feature_status[i + 1])) << "\"";
    }
    f << "},\n  \"material_semantics\": ";
    if (o.output_kind == OutputKind::Animations) f << "\"not_requested_output\"";
    else if (!o.material_override.empty()) f << "\"explicit_material_override_approximation\"";
    else if (!o.auto_materials) f << "\"explicitly_disabled_by_user\"";
    else {
        bool basic = false, emissive = false, mask = false;
        for (const auto index : used_material_indices(s)) {
            const auto& material = s.materials[index];
            const bool emits = material.emissive_strength > 0.0f &&
                std::any_of(material.emissive.begin(), material.emissive.end(), [](float value) { return value > 0.0f; });
            if (material.alpha_mode == "MASK") mask = true;
            else if (emits) emissive = true;
            else basic = true;
        }
        if (std::any_of(s.primitives.begin(), s.primitives.end(), [](const Primitive& primitive) {
                return primitive.material < 0;
            })) basic = true;
        const int families = static_cast<int>(basic) + static_cast<int>(emissive) + static_cast<int>(mask);
        if (!families) f << "\"no_used_source_materials\"";
        else if (families > 1) f << "\"owned_native_material_families\"";
        else if (mask) f << "\"owned_mask_pbr\"";
        else if (emissive) f << "\"owned_pbr_emissive\"";
        else f << "\"owned_substance_basic_core_pbr\"";
    }
    f << ",\n  \"notes\": ";
    write_string_array(f, s.notes);
    f << "\n}\n";
    return static_cast<bool>(f);
}

bool create_exclusive_sibling(const std::filesystem::path& output, const std::string& label,
                              std::filesystem::path& result, std::string& error) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt != 32; ++attempt) {
        std::ostringstream suffix;
        suffix << std::hex << (stamp & 0xffffffffffffull) << "-"
               << sequence.fetch_add(1) << "-" << attempt;
        // Keep the temporary basename short.  Compiler output paths are often
        // already near legacy Win32 MAX_PATH, and embedding the full output
        // basename twice made otherwise valid backup renames fail.
        result = output.parent_path() / (".dtglb-" + label + "-" + suffix.str());
        std::error_code ec;
        if (std::filesystem::create_directory(result, ec)) return true;
        if (!ec) continue;
    }
    error = "cannot create exclusive " + label + " directory beside " + output.string();
    return false;
}

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

std::filesystem::path preferred_path(std::filesystem::path path) {
#ifdef _WIN32
    path.make_preferred();
#endif
    return path;
}

bool validate_publish_parent(const std::filesystem::path& output,
                             const std::filesystem::path& parent,
                             std::string& error) {
    std::error_code ec;
    const auto absolute_output = std::filesystem::absolute(output, ec).lexically_normal();
    if (ec) { error = "cannot resolve output directory: " + ec.message(); return false; }
    const auto absolute_parent = std::filesystem::absolute(parent, ec).lexically_normal();
    if (ec) { error = "cannot resolve publish directory: " + ec.message(); return false; }
    const auto relative = absolute_parent.lexically_relative(absolute_output);
    if (relative.empty() && absolute_parent != absolute_output) {
        error = "publish target is outside the selected output directory";
        return false;
    }
    for (const auto& component : relative) {
        if (component == "..") { error = "publish target escapes the selected output directory"; return false; }
    }

    auto current = absolute_output;
    const auto check_component = [&](const std::filesystem::path& component) {
        const auto status = std::filesystem::symlink_status(component, ec);
        if (ec == std::errc::no_such_file_or_directory) { ec.clear(); return true; }
        if (ec) { error = "cannot inspect publish path " + component.string() + ": " + ec.message(); return false; }
        if (status.type() == std::filesystem::file_type::not_found) return true;
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            error = "publish path contains a symlink/reparse point or non-directory: " + component.string();
            return false;
        }
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(component.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            error = "publish path contains a Windows reparse point: " + component.string();
            return false;
        }
#endif
        return true;
    };
    if (!check_component(current)) return false;
    for (const auto& component : relative) {
        if (component == ".") continue;
        current /= component;
        if (!check_component(current)) return false;
    }

    // The lexical containment check and per-component reparse checks above
    // establish the target boundary, including still-missing directories.
    // MSVC's weakly_canonical can return access_denied for a missing nested
    // descendant (for example output/data/am), so do not canonicalize that
    // descendant merely to repeat the same containment check.
    const auto canonical_output = std::filesystem::weakly_canonical(absolute_output, ec);
    if (ec || canonical_output.empty()) {
        error = "cannot canonicalize output directory: " + ec.message();
        return false;
    }
    return true;
}

bool publish_staged(const std::filesystem::path& staging, const std::filesystem::path& output,
                    std::string& error) {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups;
    std::vector<std::filesystem::path> published;
    std::filesystem::path backup_root;
    std::error_code ec;
    if (!create_exclusive_sibling(output, "rollback", backup_root, error)) return false;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(staging, ec))
        if (!ec && entry.is_regular_file()) files.push_back(entry.path());
    if (ec) { error = "cannot enumerate staged output: " + ec.message(); }
    std::sort(files.begin(), files.end(), [&](const auto& a, const auto& b) {
        const bool am = a.filename() == "compile_manifest.json";
        const bool bm = b.filename() == "compile_manifest.json";
        return am == bm ? a < b : !am;
    });
    std::set<std::string> staged_paths;
    for (const auto& staged_file : files) {
        const auto relative = std::filesystem::relative(staged_file, staging, ec);
        if (ec || relative.is_absolute() || relative.empty()) { error = "staged output path is not a safe relative path"; break; }
        for (const auto& component : relative) if (component == "..") error = "staged output path escapes its staging directory";
        if (!error.empty()) break;
        staged_paths.insert(relative.generic_string());
    }
    if (error.empty()) {
        std::vector<std::string> prior_artifacts;
        std::string manifest_error;
        const auto prior_manifest = output / "build.json";
        const auto prior_manifest_status = std::filesystem::symlink_status(prior_manifest, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) { error = "cannot inspect prior build manifest: " + ec.message(); }
        else if (prior_manifest_status.type() != std::filesystem::file_type::not_found &&
                 (std::filesystem::is_symlink(prior_manifest_status) ||
                  prior_manifest_status.type() != std::filesystem::file_type::regular)) {
            error = "prior build manifest is not a regular file";
        }
#ifdef _WIN32
        else if (prior_manifest_status.type() != std::filesystem::file_type::not_found &&
                 [&] {
                     const DWORD attributes = GetFileAttributesW(prior_manifest.c_str());
                     return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                 }()) {
            error = "prior build manifest is a Windows reparse point";
        }
#endif
        else if (prior_manifest_status.type() != std::filesystem::file_type::not_found &&
                 !compiler::read_build_artifact_paths(output, prior_artifacts, manifest_error)) {
            error = "cannot validate prior build manifest: " + (manifest_error.empty() ? std::string("invalid manifest") : manifest_error);
        } else if (prior_manifest_status.type() == std::filesystem::file_type::not_found) {
            // A first publication has no ownership manifest to consult.
        } else {
            std::vector<std::filesystem::path> stale;
            for (const auto& path : prior_artifacts) {
                if (!compiler::portable_payload_path(path)) {
                    error = "prior build manifest contains unsafe artifact path: " + path;
                    break;
                }
                const auto relative = std::filesystem::path(path);
                if (relative.is_absolute() || staged_paths.count(relative.generic_string())) continue;
                stale.push_back(relative);
            }
            if (error.empty()) {
                for (const auto& relative : stale) {
                    const auto target = preferred_path(output / relative);
                    if (!validate_publish_parent(output, target.parent_path(), manifest_error)) { error = "cannot validate stale output " + target.string() + ": " + manifest_error; break; }
                    const auto status = std::filesystem::symlink_status(target, ec);
                    if (ec && ec != std::errc::no_such_file_or_directory) { error = "cannot inspect stale output " + target.string() + ": " + ec.message(); break; }
                    ec.clear();
                    if (status.type() == std::filesystem::file_type::not_found) continue;
                    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
                        error = "prior build manifest points to a non-regular artifact: " + target.string();
                        break;
                    }
                }
            }
            if (error.empty()) {
                for (const auto& relative : stale) {
                    const auto target = preferred_path(output / relative);
                    const bool target_exists = std::filesystem::exists(target, ec);
                    if (ec) { error = "cannot inspect stale output " + target.string() + ": " + ec.message(); break; }
                    if (!target_exists) continue;
                    const auto backup = preferred_path(backup_root / relative);
                    std::filesystem::create_directories(backup.parent_path(), ec);
                    if (ec || !validate_publish_parent(output, target.parent_path(), manifest_error)) { error = "cannot prepare stale output cleanup"; break; }
                    std::filesystem::rename(target, backup, ec);
                    if (ec) { error = "cannot preserve stale output " + target.string() + ": " + ec.message(); break; }
                    backups.emplace_back(target, backup);
                }
            }
        }
    }
    for (const auto& staged_file : files) {
        if (!error.empty()) break;
        const auto relative = std::filesystem::relative(staged_file, staging, ec);
        if (ec) { error = "cannot enumerate staged output: " + ec.message(); break; }
        if (relative.is_absolute() || relative.empty()) { error = "staged output path is not a safe relative path"; break; }
        for (const auto& component : relative) {
            if (component == "..") { error = "staged output path escapes its staging directory"; break; }
        }
        if (!error.empty()) break;
        const auto target = preferred_path(output / relative);
        if (!validate_publish_parent(output, target.parent_path(), error)) break;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) { error = "cannot create output directory for " + target.string() + ": " + ec.message(); break; }
        if (!validate_publish_parent(output, target.parent_path(), error)) break;
        const auto target_status = std::filesystem::symlink_status(target, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) { error = "cannot inspect existing output " + target.string() + ": " + ec.message(); break; }
        ec.clear();
        const bool target_exists = target_status.type() != std::filesystem::file_type::not_found;
        if (target_exists && (std::filesystem::is_symlink(target_status) || !std::filesystem::is_regular_file(target_status))) {
            error = "existing output is not a regular file: " + target.string();
            break;
        }
#ifdef _WIN32
        if (target_exists) {
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                error = "existing output cannot be replaced as a regular file: " + target.string();
                break;
            }
        }
#endif
        if (target_exists) {
            const auto backup = preferred_path(backup_root / relative);
            std::filesystem::create_directories(backup.parent_path(), ec);
            std::filesystem::rename(target, backup, ec);
            if (ec) {
                error = "cannot preserve existing output " + target.string() + ": " + (ec ? ec.message() : "rename failed");
                break;
            }
            backups.emplace_back(target, backup);
        }
        std::filesystem::rename(staged_file, target, ec);
        if (ec) { error = "cannot publish " + target.string() + ": " + ec.message(); break; }
        published.push_back(target);
    }
    if (!error.empty()) {
        std::string rollback_error;
        for (const auto& path : published) {
            std::filesystem::remove(path, ec);
            if (ec && rollback_error.empty()) rollback_error = "cannot remove published " + path.string() + ": " + ec.message();
        }
        for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
            std::filesystem::create_directories(it->first.parent_path(), ec);
            std::filesystem::rename(it->second, it->first, ec);
            if (ec && rollback_error.empty()) rollback_error = "cannot restore " + it->first.string() + ": " + ec.message();
        }
        if (rollback_error.empty()) std::filesystem::remove_all(backup_root, ec);
        if (!rollback_error.empty()) error += "; rollback incomplete (recovery retained at " + backup_root.string() + "): " + rollback_error;
        return false;
    }
    std::filesystem::remove_all(backup_root, ec);
    if (ec) {
        std::cerr << "warning: output published successfully but rollback directory could not be removed: "
                  << backup_root.string() << ": " << ec.message() << "\n";
    }
    return true;
}

void cleanup_staging(const std::filesystem::path& staging) {
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    if (ec) std::cerr << "warning: cannot clean staging directory " << staging.string() << ": " << ec.message() << "\n";
}

} // namespace

int compile(const CompileOptions& requested_options) {
    CompileOptions options = requested_options;
    const auto display_output_dir = options.output_dir;
#ifdef _WIN32
    // Compiler outputs are written through both Win32-backed filesystem calls
    // and standard streams. Keep every output path in the extended namespace
    // so those APIs can handle deep Blender output directories consistently.
    options.output_dir = windows_long_path(options.output_dir);
#endif
    Scene scene;
    std::string error;
    AssetSettings settings;
    if (!load_asset_settings(options.settings_path, settings, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    options.allow_fixed_morph_bake = options.allow_fixed_morph_bake || settings.allow_fixed_morph_bake;
    options.allow_animation_resampling = options.allow_animation_resampling || settings.allow_animation_resampling;
    options.allow_native_animation_interpolation = options.allow_native_animation_interpolation || settings.allow_native_animation_interpolation;
    if (settings.animation_fit) options.animation_fit = *settings.animation_fit;
    const auto& fit_overrides = options.animation_fit_overrides;
    if (fit_overrides.translation_tolerance) options.animation_fit.translation_tolerance = *fit_overrides.translation_tolerance;
    if (fit_overrides.scale_tolerance) options.animation_fit.scale_tolerance = *fit_overrides.scale_tolerance;
    if (fit_overrides.rotation_tolerance_radians) options.animation_fit.rotation_tolerance_radians = *fit_overrides.rotation_tolerance_radians;
    if (fit_overrides.max_controls) options.animation_fit.max_controls = *fit_overrides.max_controls;
    if (fit_overrides.max_refinements) options.animation_fit.max_refinements = *fit_overrides.max_refinements;
    if (fit_overrides.max_evaluations) options.animation_fit.max_evaluations = *fit_overrides.max_evaluations;
    if (!options.asset_scale) options.asset_scale = settings.asset_scale;
    if (!options.fitted_physics) options.fitted_physics = settings.fitted_physics;
    if (!options.physics) options.fitted_physics.reset();
    if (!options.scene_index) options.scene_index = settings.scene_index;
    if (!options.animation_index) options.animation_index = settings.animation_index;
    if (!options.asset_path) options.asset_path = settings.asset_path;
    if (!options.material_variant) options.material_variant = settings.material_variant;
    compiler::CompilationContext context;
    if (!compiler::make_compilation_context(options.input.stem().string(), context, error, options.asset_path)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    for (const auto& key : settings.unknown_keys)
        std::cerr << "warning: unsupported asset setting was not applied: " << key << "\n";
    gltf::LoadOptions load_options;
    load_options.include_unused_meshes = false;
    load_options.scene_index = options.scene_index;
    load_options.animation_index = options.animation_index;
    load_options.material_variant = options.material_variant;
#ifdef _WIN32
    const auto input_path = windows_long_path(options.input);
#else
    const auto& input_path = options.input;
#endif
    if (!gltf::load_scene(input_path, scene, error, load_options)) {
#ifdef _WIN32
        const auto extended_input = input_path.string();
        const auto ordinary_input = std::filesystem::absolute(options.input).lexically_normal().string();
        for (auto at = error.find(extended_input); at != std::string::npos;
             at = error.find(extended_input, at + ordinary_input.size()))
            error.replace(at, extended_input.size(), ordinary_input);
#endif
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }

    const float effective_scale = options.asset_scale.value_or(1.0f);
    options.asset_scale = effective_scale;
    if (!processing::scale_scene(scene, effective_scale, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }

    processing::TopologyCanonicalizationReport topology_report;
    if (!processing::canonicalize_non_triangle_topology(scene, topology_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }

    processing::SceneFeatureCanonicalizationReport scene_feature_report;
    if (!processing::canonicalize_scene_features(scene, scene_feature_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }

    const bool automatic_materials = options.auto_materials && options.material_override.empty();
    if (automatic_materials)
        processing::canonicalize_generated_uv1_materials(scene);

    processing::MaterialCanonicalizationReport material_report;
    if (automatic_materials &&
        !processing::canonicalize_material_workflows(scene, material_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    processing::VertexColorCanonicalizationReport vertex_color_report;
    if (!processing::canonicalize_vertex_colors(scene, automatic_materials, vertex_color_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    if (automatic_materials &&
        !processing::finalize_material_profiles(scene, material_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    processing::AttributeCanonicalizationReport attribute_report;
    if (!processing::canonicalize_unused_attributes(scene, attribute_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    processing::NativeProfileCanonicalizationReport native_profile_report;
    if (!processing::canonicalize_native_profile(scene, native_profile_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    processing::AnimationCanonicalizationReport animation_report;
    if (!processing::canonicalize_animations(scene, animation_report, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    if (ec) {
        std::cerr << "compile failed: cannot create output directory: " << ec.message() << "\n";
        return 2;
    }

    const auto physics = resolve_physics_inventory(scene.source_features, settings, options.physics);
    auto plan = compiler::plan_compilation(scene, options, physics);
    plan.approximations.insert(plan.approximations.end(), material_report.approximations.begin(),
                               material_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), vertex_color_report.approximations.begin(),
                               vertex_color_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), attribute_report.approximations.begin(),
                               attribute_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), topology_report.approximations.begin(),
                               topology_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), scene_feature_report.approximations.begin(),
                               scene_feature_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), native_profile_report.approximations.begin(),
                               native_profile_report.approximations.end());
    plan.approximations.insert(plan.approximations.end(), animation_report.approximations.begin(),
                               animation_report.approximations.end());
    const auto& gaps = plan.gaps;
    if (!gaps.empty()) {
        std::error_code manifest_ec;
        const auto published_manifest = options.output_dir / "compile_manifest.json";
        const bool preserve_published_manifest = std::filesystem::exists(published_manifest, manifest_ec);
        if (manifest_ec) {
            std::cerr << "compile failed: cannot inspect existing manifest: " << manifest_ec.message() << "\n";
            return 2;
        }
        const std::filesystem::path diagnostic_name = preserve_published_manifest
            ? "compile_diagnostic.json" : "compile_manifest.json";
        std::vector<std::string> gap_features{"unit", "not_emitted", "bones", "not_emitted", "animation", "not_emitted", "materials", options.material_override.empty() ? "not_emitted" : "explicit_override_approximation", "physics", physics_feature_status(physics, options, scene)};
        if (vertex_color_report.changed) gap_features.insert(gap_features.end(), {"texture_domain_canonicalization", vertex_color_report.atlas_capacity_fallback ? "atlas_capacity_fallback" : (vertex_color_report.atlas_applied ? "canonicalized_atlas" : (vertex_color_report.direct_specgloss_applied ? "specgloss_full_resolution" : (vertex_color_report.constant_applied ? "canonicalized_constant" : (vertex_color_report.explicit_drop ? "explicit_drop" : "dropped_unconsumed"))))});
        if (material_report.changed) gap_features.insert(gap_features.end(), {"material_canonicalization", material_report.lossy ? "lossy" : "exact"});
        if (attribute_report.changed) gap_features.insert(gap_features.end(), {"attribute_canonicalization", "lossy"});
        if (topology_report.changed) gap_features.insert(gap_features.end(), {"topology_canonicalization", "lossy"});
        if (scene_feature_report.changed) gap_features.insert(gap_features.end(), {"scene_feature_canonicalization", scene_feature_report.lossy ? "lossy" : "exact"});
        if (native_profile_report.changed) gap_features.insert(gap_features.end(), {"native_profile_canonicalization", native_profile_report.lossy ? "lossy" : "exact"});
        if (animation_report.changed) gap_features.insert(gap_features.end(), {"animation_canonicalization", animation_report.lossy ? "lossy" : "exact"});
        if (!write_manifest(scene, options, physics, {}, "compiler_gap", gaps, {}, gap_features,
                            diagnostic_name, plan.approximations))
            std::cerr << "warning: cannot write diagnostic manifest: " << (options.output_dir / diagnostic_name).string() << "\n";
        std::cerr << "compile stopped: input contains features the current compiler does not lower completely:\n";
        for (const auto& gap : gaps) std::cerr << "  - " << gap << "\n";
        std::cerr << "diagnostic manifest: " << (options.output_dir / diagnostic_name).string() << "\n";
        return 4;
    }

    std::filesystem::path staging;
    if (!create_exclusive_sibling(options.output_dir, "staging", staging, error)) {
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    const auto& resource_base = context.file_base();
    const auto unit_path = staging / (resource_base + ".unit");
    std::vector<std::string> resources;
    std::vector<std::string> feature_status;
    const bool emit_unit = plan.emit_unit;
    feature_status.insert(feature_status.end(), {"unit", emit_unit ? "pending" : "not_emitted", "bones", "not_requested", "animation", "not_requested", "materials", options.material_override.empty() ? (options.auto_materials ? "not_requested" : "disabled_by_user") : "explicit_override_approximation", "physics", physics_feature_status(physics, options, scene)});
    if (vertex_color_report.changed) feature_status.insert(feature_status.end(), {"texture_domain_canonicalization", vertex_color_report.atlas_capacity_fallback ? "atlas_capacity_fallback" : (vertex_color_report.atlas_applied ? "canonicalized_atlas" : (vertex_color_report.direct_specgloss_applied ? "specgloss_full_resolution" : (vertex_color_report.constant_applied ? "canonicalized_constant" : (vertex_color_report.explicit_drop ? "explicit_drop" : "dropped_unconsumed"))))});
    if (material_report.changed) feature_status.insert(feature_status.end(), {"material_canonicalization", material_report.lossy ? "lossy" : "exact"});
    if (attribute_report.changed) feature_status.insert(feature_status.end(), {"attribute_canonicalization", "lossy"});
    if (topology_report.changed) feature_status.insert(feature_status.end(), {"topology_canonicalization", "lossy"});
    if (scene_feature_report.changed) feature_status.insert(feature_status.end(), {"scene_feature_canonicalization", scene_feature_report.lossy ? "lossy" : "exact"});
    if (native_profile_report.changed) feature_status.insert(feature_status.end(), {"native_profile_canonicalization", native_profile_report.lossy ? "lossy" : "exact"});
    if (animation_report.changed) feature_status.insert(feature_status.end(), {"animation_canonicalization", animation_report.lossy ? "lossy" : "exact"});
    if (emit_unit && scene.source_features.active_morph_target_count != 0) {
        feature_status.insert(feature_status.end(), {"morph_targets", "approximated_fixed_initial_bake"});
    }

    compiler::ResourceGraph graph;
    compiler::SerializedResources serialized;
    compiler::BuildMetadata metadata;
    metadata.requested_output = output_kind_name(options.output_kind);
    metadata.applied_approximations = plan.approximations;
    metadata.selected_scene = scene.selected_scene >= 0 ? std::optional<int>(scene.selected_scene) : std::nullopt;
    metadata.requested_clip = options.animation_index;
    metadata.target_profile = context.target_profile();
    for (const auto index : plan.animation_indices) metadata.emitted_clips.push_back(scene.animations[index].source_index);
    if (!compiler::build_glb_resources(scene, options, context, plan, graph, feature_status, error)) {
        cleanup_staging(staging);
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    bool animation_fit_best_effort = false;
    for (const auto& node : graph.owned) {
        const auto* animation = std::get_if<stingray::animation::BuiltAnimation>(&node.resource);
        if (!animation) continue;
        animation_fit_best_effort = animation_fit_best_effort || animation->best_effort ||
            std::any_of(animation->fits.begin(), animation->fits.end(),
                [](const auto& fit) { return fit.best_effort; });
    }
    if (animation_fit_best_effort) {
        if (std::find(plan.approximations.begin(), plan.approximations.end(), "animation_fit_best_effort") ==
            plan.approximations.end()) plan.approximations.push_back("animation_fit_best_effort");
        feature_status.insert(feature_status.end(), {"animation_fitting", "best_effort"});
    }
    metadata.applied_approximations = plan.approximations;
    if (!compiler::serialize_graph(graph, staging, serialized, error,
                                   context.target_profile()) ||
        !compiler::write_build_manifest(graph, serialized, staging, error, metadata)) {
        cleanup_staging(staging);
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    resources = std::move(serialized.files);
    std::string report;
    if (options.validate && emit_unit && !validation::validate_unit_v115(unit_path, report)) {
        cleanup_staging(staging);
        std::cerr << report << "\n";
        return 3;
    }
    for (const auto& resource : resources) {
        if (!std::filesystem::is_regular_file(preferred_path(staging / resource), ec) || ec) {
            error = "staged resource is missing: " + resource;
            cleanup_staging(staging);
            std::cerr << "compile failed: " << error << "\n";
            return 2;
        }
    }
    CompileOptions staged_options = options;
    staged_options.output_dir = staging;
    if (!write_manifest(scene, staged_options, physics, emit_unit ? unit_path : std::filesystem::path{}, "compiled", {}, resources, feature_status, "compile_manifest.json", plan.approximations, metadata.emitted_clips, serialized.texture_normalizations)) {
        error = "cannot write success manifest in staging directory";
        cleanup_staging(staging);
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    if (!publish_staged(staging, options.output_dir, error)) {
        cleanup_staging(staging);
        std::cerr << "compile failed: " << error << "\n";
        return 2;
    }
    cleanup_staging(staging);

    std::size_t verts = 0;
    std::size_t tris = 0;
    for (const auto& p : scene.primitives) {
        for (const auto& c : p.channels) {
            if (c.semantic == VertexChannel::Semantic::Position && c.components) {
                verts += c.values.size() / c.components;
                break;
            }
        }
        tris += p.indices.size() / 3;
    }

    if (options.output_kind == OutputKind::Animations)
        std::cout << "compiled " << plan.animation_indices.size() << " animation clip(s) -> " << display_output_dir.string() << "\n";
    else
        std::cout << "compiled " << scene.primitives.size() << " primitive(s), " << verts << " vertices, " << tris
                  << " triangles -> " << (display_output_dir / unit_path.filename()).string() << "\n";
    if (options.validate && !report.empty()) std::cout << report << "\n";
    for (const auto& approximation : plan.approximations)
        std::cout << "approximation: " << approximation << "\n";
    for (const auto& n : scene.notes) std::cout << "note: " << n << "\n";
    return 0;
}

} // namespace dtglb::app
