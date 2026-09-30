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

} // namespace dtglb::stingray::physics
