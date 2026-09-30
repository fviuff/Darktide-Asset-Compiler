#include "stingray/physics/authored_physics.h"

#include <map>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace dtglb::stingray::physics {
namespace {

bool finite_matrix(const Matrix4& m) {
    return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
}

bool inverse_rigid(const Matrix4& m, Matrix4& inverse) {
    if (!finite_matrix(m)) return false;
    const float x0=m[0], x1=m[1], x2=m[2], y0=m[4], y1=m[5], y2=m[6], z0=m[8], z1=m[9], z2=m[10];
    const float eps = 1e-4f;
    if (std::fabs(m[3]) > eps || std::fabs(m[7]) > eps || std::fabs(m[11]) > eps || std::fabs(m[15] - 1.f) > eps) return false;
    const float lx=x0*x0+x1*x1+x2*x2, ly=y0*y0+y1*y1+y2*y2, lz=z0*z0+z1*z1+z2*z2;
    const float d=x0*(y1*z2-y2*z1)-y0*(x1*z2-x2*z1)+z0*(x1*y2-x2*y1);
    if (std::fabs(lx-1.f)>eps || std::fabs(ly-1.f)>eps || std::fabs(lz-1.f)>eps ||
        std::fabs(x0*y0+x1*y1+x2*y2)>eps || std::fabs(x0*z0+x1*z1+x2*z2)>eps ||
        std::fabs(y0*z0+y1*z1+y2*z2)>eps || std::fabs(d-1.f)>eps) return false;
    inverse = {x0,y0,z0,0, x1,y1,z1,0, x2,y2,z2,0, 0,0,0,1};
    inverse[12] = -(x0*m[12] + x1*m[13] + x2*m[14]);
    inverse[13] = -(y0*m[12] + y1*m[13] + y2*m[14]);
    inverse[14] = -(z0*m[12] + z1*m[13] + z2*m[14]);
    return true;
}

void transform_point(const Matrix4& m, float& x, float& y, float& z) {
    const float a=x,b=y,c=z;
    x=m[0]*a+m[4]*b+m[8]*c+m[12]; y=m[1]*a+m[5]*b+m[9]*c+m[13]; z=m[2]*a+m[6]*b+m[10]*c+m[14];
}

void transform_vector(const Matrix4& m, float& x, float& y, float& z) {
    const float a=x,b=y,c=z;
    x=m[0]*a+m[4]*b+m[8]*c; y=m[1]*a+m[5]*b+m[9]*c; z=m[2]*a+m[6]*b+m[10]*c;
}

PhysicsShapeType shape_type(ColliderShape shape) {
    switch (shape) {
    case ColliderShape::Geometry: return PhysicsShapeType::TriangleMesh;
    case ColliderShape::Convex: return PhysicsShapeType::Convex;
    case ColliderShape::Box: return PhysicsShapeType::Box;
    case ColliderShape::Sphere: return PhysicsShapeType::Sphere;
    case ColliderShape::Capsule: return PhysicsShapeType::Capsule;
    }
    return PhysicsShapeType::TriangleMesh;
}

std::string group_name(const BodyDefinition& body, const ColliderDefinition& collider, int instance) {
    return body.id + "_" + collider.id + "_instance_" + std::to_string(instance);
}

} // namespace

bool invert_rigid_body_frame(const Matrix4& world, Matrix4& inverse) {
    return inverse_rigid(world, inverse);
}

