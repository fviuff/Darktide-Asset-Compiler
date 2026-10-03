#pragma once
#include "compiler/resource_key.h"
#include "compiler/target_profile.h"
#include "stingray/unit/unit_resource.h"
#include "stingray/bones/bones_writer.h"
#include "stingray/animation/animation_writer.h"
#include "stingray/material/material_v61.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/state_machine/state_machine_writer.h"
#include <variant>
#include <vector>

namespace dtglb::compiler {
struct TextureResource {
    stingray::texture::ImageRGBA image;
    stingray::texture::TextureProfile profile;
};
// A copy of one of the game's textures: its kind-1 body and, when it has streamed mips, their stream.
struct PreservedTexture {
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> stream;
};
// A particles resource body (version 102).
struct ParticlesResource {
    std::vector<std::uint8_t> body;
};
// A copy of one of the game's units (a mesh particle effects draw), its materials renamed to owned copies.
struct PreservedUnit {
    std::vector<std::uint8_t> body;
};
using NativeResource = std::variant<stingray::unit::UnitResource,
    stingray::bones::BonesResource, stingray::animation::BuiltAnimation,
    stingray::material::MaterialStream, stingray::material::PreservedMaterialStream,
    TextureResource, std::vector<std::uint8_t>, PreservedTexture, ParticlesResource, PreservedUnit>;
struct OwnedResourceNode {
    ResourceKey key;
    std::string file;
    NativeResource resource;
    std::vector<ResourceKey> dependencies;
};
struct ExternalResourceRef {
    ResourceKey key;
    // Retail parent materials use their established resident dependency path;
    // explicit material overrides retain their established PACKAGE membership.
    bool package_member = false;
};
struct ResourceGraph {
    std::vector<OwnedResourceNode> owned;
    std::vector<ExternalResourceRef> external;
    std::vector<ResourceKey> roots;
};
struct ResourceClosure {
    std::vector<std::size_t> owned; // deterministic dependency-first order
    std::vector<ExternalResourceRef> external;
    std::vector<ResourceKey> package_members;
};
bool portable_payload_path(const std::string& path);
std::string resource_stream_name(const OwnedResourceNode& node);
std::string resource_stream_name(const OwnedResourceNode& node, const TargetProfile& profile);
bool validate_graph(const ResourceGraph& graph, ResourceClosure& closure, std::string& error,
                    const std::vector<std::string>& reserved_payloads = {});
bool validate_graph(const ResourceGraph& graph, ResourceClosure& closure, std::string& error,
                    const TargetProfile& profile, const std::vector<std::string>& reserved_payloads = {});
}
