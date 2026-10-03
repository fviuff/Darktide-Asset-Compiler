#pragma once

#include "scene/asset_definition.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dtglb {

using Matrix4 = std::array<float, 16>;

struct VertexChannel {
    enum class Semantic : std::uint32_t {
        Position = 0,
        Normal = 1,
        Tangent = 2,
        Binormal = 3,
        Texcoord = 4,
        Color = 5,
        BlendIndices = 6,
        BlendWeights = 7,
    };

    Semantic semantic = Semantic::Position;
    std::uint32_t set = 0;
    std::uint32_t components = 0;
    std::vector<float> values;
};

struct CustomAttribute {
    std::string name;
    std::uint32_t components = 0;
    std::vector<float> values;
};

struct MorphTarget {
    std::string name;
    std::vector<VertexChannel> channels;
};

struct Primitive {
    std::string name;
    std::string material_name = "__no_material__";
    int source_node = -1;
    // Node-bound rigid physics owns these world-baked render channels even if
    // later canonicalization detaches their original mesh node.
    int render_owner_node = -1;
    int source_mesh = -1;
    int source_primitive = -1;
    // Stable collision grouping identity assigned while emitting frontend geometry.
    // Unlike source_node, these survive canonicalization and partition/copy paths.
    int collision_object = -1;
    int collision_instance = -1;
    int material = -1;
    int mode = 4;

    // Local-space glTF attributes converted only to Stingray's basis. These are retained
    // for skinning, morphs, animation, collision and future non-baked node emission.
    std::vector<VertexChannel> source_channels;
    std::vector<CustomAttribute> custom_attributes;
    // World-baked attributes used by the currently verified static MeshGeometry writer.
    std::vector<VertexChannel> channels;
    std::vector<std::uint32_t> indices;
    std::vector<MorphTarget> morph_targets;
    std::vector<float> morph_weights;

    std::array<float, 3> source_bounds_min{};
    std::array<float, 3> source_bounds_max{};
    float source_bounds_radius = 0.0f;
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
    float bounds_radius = 0.0f;
};

struct TextureTransform {
    std::array<float, 2> offset{0.0f, 0.0f};
    std::array<float, 2> scale{1.0f, 1.0f};
    float rotation = 0.0f;
    int texcoord = -1;
};

struct TextureBinding {
    int texture = -1;
    int texcoord = 0;
    float scale = 1.0f;    // normal scale or generic strength when applicable
    float strength = 1.0f; // occlusion strength
    TextureTransform transform;
};

struct MaterialInfo {
    enum class Intent { Generated, Emissive, External, Template, Donor, GameShader };
    std::string name;
    Intent intent = Intent::Generated;
    std::string external_resource;
    std::string surface_material = "default";
    std::string donor_family;
    std::string donor_stream_path;
    std::map<std::string, float> donor_variable_overrides;
    // GameShader: a retail material stream (donor_stream_path) cloned with these
    // variables (name or #hex IdString32 -> 1..4 floats) and texture channels
    // (name or #hex -> base_color | normal | orm | emissive glTF image) replaced.
    std::map<std::string, std::vector<float>> shader_variables;
    std::map<std::string, std::string> shader_textures;
    // Retail shader provider/parent material hash -> stream file, shipped as owned copies.
    std::map<std::uint64_t, std::string> shader_streams;
    std::array<float, 4> base_color{1, 1, 1, 1};
    float metallic = 1.0f;
    float roughness = 1.0f;
    std::array<float, 3> emissive{0, 0, 0};
    float emissive_strength = 1.0f;
    bool double_sided = false;
    bool unlit = false;
    std::string alpha_mode = "OPAQUE";
    float alpha_cutoff = 0.5f;

    TextureBinding base_color_texture;
    TextureBinding metallic_roughness_texture;
    TextureBinding normal_texture;
    TextureBinding occlusion_texture;
    TextureBinding emissive_texture;

    // KHR_materials_pbrSpecularGlossiness is retained verbatim for a later
    // conversion stage; it must not be silently folded into metallic-roughness.
    std::array<float, 4> diffuse_factor{1, 1, 1, 1};
    std::array<float, 3> specular_factor{1, 1, 1};
    float glossiness_factor = 1.0f;
    TextureBinding diffuse_texture;
    TextureBinding specular_glossiness_texture;

    // Common KHR_materials_* values are retained even where Darktide material emission
    // still needs a shader-template mapping.
    float ior = 1.5f;
    float transmission = 0.0f;
    float clearcoat = 0.0f;
    float clearcoat_roughness = 0.0f;
    float sheen_roughness = 0.0f;
    std::array<float, 3> sheen_color{0, 0, 0};
    float specular = 1.0f;
    std::array<float, 3> specular_color{1, 1, 1};

    // These flags distinguish authored extension semantics from their default
    // values.  The current Darktide material writer cannot lower these profiles.
    bool has_pbr_specular_glossiness = false;
    bool has_ior = false;