bool localize_rigid_primitive(const Primitive& source, const Matrix4& body_world,
                              Primitive& out, std::string& error) {
    error.clear();
    Matrix4 inverse{};
    if (!inverse_rigid(body_world, inverse)) {
        error = "authored physics body transform must be a finite rigid transform";
        return false;
    }
    out = source;
    out.channels = source.channels;
    const VertexChannel* position = nullptr;
    for (auto& channel : out.channels) {
        if (channel.components == 0 || channel.values.size() % channel.components != 0 ||
            !std::all_of(channel.values.begin(), channel.values.end(), [](float v) { return std::isfinite(v); })) {
            error = "authored physics primitive contains invalid vertex channel data";
            return false;
        }
        const bool vector_channel = channel.semantic == VertexChannel::Semantic::Position ||
            channel.semantic == VertexChannel::Semantic::Normal || channel.semantic == VertexChannel::Semantic::Tangent ||
            channel.semantic == VertexChannel::Semantic::Binormal;
        if (vector_channel && channel.components < 3) { error = "authored physics vector channel has fewer than 3 components"; return false; }
        for (std::size_t i=0; i<channel.values.size(); i+=channel.components) {
            if (channel.semantic == VertexChannel::Semantic::Position) transform_point(inverse, channel.values[i], channel.values[i+1], channel.values[i+2]);
            else if (channel.semantic == VertexChannel::Semantic::Normal || channel.semantic == VertexChannel::Semantic::Tangent || channel.semantic == VertexChannel::Semantic::Binormal)
                transform_vector(inverse, channel.values[i], channel.values[i+1], channel.values[i+2]);
        }
        if (channel.semantic == VertexChannel::Semantic::Position) position = &channel;
    }
    if (!position || position->values.empty()) { error = "authored physics primitive has no POSITION channel"; return false; }
    out.bounds_min = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    out.bounds_max = {-out.bounds_min[0], -out.bounds_min[1], -out.bounds_min[2]};
    for (std::size_t i=0; i<position->values.size(); i+=position->components) for (int axis=0; axis<3; ++axis) {
        const float value=position->values[i+axis]; out.bounds_min[axis]=std::min(out.bounds_min[axis],value); out.bounds_max[axis]=std::max(out.bounds_max[axis],value);
    }
    double radius2=0; const std::array<float,3> center{(out.bounds_min[0]+out.bounds_max[0])*.5f,(out.bounds_min[1]+out.bounds_max[1])*.5f,(out.bounds_min[2]+out.bounds_max[2])*.5f};
    for (std::size_t i=0; i<position->values.size(); i+=position->components) { double dx=position->values[i]-center[0],dy=position->values[i+1]-center[1],dz=position->values[i+2]-center[2]; radius2=std::max(radius2,dx*dx+dy*dy+dz*dz); }
    out.bounds_radius=static_cast<float>(std::sqrt(radius2));
    if (!std::isfinite(out.bounds_radius)) { error="authored physics primitive bounds are non-finite"; return false; }
    return true;
}

bool build_authored_actor(const Scene& scene,
                          const std::string& root_node,
                          std::uint32_t root_hash,
                          PhysicsActor& actor,
                          std::string& error) {
    error.clear();
    actor = {};
    if (!scene.asset_definition.body) {
        error = "authored physics requires a body definition";
        return false;
    }
    const BodyDefinition& body = *scene.asset_definition.body;
    if (body.id.empty()) {
        error = "authored physics body id cannot be empty";
        return false;
    }
    if (scene.asset_definition.colliders.empty()) {
        error = "authored physics requires at least one collider definition";
        return false;
    }
    if (root_node.empty()) {
        error = "authored physics requires a generated UNIT root node";
        return false;
    }

    actor.name = body.id;
    actor.actor_template = body.actor;
    actor.node = root_node;
    actor.node_hash = root_hash;
    actor.mass = body.mass;
    actor.enabled = true;

    FittedActorOptions options;
    options.actor_template = body.actor;
    options.material = body.material;
    options.mass = body.mass;

    std::size_t matched_colliders = 0;
    for (const ColliderDefinition& collider : scene.asset_definition.colliders) {
        if (collider.body != body.id) {
            error = "authored collider '" + collider.id + "' has an invalid body reference";
            return false;
        }
        ++matched_colliders;

        std::map<int, std::vector<Primitive>> instances;
        for (const Primitive& primitive : scene.collider_primitives) {
            if (primitive.collision_object != collider.source_node) continue;
            // Unassigned instances must remain independent, matching the
            // existing builder's metadata-less grouping behavior.
            const int key = primitive.collision_instance >= 0
                ? primitive.collision_instance
                : static_cast<int>(instances.size()) * -1 - 1;
            instances[key].push_back(primitive);
        }
        if (instances.empty()) {
            error = "authored collider '" + collider.id + "' has no emitted geometry for source node " +
                    std::to_string(collider.source_node);
            return false;
        }

        options.shape = shape_type(collider.shape);
        for (const auto& [instance, primitives] : instances) {
            PhysicsActor part;
            if (!build_actor(primitives, group_name(body, collider, instance), root_node, root_hash,
                             options, part, error)) {
                return false;
            }
            for (auto& shape : part.shapes) {
                shape.node = root_node;
                shape.node_hash = root_hash;
                actor.shapes.push_back(std::move(shape));
            }
        }
    }
    if (matched_colliders == 0) {
        error = "authored physics has no collider bound to body '" + body.id + "'";
        return false;
    }
    if (actor.shapes.empty()) {
        error = "authored physics produced no collision shapes for body '" + body.id + "'";
        return false;
    }
    return true;
}

} // namespace dtglb::stingray::physics
