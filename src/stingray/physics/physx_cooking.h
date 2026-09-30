#pragma once

#include "scene/scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::physics {

enum class PhysicsShapeType : std::uint32_t { Sphere = 0, Box = 1, Capsule = 2, TriangleMesh = 3, Convex = 4, HeightField = 5, Unknown7 = 7 };
struct FittedActorOptions {
    PhysicsShapeType shape = PhysicsShapeType::TriangleMesh;
    std::string actor_template = "static";
    std::string material = "default";
    std::string shape_template = "default";
    float mass = 0.0f;
};
struct PhysicsShape {
    std::string name; std::string node; std::uint32_t node_hash = 0; int node_index = -1; PhysicsShapeType type = PhysicsShapeType::TriangleMesh;
    std::string material = "default"; std::string shape_template = "default";
    Matrix4 local_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float radius = 0.0f; float height = 0.0f; std::array<float, 3> half_extents{}; int primitive = -1;
    bool computed_convex = false; std::vector<std::uint8_t> physx_cooked; std::vector<std::uint8_t> secondary_cooked;
};
struct PhysicsActor {
    std::string name; std::uint32_t name_hash = 0; std::string actor_template = "static";
    std::string node; std::uint32_t node_hash = 0; int node_index = -1;
    float mass = 0.0f; bool enabled = true; std::vector<PhysicsShape> shapes;
};

// Serialized shape-template id32. The global "default" template has an empty
// collides-with mask: retail uses it for static and keyframed shapes, where the
// other body's mask provides the contact. A simulated body on "default" collides
// with nothing (it falls through the level), so dynamic shapes are mapped to the
// retail dynamic-prop template (0x9800a618, luggables/throwables), and node-bound
// dynamic bodies (ragdolls, articulated parts) to "ragdoll". "#xxxxxxxx" names a
// raw id32 for templates whose string names are unknown.
std::uint32_t shape_template_id32(const std::string& actor_template,
                                  const std::string& shape_template,
                                  bool node_bound);

// PhysX 4.1/4.1.2 triangle-mesh stream cooker.  The returned stream is the
// complete native PxTriangleMesh serialization (never a hand-written BVH).
struct CookedTriangleMesh {
    std::vector<std::uint8_t> bytes;
    std::uint32_t source_triangle_count = 0;
};

bool cook_triangle_mesh(const Primitive& primitive,
                        CookedTriangleMesh& cooked,
                        std::string& error);

// Resolve the scene-level physics directives into runtime-ready ActorResource shapes.
// Triangle meshes are cooked to PhysX streams here; primitive shapes need no cooked blob.
bool prepare_scene_physics(const std::vector<Primitive>& primitives,
                           std::vector<PhysicsActor>& actors, std::string& error);

// Build geometry, convex, or fitted primitive collision from the emitted
// Stingray-space geometry and attach it to the UNIT root.
bool build_actor(const std::vector<Primitive>& primitives,
                 std::string actor_name,
                 std::string root_node,
                 std::uint32_t root_node_hash,
                 const FittedActorOptions& options,
                 PhysicsActor& actor,
                 std::string& error);

// Serialize one v115 actor record with native cooked or primitive shapes.
// node_bound selects the template mapping for bodies that also live in the
// UNIT's PhysX collection (ragdolls, jointed parts).
bool serialize_actor(const PhysicsActor& actor,
                     std::vector<std::uint8_t>& bytes,
                     std::string& error,
                     bool node_bound = false);

} // namespace dtglb::stingray::physics
