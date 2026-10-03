#pragma once

#include <optional>
#include <string>
#include <vector>

namespace dtglb {

enum class ColliderShape { Geometry, Convex, Box, Sphere, Capsule };
enum class ColliderRole { Collision, Both };

// Version 1 describes one asset-wide rigid body, bound to the generated UNIT
// root. Exported node transforms remain authoritative for collider placement.
struct BodyDefinition {
    std::string id;
    std::string actor = "static";
    float mass = 0.0f;
    std::string material = "default";
    int source_node = -1; // -1 for the version-1 generated UNIT root.
};

enum class JointKind { Fixed, Hinge, Ragdoll };

struct JointDefinition {
    std::string id;
    std::string body_a;
    std::string body_b;
    int source_node = -1;
    JointKind kind = JointKind::Fixed;
    // Radians. An unlimited hinge omits both twist limits.
    std::optional<float> twist_min;
    std::optional<float> twist_max;
    float swing_y = 0.78539816339f;
    float swing_z = 0.78539816339f;
};

struct ColliderDefinition {
    std::string id;
    std::string body;
    int source_node = -1;
    ColliderShape shape = ColliderShape::Geometry;
    ColliderRole role = ColliderRole::Collision;
};

// A node's meshes, and every mesh below it, belong to the named UNIT visibility
// group (Unit.set_visibility(unit, group, visible)).
struct VisibilityGroupMember {
    std::string group;
    int source_node = -1;
};

// A skin joint that swings under gravity (state-machine pendulum constraint), or
// with jiggle set, whose position springs behind its animated position.
// length 0 means "measure it": distance to the child joint, else the bone length.
struct DangleDefinition {
    int source_node = -1;
    bool jiggle = false;
    float length = 0.0f;
    float mass = 1.0f;
    float gravity = 9.82f;
    float damping = 3.0f;       // jiggle default 400
    float stiffness = 0.0f;     // swing: pull to rest; jiggle: spring constant, default 1000
    float max_angle_degrees = 0.0f;
    float max_stretch = 0.1f;   // jiggle only
};

struct AssetDefinition {
    std::optional<BodyDefinition> body;
    std::vector<ColliderDefinition> colliders;
    std::vector<BodyDefinition> node_bodies;
    std::vector<JointDefinition> joints;
    std::vector<VisibilityGroupMember> visibility_groups;
    std::vector<DangleDefinition> dangles;
};

} // namespace dtglb
