#pragma once
#include "compiler/resource_graph.h"
#include <functional>
#include "compiler/compilation_context.h"
#include "scene/scene.h"
#include "stingray/unit/unit_identity.h"

namespace dtglb::compiler {
// Gives the key of an owned copy of one of the game's textures.
using OwnTexture = std::function<bool(std::uint64_t texture_hash, ResourceKey& out, std::string& error)>;
// Ship a renamed copy of one of the game's shader provider/parent materials (they hold the compiled shaders).
// Its textures stay the game's own unless own_texture is given.
bool own_game_shader_material(const CompilationContext& context, ResourceGraph& graph, std::uint64_t hash,
                              const std::filesystem::path& stream, std::uint64_t expected_parent,
                              const ResourceKey* owned_parent, ResourceKey& out, std::string& error,
                              const OwnTexture& own_texture = {});
std::string core_material_gap(const Scene& scene, const MaterialInfo& material);
bool build_core_materials(const Scene& scene, const CompilationContext& context,
                          ResourceGraph& graph,
                          std::vector<std::pair<std::string, std::string>>& bindings,
                          std::string& error,
                          const stingray::unit::MaterialSlotLowering* slots = nullptr);
}
