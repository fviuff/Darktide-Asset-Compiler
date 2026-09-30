#pragma once
#include "scene/scene.h"

namespace dtglb::compiler {
// Resolve authored collider ownership and separate render/collision geometry.
// Called exactly once after frontend emission and before image/material work.
bool resolve_asset_definition(Scene& scene, std::string& error);
}
