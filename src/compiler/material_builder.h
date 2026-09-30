#pragma once
#include "compiler/resource_graph.h"
#include "compiler/compilation_context.h"
#include "scene/scene.h"
#include "stingray/unit/unit_identity.h"

namespace dtglb::compiler {
std::string core_material_gap(const Scene& scene, const MaterialInfo& material);
bool build_core_materials(const Scene& scene, const CompilationContext& context,
                          ResourceGraph& graph,
                          std::vector<std::pair<std::string, std::string>>& bindings,
                          std::string& error,
                          const stingray::unit::MaterialSlotLowering* slots = nullptr);
}
