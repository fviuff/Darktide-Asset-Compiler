#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

// Walk/run clips often carry the character's travel in a root bone, so a looping state slides forward and snaps
// back. For every clip this removes the horizontal travel of the top-most moving joint that carries the body (half
// the skin or more below it) as a straight line from the first to the last key, so loops close and vertical motion
// stays, and adds a note with the ground speed the mod should move the unit at.
bool remove_root_motion(Scene& scene, std::string& error);

} // namespace dtglb::processing
