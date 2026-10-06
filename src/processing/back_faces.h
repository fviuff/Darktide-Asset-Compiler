#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

// The game draws a material double-sided only when its shader was compiled that way; the shaders our materials
// use cull back faces. A glTF double-sided material therefore gets its back faces as geometry: each of its
// primitives is copied into a second primitive with reversed winding whose normals (and morph normal deltas)
// point the other way; tangent handedness flips so the texture mapping stays the same.
// Returns the number of back-face primitives added.
std::size_t add_back_faces(Scene& scene, std::string& error);

} // namespace dtglb::processing
