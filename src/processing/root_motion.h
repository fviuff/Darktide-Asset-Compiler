#pragma once

#include "scene/scene.h"

#include <string>

namespace dtglb::processing {

// Walk/run clips often carry the character's travel in a root bone, so a looping state slides forward and snaps
// back. For every clip this removes the horizontal travel of the top-most moving joint that carries the body (half
// the skin or more below it) as a straight line from the first to the last key, so loops close and vertical motion
// stays, and adds a note with the ground speed the mod should move the unit at.
// onto_root moves that travel onto the skeleton's top joint instead (root_point on the game's skeletons), where the
// engine reads it as root motion (Unit.animation_wanted_root_pose) without drawing it.
bool remove_root_motion(Scene& scene, std::string& error, bool onto_root = false);

} // namespace dtglb::processing
