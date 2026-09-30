#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

// Applies one global unit conversion to all spatial scene data.  The operation
// is rejected without modifying the scene when scale is not finite and > 0.
bool scale_scene(Scene& scene, float scale, std::string& error);

} // namespace dtglb::processing
