#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::compiler {

// Experimental: lower animated, unskinned rigid geometry into one synthesized
// SkinDT.  This is intentionally opt-in until the runtime representation has
// been verified against a live Darktide client.
bool lower_rigid_animation(const Scene& source, Scene& lowered, std::string& error);

} // namespace dtglb::compiler
