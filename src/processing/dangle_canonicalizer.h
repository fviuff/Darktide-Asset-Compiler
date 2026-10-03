#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

// Darktide's pendulum constraint swings a bone so its local +X points at a
// simulated bob; retail rigs run +X down the bone. Blender/glTF bones run
// +Y instead, so each dangling bone's rest frame is rotated to put +X along
// the bone. Its children, inverse bind matrices and animation tracks are
// compensated, so the skinned result is unchanged. Also resolves an unset
// dangle length from the child joint distance.
bool canonicalize_dangles(Scene& scene, std::string& error);

} // namespace dtglb::processing