    bool has_clearcoat = false;
    TextureBinding clearcoat_texture;
    TextureBinding clearcoat_roughness_texture;
    TextureBinding clearcoat_normal_texture;

    bool has_transmission = false;
    TextureBinding transmission_texture;

    bool has_volume = false;
    float thickness = 0.0f;
    std::array<float, 3> attenuation_color{1, 1, 1};
    float attenuation_distance = 0.0f;
    TextureBinding thickness_texture;

    bool has_sheen = false;
    TextureBinding sheen_color_texture;
    TextureBinding sheen_roughness_texture;

    bool has_specular = false;
    TextureBinding specular_texture;
    TextureBinding specular_color_texture;

    bool has_iridescence = false;
    float iridescence = 0.0f;
    float iridescence_ior = 1.3f;
    float iridescence_thickness_min = 100.0f;
    float iridescence_thickness_max = 400.0f;
    TextureBinding iridescence_texture;
    TextureBinding iridescence_thickness_texture;

    bool has_anisotropy = false;
    float anisotropy_strength = 0.0f;
    float anisotropy_rotation = 0.0f;
    TextureBinding anisotropy_texture;

    bool has_diffuse_transmission = false;
    float diffuse_transmission = 0.0f;
    std::array<float, 3> diffuse_transmission_color{1, 1, 1};
    TextureBinding diffuse_transmission_texture;
    TextureBinding diffuse_transmission_color_texture;

    bool has_dispersion = false;
    float dispersion = 0.0f;
};

struct RGBA8Payload {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
};

struct ImageInfo {
    std::string name;
    std::string mime_type;
    std::string uri;
    std::vector<std::uint8_t> bytes;
    std::optional<RGBA8Payload> rgba8;
    // Retained only when an optional alternate source could not be loaded;
    // source selection reports it if no higher-fidelity alternate succeeds.
    std::string load_error;
};

struct SamplerInfo {
    std::string name;
    int mag_filter = 0;
    int min_filter = 0;
    int wrap_s = 10497;
    int wrap_t = 10497;
};

struct TextureInfo {
    std::string name;
    int source = -1;
    int sampler = -1;
    int basisu_source = -1;
    int webp_source = -1;
    int dds_source = -1;
};


struct CameraInfo {
    enum class Type { Perspective, Orthographic };
    std::string name;
    Type type = Type::Perspective;
    float yfov = 0.0f;
    float aspect_ratio = 0.0f;
    float znear = 0.01f;
    float zfar = 0.0f;
    float xmag = 0.0f;
    float ymag = 0.0f;
};

struct LightInfo {
    enum class Type { Directional, Point, Spot };
    std::string name;
    Type type = Type::Point;
    std::array<float, 3> color{1, 1, 1};
    float intensity = 1.0f;
    float range = 0.0f;
    float inner_cone_angle = 0.0f;
    float outer_cone_angle = 0.78539816339f;
};

struct SceneInfo {
    std::string name;
    std::vector<int> roots;
};

struct NodeInfo {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    int mesh = -1;
    int skin = -1;
    int camera = -1;
    int light = -1;
    std::vector<float> weights;
    std::vector<CustomAttribute> instance_attributes;
    Matrix4 local_gltf{};
    Matrix4 world_gltf{};
    Matrix4 local_stingray{};
    Matrix4 world_stingray{};
    // Exact source TRS, already converted into Stingray basis, when the glTF node
    // was authored with TRS rather than an arbitrary matrix. Keeping this avoids
    // lossy matrix->quaternion round-trips in SceneGraph/animation resources.
    std::array<float, 3> local_translation_stingray{0,0,0};
    std::array<float, 4> local_rotation_stingray{0,0,0,1};
    std::array<float, 3> local_scale_stingray{1,1,1};
    bool has_exact_trs = false;
    bool visible = true;
};

struct SkinInfo {
    std::string name;
    int skeleton_root = -1;
    std::vector<int> joints;
    std::vector<std::string> joint_names;
    std::vector<Matrix4> inverse_bind_matrices_gltf;
    std::vector<Matrix4> inverse_bind_matrices_stingray;
};

enum class AnimationPath {
    Translation,
    Rotation,
    Scale,
    Weights,
};

enum class AnimationInterpolation {
    Linear,
    Step,
    CubicSpline,
};

struct AnimationTrack {
    int target_node = -1;
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> times;
    std::vector<float> values;
    std::uint32_t value_components = 0;
};

struct AnimationInfo {
    std::string name;
    int source_index = -1;
    std::uint64_t source_channel_count = 0;
    std::uint64_t dropped_pointer_channel_count = 0;
    std::vector<AnimationTrack> tracks;
};

