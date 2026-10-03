#pragma once

#include "scene/scene.h"
#include "stingray/physics/physx_cooking.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::physics {

// Column-major inverse of a finite, orientation-preserving rigid frame.
bool invert_rigid_body_frame(const Matrix4& world, Matrix4& inverse);

// Convert world-baked collision geometry into a rigid body's local frame.
// Metadata and non-geometric channels are retained; bounds are recomputed.
bool localize_rigid_primitive(const Primitive& source,
                              const Matrix4& body_world,
                              Primitive& out,
                              std::string& error);

// Build the single actor described by the scene's typed asset definition.
// Collider geometry is already baked into UNIT-root space in
// Scene::collider_primitives.
bool build_authored_actor(const Scene& scene,
                          const std::string& root_node,
                          std::uint32_t root_hash,
                          PhysicsActor& actor,
                          std::string& error);

#if defined(_MSC_VER) && defined(DTGLB_HAS_PHYSICS_COLLECTIONS)
// Builds the PhysX collection (jointed bodies, each an actor of its own) and one
// UNIT actor record per body in actor_records. The engine creates those records
// as separate, unjointed actors. Retail ragdolls ship the collection alone, with
// the bodies disabled until a state-machine ragdoll state creates them;
// defer_dynamic_bodies emits dynamic bodies that way.
bool build_authored_physics_scene(const Scene& scene,
                                  const std::vector<std::uint32_t>& source_node_hashes,
                                  std::vector<std::uint8_t>& bytes,
                                  std::vector<std::vector<std::uint8_t>>& actor_records,
                                  std::string& error,
                                  bool defer_dynamic_bodies = false);
#endif

} // namespace dtglb::stingray::physics
