#pragma once

#include "stingray/physics/physics_collection.h"

namespace dtglb::stingray::physics {

// Emit the complete native physics bytearray: dependency collection, aligned
// actor/joint collection, node/property lookup tables and the final 128-byte
// physics_scene_data header. Body poses are already in UNIT coordinates, so
// the header's root transform is identity.
// Available when DTGLB_HAS_PHYSICS_COLLECTIONS is defined by the SDK build.
bool serialize_physics_scene(const std::vector<CollectionBody>& bodies,
                             const std::vector<CollectionJoint>& joints,
                             std::vector<std::uint8_t>& bytes,
                             std::string& error);

} // namespace dtglb::stingray::physics
