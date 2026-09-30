#pragma once

#include "scene/scene.h"

namespace dtglb::processing {

// Moves generated core PBR materials that sample only TEXCOORD_1 onto UV0,
// copying the corresponding source and render UV values before doing so.
void canonicalize_generated_uv1_materials(Scene& scene);

} // namespace dtglb::processing