struct SourceFeatureInventory {
    std::uint64_t scene_count = 0;
    std::uint64_t node_count = 0;
    std::uint64_t root_count = 0;
    std::uint64_t parent_edge_count = 0;
    std::uint64_t mesh_node_count = 0;
    std::uint64_t mesh_count = 0;
    std::uint64_t source_primitive_count = 0;
    std::uint64_t active_source_primitive_count = 0;
    std::uint64_t decoded_source_primitive_count = 0;
    std::uint64_t decoded_primitive_count = 0;
    std::uint64_t approximated_primitive_count = 0;
    std::vector<std::string> approximation_locations;
    std::array<std::uint64_t, 7> primitive_modes{};
    std::array<std::uint64_t, 7> active_primitive_modes{};

    std::uint64_t custom_attribute_count = 0;
    std::uint64_t texcoord_attribute_count = 0;
    std::uint64_t color_attribute_count = 0;
    std::uint64_t joint_attribute_count = 0;
    std::uint64_t weight_attribute_count = 0;
    std::uint32_t max_texcoord_set = 0;

    std::uint64_t morph_target_primitive_count = 0;
    std::uint64_t morph_target_count = 0;
    // Fixed initial morph weights were applied to all active primitives.  The
    // count is the number of targets successfully consumed by that bake.
    std::uint64_t fixed_initial_morph_bake_count = 0;
    std::uint64_t active_morph_target_count = 0;
    bool fixed_initial_morph_bake_succeeded = false;

    std::uint64_t material_count = 0;
    std::uint64_t material_texture_binding_count = 0;
    std::uint64_t pbr_specular_glossiness_material_count = 0;
    std::uint64_t ior_material_count = 0;
    std::uint64_t material_variant_count = 0;
    std::uint64_t material_variant_mapping_count = 0;
    std::uint64_t active_material_variant_mapping_count = 0;
    bool material_variant_selected = false;
    std::uint64_t image_count = 0;
    std::uint64_t sampler_count = 0;
    std::uint64_t texture_count = 0;

    std::uint64_t skin_count = 0;
    std::uint64_t joint_count = 0;
    std::uint64_t inverse_bind_skin_count = 0;

    std::uint64_t animation_count = 0;
    std::uint64_t animation_channel_count = 0;
    std::uint64_t animation_weight_channel_count = 0;
    std::uint64_t active_animation_count = 0;
    std::uint64_t active_animation_channel_count = 0;
    std::uint64_t active_animation_weight_channel_count = 0;
    std::uint64_t animation_linear_sampler_count = 0;
    std::uint64_t animation_step_sampler_count = 0;
    std::uint64_t animation_cubic_sampler_count = 0;

    std::uint64_t camera_count = 0;
    std::uint64_t light_count = 0;
    std::uint64_t meshopt_buffer_view_count = 0;
    std::uint64_t draco_primitive_count = 0;
    std::uint64_t gpu_instancing_node_count = 0;

    std::uint64_t physics_requested_count = 0;
    std::uint64_t physics_effective_count = 0;
    std::uint64_t physics_suppressed_count = 0;
    std::vector<std::string> physics_locations;
};

// A particle effect shipped under the asset's name: authored as a JSON description (systems, initializers,
// simulators, visualizers; see stingray/particles) or copied from one of the game's effects. The game's
// materials, units, shaders and textures it uses are shipped as owned copies (edited where the author says so).
struct ParticleMaterialEdit {
    std::map<std::string, std::vector<float>> variables; // name or #hex -> 1..4 floats
    std::map<std::string, std::string> textures;          // channel name or #hex -> image file
};
struct ParticleEffectDefinition {
    std::string name;        // resource name inside the asset's folder
    std::string description; // the effect as JSON; empty: a copy of `source`
    std::string source;      // the game's cooked .particles from a limn extract
    std::string extract;     // that extract folder (material and texture resources are looked up there)
    std::map<std::uint64_t, ParticleMaterialEdit> materials; // the game's material hash -> edits
};

struct Scene {
    SourceFeatureInventory source_features;
    std::vector<Primitive> primitives;
    AssetDefinition asset_definition;
    // Snapshot before render-only canonicalization, including collision-only
    // objects. Coordinates are world-baked into the generated UNIT root frame.
    std::vector<Primitive> collider_primitives;
    std::vector<MaterialInfo> materials;
    std::vector<ImageInfo> images;
    std::vector<SamplerInfo> samplers;
    std::vector<TextureInfo> textures;
    std::vector<CameraInfo> cameras;
    std::vector<LightInfo> lights;
    std::vector<NodeInfo> nodes;
    std::vector<SceneInfo> scenes;
    int default_scene = -1;
    int selected_scene = -1;
    std::vector<int> scene_roots;
    std::vector<SkinInfo> skins;
    std::vector<AnimationInfo> animations;
    std::vector<ParticleEffectDefinition> particle_effects;
    std::vector<std::string> extensions_used;
    std::vector<std::string> extensions_required;
    std::vector<std::string> active_extensions_used;
    std::vector<std::string> notes;
};

} // namespace dtglb
