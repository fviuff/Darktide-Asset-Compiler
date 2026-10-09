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

enum class JointKind { Fixed, Hinge, Ragdoll, Slider, Ball };

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
    // Slider: travel along the marker's x axis in meters; no limits = free.
    std::optional<float> slide_min;
    std::optional<float> slide_max;
    // Spring pulling the joint back to its authored pose (0 = none).
    float spring_stiffness = 0.0f;
    float spring_damping = 0.0f;
    // The joint breaks above this force / torque (0 = never).
    float break_force = 0.0f;
    float break_torque = 0.0f;
};

struct ColliderDefinition {
    std::string id;
    std::string body;
    int source_node = -1;
    ColliderShape shape = ColliderShape::Geometry;
    ColliderRole role = ColliderRole::Collision;
};

// An actor of its own on a node (a UNIT actor record, not part of the PhysX collection): the node's mesh
// gives its shape, fitted in the node's frame. Enemy hit zones are these (keyframed, following their bone,
// looked up by name from Lua). Templates are physics_properties names or "#xxxxxxxx" ids.
struct NodeActorDefinition {
    std::string name;
    std::string actor_template = "keyframed";
    std::string shape_template = "default";
    std::string material = "default";
    ColliderShape shape = ColliderShape::Capsule;
    bool spawn = true;          // created when the unit spawns (else by script, Unit.create_actor)
    int source_node = -1;
};

// A character mover (the capsule that walks a unit through the level, Unit.set_mover / Mover.*).
struct MoverDefinition {
    std::string name = "mover";
    float height = 1.8f;
    float radius = 0.4f;
    float slope_limit = 0.6981317f;   // radians
    std::string collision_filter = "filter_minion_mover";
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

// A skin joint that points at a named target (state-machine aim constraint): the listed bones turn, each by its
// weight, so the direction from them to this joint faces the target; scripts move the target
// (Unit.animation_set_constraint_target).
struct AimDefinition {
    int source_node = -1;
    std::string target;
    std::vector<std::pair<std::string, float>> turn;   // bone name -> weight
};

// One detail level of a LOD object: the node's meshes (and those below it) draw while the
// object's bounding volume takes up between the previous level's height and down_to of the
// screen height. The last level with down_to 0 never hides.
struct LodLevel {
    std::string group;
    int level = 0;
    float down_to = 0.0f;
    int source_node = -1;
};

// How the meshes of a node (and the nodes below it, nearest setting wins) draw: in view and/or into shadows.
struct RenderSetting {
    int source_node = -1;
    bool visible = true;
    bool shadow = true;
};

struct AssetDefinition {
    std::optional<BodyDefinition> body;
    std::vector<ColliderDefinition> colliders;
    std::vector<BodyDefinition> node_bodies;
    std::vector<JointDefinition> joints;
    std::vector<NodeActorDefinition> node_actors;
    std::vector<MoverDefinition> movers;
    std::vector<VisibilityGroupMember> visibility_groups;
    std::vector<DangleDefinition> dangles;
    std::vector<AimDefinition> aims;
    std::vector<LodLevel> lod_levels;
    std::vector<RenderSetting> render_settings;
    std::vector<int> shadow_lights;   // source nodes whose light casts shadows
    std::string flow;   // authored unit flow graph (JSON, stingray/flow/flow_authoring.h), empty = none
    std::string script_data;   // Unit.get_data values (JSON object, stingray/unit/script_data.h), empty = none
};

} // namespace dtglb
