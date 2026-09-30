#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

enum class TangentRegenerationStatus { Regenerated, NotApplicable, Invalid };

TangentRegenerationStatus regenerate_atlas_tangents(Primitive& primitive, std::string& error);
TangentRegenerationStatus regenerate_indexed_tangents(Primitive& primitive, std::string& error);

} // namespace dtglb::processing
