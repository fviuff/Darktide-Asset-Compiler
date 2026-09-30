#pragma once
#include "compiler/resource_graph.h"
#include "compiler/target_profile.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace dtglb::compiler {
enum class ArtifactRole { Primary, Stream, Binary, Bulk, Metadata, Auxiliary };
struct ResourceArtifact {
    ResourceKey logical_resource;
    ArtifactRole role = ArtifactRole::Primary;
    std::string path;
    // Serialized artifacts remain file-backed so large streams are not copied
    // back into process memory merely to inventory them.
    std::uint64_t size = 0;
};
struct TextureDimensionNormalization {
    ResourceKey resource;
    std::string profile;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t stored_width = 0;
    std::uint32_t stored_height = 0;
};
struct SerializedResources {
    ResourceClosure closure;
    std::vector<std::string> files;
    std::vector<ResourceArtifact> artifacts;
    std::vector<TextureDimensionNormalization> texture_normalizations;
};
struct BuildMetadata {
    std::string requested_output = "all";
    std::vector<std::string> applied_approximations;
    std::optional<int> selected_scene;
    std::optional<int> requested_clip;
    std::vector<int> emitted_clips;
    TargetProfile target_profile = darktide_target_profile();
};
bool serialize_graph(const ResourceGraph& graph, const std::filesystem::path& directory,
                     SerializedResources& out, std::string& error,
                     const TargetProfile& target_profile = darktide_target_profile());
bool write_build_manifest(const ResourceGraph& graph, const SerializedResources& resources,
                          const std::filesystem::path& directory,
                          std::string& error, const BuildMetadata& metadata = {});
bool read_build_artifact_paths(const std::filesystem::path& directory,
                               std::vector<std::string>& paths, std::string& error);
}
