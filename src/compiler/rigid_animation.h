#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::compiler {

// Lower animated, unskinned rigid geometry into one synthesized SkinDT, so object
// clips play through the unit's skeletal animation.
bool lower_rigid_animation(const Scene& source, Scene& lowered, std::string& error);

} // namespace dtglb::compiler
