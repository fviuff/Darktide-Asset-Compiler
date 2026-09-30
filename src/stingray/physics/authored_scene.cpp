#include "stingray/physics/authored_physics.h"

#ifdef DTGLB_HAS_PHYSICS_COLLECTIONS
#include "stingray/physics/physics_scene.h"

#include <unordered_map>

namespace dtglb::stingray::physics {
namespace {
Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
    return result;
}
}

bool build_authored_physics_scene(const Scene& scene,
                                  const std::vector<std::uint32_t>& hashes,
                                  std::vector<std::uint8_t>& bytes,
                                  std::vector<std::vector<std::uint8_t>>& actor_records,
                                  std::string& error) {
    bytes.clear();
    actor_records.clear();
    error.clear();
    if (scene.asset_definition.node_bodies.empty()) {
        error = "authored physics scene requires at least one node body";
        return false;
    }
    std::unordered_map<std::string, std::size_t> body_by_id;
    std::vector<CollectionBody> bodies;
    std::vector<Matrix4> inverse_frames;
    bodies.reserve(scene.asset_definition.node_bodies.size());
    inverse_frames.reserve(scene.asset_definition.node_bodies.size());
    for (const auto& definition : scene.asset_definition.node_bodies) {
        if (definition.id.empty() || !body_by_id.emplace(definition.id, bodies.size()).second) {
            error = "authored physics node body ids must be non-empty and distinct";
            return false;
        }
        const int index = definition.source_node;
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size() ||
            static_cast<std::size_t>(index) >= hashes.size() || hashes[index] == 0) {
            error = "authored physics body source node or emitted hash is invalid";
            return false;
        }
        const auto& node = scene.nodes[index];
        Matrix4 inverse{};
        if (!invert_rigid_body_frame(node.world_stingray, inverse)) {
            error = "authored physics body frame must be rigid: " + definition.id;
            return false;
        }
        // Reuse the compound collider builder, including instance grouping and
        // dynamic geometry-to-convex cooking, in this body's local frame.
        Scene local_scene;
        local_scene.asset_definition.body = definition;
        for (const auto& collider : scene.asset_definition.colliders) {
            if (collider.body != definition.id) continue;
            local_scene.asset_definition.colliders.push_back(collider);
            for (const auto& primitive : scene.collider_primitives) {
                if (primitive.collision_object != collider.source_node) continue;
                Primitive local;
                if (!localize_rigid_primitive(primitive, node.world_stingray, local, error)) return false;
                local_scene.collider_primitives.push_back(std::move(local));
            }
        }
        CollectionBody body;
        const auto& name = node.name.empty() ? definition.id : node.name;
        if (!build_authored_actor(local_scene, name, hashes[index], body.actor, error)) return false;
        std::vector<std::uint8_t> record;
        if (!serialize_actor(body.actor, record, error, true)) return false;
        actor_records.push_back(std::move(record));
        body.world_transform = node.world_stingray;
        bodies.push_back(std::move(body));
        inverse_frames.push_back(inverse);
    }

    std::vector<CollectionJoint> joints;
    joints.reserve(scene.asset_definition.joints.size());
    for (const auto& definition : scene.asset_definition.joints) {
        const auto a = body_by_id.find(definition.body_a);
        const auto b = body_by_id.find(definition.body_b);
        if (a == body_by_id.end() || b == body_by_id.end() || a->second == b->second) {
            error = "authored joint body reference is invalid";
            return false;
        }
        const int index = definition.source_node;
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size()) {
            error = "authored joint source node is invalid";
            return false;
        }
        const auto& world = scene.nodes[index].world_stingray;
        Matrix4 inverse{};
        if (!invert_rigid_body_frame(world, inverse)) {
            error = "authored joint marker transform must be rigid: " + definition.id;
            return false;
        }
        if (definition.twist_min.has_value() != definition.twist_max.has_value()) {
            error = "authored joint requires both twist limits or neither";
            return false;
        }
        CollectionJoint joint;
        joint.name = definition.id;
        joint.body0 = static_cast<std::uint32_t>(a->second);
        joint.body1 = static_cast<std::uint32_t>(b->second);
        joint.local_frame0 = multiply(inverse_frames[a->second], world);
        joint.local_frame1 = multiply(inverse_frames[b->second], world);
        joint.motion.fill(CollectionMotion::Locked);
        switch (definition.kind) {
        case JointKind::Fixed:
            break;
        case JointKind::Hinge:
            joint.motion[3] = definition.twist_min ? CollectionMotion::Limited : CollectionMotion::Free;
            if (definition.twist_min) {
                joint.twist_lower = *definition.twist_min;
                joint.twist_upper = *definition.twist_max;
            }
            break;
        case JointKind::Ragdoll:
            joint.motion[3] = joint.motion[4] = joint.motion[5] = CollectionMotion::Limited;
            joint.twist_lower = definition.twist_min.value_or(-0.78539816339f);
            joint.twist_upper = definition.twist_max.value_or(0.78539816339f);
            joint.swing_y = definition.swing_y;
            joint.swing_z = definition.swing_z;
            break;
        }
        joints.push_back(joint);
    }
    return serialize_physics_scene(bodies, joints, bytes, error);
}
} // namespace dtglb::stingray::physics
#endif
