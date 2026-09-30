#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dtglb::stingray::material {

constexpr std::uint32_t kMaterialVersion = 61;
constexpr std::uint32_t kMaterialPayloadOffset = 28;
constexpr std::uint32_t kAbsentSectionOffset = 0xffffffffu;
constexpr std::size_t kMaterialHeaderBytes = 68;
constexpr std::uint32_t kSurfaceMaterialContextHash = 0x0254a04bu;

struct TextureBinding {
    std::uint32_t channel_hash = 0;
    std::uint64_t resource_hash = 0;
};

struct ContextBinding {
    std::uint32_t name_hash = 0;
    std::uint32_t value_hash = 0;
};

struct Variable {
    std::uint32_t klass = 0;
    std::uint32_t elements = 0;
    std::uint32_t name_hash = 0;
    std::uint32_t data_offset = 0;
    std::uint32_t element_stride = 0;
};

struct MaterialStream {
    std::uint32_t version = kMaterialVersion;
    std::uint32_t direct_shader_selector_hash = 0;
    std::uint64_t shader_provider_material_hash = 0;
    std::uint64_t parent_material_hash = 0;
    std::vector<std::uint32_t> unknown_u32;
    std::vector<TextureBinding> textures;
    std::vector<ContextBinding> contexts;
    std::vector<Variable> variables;
    std::vector<std::uint8_t> variable_data;
    std::vector<std::array<std::uint8_t,5>> unknown_5;
    std::vector<std::array<std::uint8_t,8>> unknown_8;
    std::vector<std::uint8_t> shader_blob;
    std::vector<std::uint8_t> other_blob;
    std::uint32_t shader_offset_if_empty = kAbsentSectionOffset;
    std::uint32_t other_offset_if_empty = kAbsentSectionOffset;
};

// Structurally bounded v61 bytes retained verbatim when the parsed model
// cannot represent every record in an edited stream.
struct PreservedMaterialStream {
    std::vector<std::uint8_t> bytes;
};

struct MaterialDependencyHashes {
    std::uint64_t shader_provider_material_hash = 0;
    std::uint64_t parent_material_hash = 0;
    std::vector<std::uint64_t> texture_resource_hashes;
};

struct TextureResourceHashEdit {
    std::uint32_t channel_hash = 0;
    std::uint64_t expected_resource_hash = 0;
    std::uint64_t replacement_resource_hash = 0;
};

struct MaterialHeader {
    std::uint64_t resource_hash = 0;
    std::string stream_name;
};

struct InheritedMaterialSpec {
    std::uint64_t shader_provider_material_hash = 0;
    std::uint64_t parent_material_hash = 0;
    std::vector<TextureBinding> textures;
    std::vector<Variable> variables;
    std::vector<std::uint8_t> variable_data;
    // For callers that need contexts beyond the evidenced surface material.
    // Do not combine this with surface_material; the builder rejects that
    // ambiguous form instead of silently overriding one source.
    std::optional<std::vector<ContextBinding>> contexts;
    std::string surface_material;
};

bool validate_material_stream_model(const MaterialStream& model, std::string& error);
std::vector<std::uint8_t> serialize_material_stream(const MaterialStream& model, std::string& error);
bool parse_material_stream(const std::vector<std::uint8_t>& blob, MaterialStream& out, std::string& error);
bool validate_preserved_material_v61(const std::vector<std::uint8_t>& bytes,
                                     MaterialDependencyHashes& dependencies,
                                     std::string& error);

// Copy a v61 donor stream and replace bytes within its variable_data section.
// data_offset is relative to variable_data; expected_bytes must match the donor
// and replacement_bytes must have the same nonzero length. This preserves every
// other stream byte verbatim and does not interpret variable or shader meaning.
bool clone_material_v61_with_variable_data_edit(
    const std::vector<std::uint8_t>& donor,
    std::uint32_t data_offset,
    const std::vector<std::uint8_t>& expected_bytes,
    const std::vector<std::uint8_t>& replacement_bytes,
    std::vector<std::uint8_t>& out,
    std::string& error);

// Copy a v61 stream and replace selected texture resource hashes by channel.
// Each edit must identify one unique channel and match its expected donor hash.
// Only the corresponding 8-byte resource-hash fields are changed.
bool clone_material_v61_with_texture_hash_edits(
    const std::vector<std::uint8_t>& donor,
    const std::vector<TextureResourceHashEdit>& edits,
    std::vector<std::uint8_t>& out,
    std::string& error);

std::vector<std::uint8_t> serialize_material_header(const MaterialHeader& model, std::string& error);
bool parse_material_header(const std::vector<std::uint8_t>& blob, MaterialHeader& out, std::string& error);

bool build_inherited_material(const InheritedMaterialSpec& spec, MaterialStream& out, std::string& error);

bool write_material_pair(
    std::string resource_name,
    const MaterialStream& stream,
    const std::filesystem::path& header_path,
    const std::filesystem::path& stream_path,
    std::string stream_name,
    std::string& error);

bool write_preserved_material_pair(
    std::string resource_name,
    const PreservedMaterialStream& stream,
    const std::filesystem::path& header_path,
    const std::filesystem::path& stream_path,
    std::string stream_name,
    std::string& error);

} // namespace dtglb::stingray::material
