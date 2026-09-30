#pragma once
#include "compiler/resource_graph.h"
#include "compiler/compilation_context.h"
#include "compiler/compilation_plan.h"
#include "app/compiler.h"
#include "scene/scene.h"

namespace dtglb::compiler {
bool build_glb_resources(const Scene& source, const app::CompileOptions& options,
                         const CompilationContext& context, const CompilationPlan& plan, ResourceGraph& graph,
                         std::vector<std::string>& feature_status, std::string& error);
}
