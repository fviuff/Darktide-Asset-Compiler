#pragma once
#include "compiler/compilation_context.h"
#include "compiler/resource_graph.h"
#include "scene/scene.h"

namespace dtglb::compiler {
// Each of the scene's particle effects becomes a renamed copy of the game's effect. Its materials, their shader
// provider/parent and their textures are shipped with the asset too (edited where the author asked), so the
// effect works wherever the asset is loaded. Adds the effects to `effects` (they are graph roots).
bool build_particle_effects(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                            std::vector<ResourceKey>& effects, std::string& error);
}
