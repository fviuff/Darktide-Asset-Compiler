#pragma once

#include "stingray/physics/physx_cooking.h"
#include "scene/scene.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::physics {

enum class CollectionMotion : std::uint8_t { Locked, Limited, Free };

struct CollectionBody {
    PhysicsActor actor;
    Matrix4 world_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

struct CollectionJoint {
    std::string name;
    std::uint32_t body0 = 0, body1 = 0;
    Matrix4 local_frame0{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Matrix4 local_frame1{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::array<CollectionMotion, 6> motion{};
    float twist_lower = -0.78539816339f, twist_upper = 0.78539816339f;
    float swing_y = 0.78539816339f, swing_z = 0.78539816339f;
    float linear_lower = -1.0f, linear_upper = 1.0f;   // x axis travel when motion[0] is Limited
    // Drive back to the authored pose (stiffness 0 = none) on twist, swing and twist (as retail ragdoll joints) or x.
    enum class Drive { None, Twist, SwingTwist, X };
    Drive drive = Drive::None;
    float drive_stiffness = 0.0f, drive_damping = 0.0f;
    float break_force = 0.0f, break_torque = 0.0f;     // 0 = never breaks
    bool collision_enabled = false;
};

struct CollectionShapeBinding {
    std::uint32_t body_index = 0, local_shape_index = 0, object_index = 0;
    std::uint32_t shape_template_hash = 0;
};

struct CollectionMaterialBinding {
    std::uint32_t object_index = 0, material_hash = 0;
};

struct PhysicsCollections {
    std::vector<std::uint8_t> dependencies, objects;
    // Ordinals in the reloaded PxCollection, suitable for getObject(index).
    std::vector<std::uint32_t> body_object_indices;
    std::vector<CollectionShapeBinding> shapes;
    std::vector<CollectionMaterialBinding> materials;
};

bool serialize_physics_collections(const std::vector<CollectionBody>& bodies,
                                   const std::vector<CollectionJoint>& joints,
                                   PhysicsCollections& output,
                                   std::string& error);

// Loads serialized collections through PhysX and lists their D6 joints (motion per axis, limits, drives, break force).
// A unit's physics scene (the size-prefixed blob holding both collections and the body tables) as JSON: bodies
// (name, node, enabled, mass, global pose, shapes with their template and local pose) and D6 joints (bodies, frames,
// motions, limits, drives, break force).
bool describe_physics_collection(const std::vector<std::uint8_t>& scene, std::string& out, std::string& error);

// Cooked PhysX shapes (type 3 triangle mesh, 4 convex, as UNIT actor records carry them) as JSON: vertices and
// triangles per mesh (convex polygons as fans).
bool describe_cooked_meshes(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& meshes,
                            std::string& out, std::string& error);

bool describe_physics_joints(const std::vector<std::uint8_t>& dependencies, const std::vector<std::uint8_t>& objects,
                             std::string& out, std::string& error);

} // namespace dtglb::stingray::physics
