#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::bones {

struct BonesResource {
    std::string resource_name;
    std::vector<std::string> names;
    std::vector<std::uint32_t> lod_counts;
};

std::vector<std::uint8_t> build_bones_body(
    const std::vector<std::string>& names,
    const std::vector<std::uint32_t>& lod_counts = {});

std::vector<std::uint8_t> build_cooked_bones(const BonesResource& resource);

bool validate_cooked_bones(
    const std::vector<std::uint8_t>& blob,
    std::vector<std::string>* names,
    std::string& error);

// Loads a cooked reference BONES resource whose path is supplied by the caller.
// Cooked resources retain only a hashed identity, so resource_name is required
// and its hash must match the envelope.
bool load_cooked_bones(
    const std::filesystem::path& path,
    const std::string& resource_name,
    BonesResource& resource,
    std::string& error);

// Produces source ordinal -> reference ordinal. Both lists must describe the
// same complete skeleton with unique names; partial or ambiguous mappings fail.
bool map_bone_names(
    const std::vector<std::string>& source_names,
    const std::vector<std::string>& reference_names,
    std::vector<std::uint32_t>& source_to_reference,
    std::string& error);

bool write_cooked_bones(
    const BonesResource& resource,
    const std::filesystem::path& path,
    std::string& error);

} // namespace dtglb::stingray::bones
