#include "stingray/physics/physics_collection.h"

#include "stingray/murmur_hash.h"
#include "stingray/physics/physx_context.h"

#include <extensions/PxCollectionExt.h>
#include <extensions/PxExtensionsAPI.h>
#include <extensions/PxRigidActorExt.h>
#include <extensions/PxRigidBodyExt.h>
#include <extensions/PxSerialization.h>
#include <PxPhysicsAPI.h>

#include <algorithm>
#include <sstream>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace dtglb::stingray::physics {
namespace {

using namespace physx;

bool finite_matrix(const Matrix4& m) {
    for (float v : m) if (!std::isfinite(v)) return false;
    const PxVec3 x(m[0], m[1], m[2]), y(m[4], m[5], m[6]), z(m[8], m[9], m[10]);
    const float determinant = x.dot(y.cross(z));
    return std::fabs(m[3]) < 1e-5f && std::fabs(m[7]) < 1e-5f &&
           std::fabs(m[11]) < 1e-5f && std::fabs(m[15] - 1.0f) < 1e-5f &&
           std::fabs(determinant - 1.0f) < 1e-3f &&
           std::fabs(x.magnitudeSquared() - 1.0f) < 1e-3f &&
           std::fabs(y.magnitudeSquared() - 1.0f) < 1e-3f &&
           std::fabs(z.magnitudeSquared() - 1.0f) < 1e-3f &&
           std::fabs(x.dot(y)) < 1e-3f && std::fabs(x.dot(z)) < 1e-3f && std::fabs(y.dot(z)) < 1e-3f;
}

PxTransform transform(const Matrix4& m) {
    const PxMat33 r(PxVec3(m[0], m[1], m[2]), PxVec3(m[4], m[5], m[6]), PxVec3(m[8], m[9], m[10]));
    return PxTransform(PxVec3(m[12], m[13], m[14]), PxQuat(r));
}

bool valid_limits(const CollectionJoint& j) {
    return std::isfinite(j.linear_lower) && std::isfinite(j.linear_upper) && j.linear_lower <= j.linear_upper &&
           std::isfinite(j.drive_stiffness) && std::isfinite(j.drive_damping) && j.drive_stiffness >= 0.0f &&
           j.drive_damping >= 0.0f && std::isfinite(j.break_force) && std::isfinite(j.break_torque) &&
           j.break_force >= 0.0f && j.break_torque >= 0.0f &&
           std::isfinite(j.twist_lower) && std::isfinite(j.twist_upper) &&
           std::isfinite(j.swing_y) && std::isfinite(j.swing_z) &&
           j.twist_lower <= j.twist_upper && j.twist_lower > -PxTwoPi && j.twist_upper < PxTwoPi &&
           j.swing_y > 0.0f && j.swing_z > 0.0f && j.swing_y < PxPi && j.swing_z < PxPi;
}

PxD6Motion::Enum to_motion(CollectionMotion m) {
    switch (m) { case CollectionMotion::Free: return PxD6Motion::eFREE;
                 case CollectionMotion::Limited: return PxD6Motion::eLIMITED;
                 default: return PxD6Motion::eLOCKED; }
}

struct OutputStream final : PxOutputStream {
    std::vector<std::uint8_t> bytes;
    PxU32 write(const void* data, PxU32 count) override {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + count);
        return count;
    }
};

struct AlignedBuffer {
    void* data = nullptr;
    explicit AlignedBuffer(std::size_t size) {
        const std::size_t padded = std::max<std::size_t>(128, (size + 127u) & ~std::size_t(127u));
#if defined(_MSC_VER)
        data = _aligned_malloc(padded, 128);
#else
        data = std::aligned_alloc(128, padded);
#endif
    }
    ~AlignedBuffer() {
#if defined(_MSC_VER)
        _aligned_free(data);
#else
        std::free(data);
#endif
    }
    AlignedBuffer(const AlignedBuffer&) = delete;
};

struct SdkObjects {
    PxPhysics* physics = nullptr;
    bool extensions = false;
    PxSerializationRegistry* registry = nullptr;
    PxCollection* dependencies = nullptr;
    PxCollection* objects = nullptr;
    std::vector<PxJoint*> joints;
    std::vector<PxRigidActor*> actors;
    std::vector<PxTriangleMesh*> triangle_meshes;
    std::vector<PxConvexMesh*> convex_meshes;
    std::vector<PxMaterial*> materials;

    void clear() {
        if (objects) { objects->release(); objects = nullptr; }
        if (dependencies) { dependencies->release(); dependencies = nullptr; }
        for (auto* joint : joints) joint->release();
        joints.clear();
        // Exclusive shapes belong to their actors. Release creator references
        // to cooked meshes/materials only after all actors release their shapes.
        for (auto* actor : actors) actor->release();
        actors.clear();
        for (auto* mesh : triangle_meshes) mesh->release();
        triangle_meshes.clear();
        for (auto* mesh : convex_meshes) mesh->release();
        convex_meshes.clear();
        for (auto* material : materials) material->release();
        materials.clear();
        if (registry) { registry->release(); registry = nullptr; }
        if (extensions) { PxCloseExtensions(); extensions = false; }
        if (physics) { physics->release(); physics = nullptr; }
    }
    ~SdkObjects() { clear(); }
};

struct ReloadedCollections {
    PxCollection* dependencies = nullptr;
    PxCollection* objects = nullptr;
    ~ReloadedCollections() {
        if (objects) { PxCollectionExt::releaseObjects(*objects); objects->release(); }
        if (dependencies) { PxCollectionExt::releaseObjects(*dependencies); dependencies->release(); }
    }
};

std::unordered_map<PxSerialObjectId, std::uint32_t> collection_ordinals(const PxCollection& collection) {
    std::unordered_map<PxSerialObjectId, std::uint32_t> result;
    result.reserve(collection.getNbObjects());
    for (PxU32 i = 0; i < collection.getNbObjects(); ++i) {
        const PxBase& object = collection.getObject(i);
        result.emplace(collection.getId(object), i);
    }
    return result;
}

} // namespace

bool serialize_physics_collections(const std::vector<CollectionBody>& bodies,
                                   const std::vector<CollectionJoint>& joints,
                                   PhysicsCollections& output,
                                   std::string& error) {
    output = {};
    error.clear();
    if (bodies.empty()) { error = "physics collection requires at least one body"; return false; }
    for (const auto& body : bodies) {
        if (!finite_matrix(body.world_transform)) { error = "body transform is not a finite rigid transform"; return false; }
        if (body.actor.actor_template != "static" && body.actor.actor_template != "dynamic" && body.actor.actor_template != "keyframed") {
            error = "unsupported actor template: " + body.actor.actor_template; return false;
        }
        if (body.actor.actor_template == "dynamic" && !(body.actor.mass > 0.0f) ) { error = "dynamic body mass must be positive"; return false; }
        if (!std::isfinite(body.actor.mass) || body.actor.mass < 0) { error = "body mass must be finite and nonnegative"; return false; }
        if (body.actor.shapes.empty()) { error = "physics body has no shapes"; return false; }
        for (const auto& shape : body.actor.shapes) {
            if (!finite_matrix(shape.local_transform)) { error = "shape transform is not a finite rigid transform"; return false; }
            if (!std::isfinite(shape.radius) || !std::isfinite(shape.height) ||
                !std::all_of(shape.half_extents.begin(), shape.half_extents.end(), [](float v) { return std::isfinite(v); })) {
                error = "shape dimensions are not finite"; return false;
            }
        }
    }
    for (const auto& joint : joints) {
        if (joint.body0 >= bodies.size() || joint.body1 >= bodies.size() || joint.body0 == joint.body1) { error = "joint body reference is invalid"; return false; }
        if (!finite_matrix(joint.local_frame0) || !finite_matrix(joint.local_frame1) || !valid_limits(joint)) { error = "joint frame or limits are invalid"; return false; }
        for (const auto motion : joint.motion)
            if (motion != CollectionMotion::Locked && motion != CollectionMotion::Limited && motion != CollectionMotion::Free) {
                error = "unknown joint motion mode";
                return false;
            }
        if (joint.motion[1] == CollectionMotion::Limited || joint.motion[2] == CollectionMotion::Limited) { error = "only the x axis takes linear limits"; return false; }
    }

    std::lock_guard<std::recursive_mutex> guard(physx_context_mutex());
    PxFoundation* foundation = physics_foundation(error);
    if (!foundation) return false;
    physics_clear_errors();
    PxTolerancesScale scale;
    SdkObjects resources;
    PxPhysics* physics = resources.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale, true, nullptr);
    if (!physics) { error = "PxCreatePhysics failed"; return false; }
    const bool extensions = resources.extensions = PxInitExtensions(*physics, nullptr);
    PxSerializationRegistry* registry = resources.registry = extensions ? PxSerialization::createSerializationRegistry(*physics) : nullptr;
    if (!extensions || !registry) {
        error = "PhysX extensions or serialization registry initialization failed";
        return false;
    }

    PxCollection* dependencies = resources.dependencies = PxCreateCollection();
    PxCollection* objects = resources.objects = PxCreateCollection();
    auto& materials = resources.materials;
    std::unordered_map<std::string, std::size_t> material_map;
    auto& actors = resources.actors;
    auto& joints_owned = resources.joints;
    auto& triangle_meshes = resources.triangle_meshes;
    auto& convex_meshes = resources.convex_meshes;
    std::size_t shape_count = 0;
    for (const auto& body : bodies) shape_count += body.actor.shapes.size();
    // Reserve owners before creating objects, so tracking a fresh SDK pointer
    // cannot throw and strand it between construction and ownership.
    materials.reserve(shape_count);
    actors.reserve(bodies.size());
    joints_owned.reserve(joints.size());
    triangle_meshes.reserve(shape_count);
    convex_meshes.reserve(shape_count);
    PxSerialObjectId next_id = 1;
    const auto new_id = [&]() { return next_id++; };
    bool ok = dependencies && objects;
    auto fail = [&](const std::string& why) { if (error.empty()) error = why; ok = false; };
    if (!ok) fail("PxCreateCollection failed");
    auto material_for = [&](const std::string& name) -> PxMaterial* {
        auto it = material_map.find(name); if (it != material_map.end()) return materials[it->second];
        PxMaterial* m = physics->createMaterial(.6f, .6f, .1f); if (!m) return nullptr;
        const std::size_t n = materials.size(); const PxSerialObjectId id = new_id();
        materials.push_back(m); material_map.emplace(name, n); dependencies->add(*m, id);
        output.materials.push_back({static_cast<std::uint32_t>(id), id32_from_id64(name)}); return m;
    };
    if (ok) for (std::size_t i = 0; i < bodies.size(); ++i) {
        const auto& source = bodies[i]; const PxTransform pose = transform(source.world_transform);
        PxRigidActor* actor = source.actor.actor_template == "static" ? static_cast<PxRigidActor*>(physics->createRigidStatic(pose)) : static_cast<PxRigidActor*>(physics->createRigidDynamic(pose));
        if (!actor) { fail("failed to create body actor"); break; }
        actor->setName(source.actor.name.c_str()); actors.push_back(actor);
        const PxSerialObjectId body_id = new_id(); objects->add(*actor, body_id);
        output.body_object_indices.push_back(static_cast<std::uint32_t>(body_id));
        if (auto* dynamic = actor->is<PxRigidDynamic>())
            if (source.actor.actor_template == "keyframed") dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        for (std::size_t si = 0; si < source.actor.shapes.size(); ++si) {
            const auto& source_shape = source.actor.shapes[si]; PxMaterial* material = material_for(source_shape.material); if (!material) { fail("failed to create physics material"); break; }
            PxGeometryHolder geometry;
            PxTriangleMesh* triangle = nullptr; PxConvexMesh* convex = nullptr;
            if (source_shape.type == PhysicsShapeType::Box) {
                if (source_shape.half_extents[0] <= 0 || source_shape.half_extents[1] <= 0 || source_shape.half_extents[2] <= 0) { fail("box shape has invalid half extents"); break; }
                geometry.storeAny(PxBoxGeometry(source_shape.half_extents[0], source_shape.half_extents[1], source_shape.half_extents[2]));
            }
            else if (source_shape.type == PhysicsShapeType::Sphere) {
                if (!(source_shape.radius > 0)) { fail("sphere shape has invalid radius"); break; }
                geometry.storeAny(PxSphereGeometry(source_shape.radius));
            }
            else if (source_shape.type == PhysicsShapeType::Capsule) {
                if (!(source_shape.radius > 0) || source_shape.height < 0) { fail("capsule shape has invalid dimensions"); break; }
                geometry.storeAny(PxCapsuleGeometry(source_shape.radius, source_shape.height * .5f));
            }
            else if (source_shape.type == PhysicsShapeType::TriangleMesh) {
                if (source.actor.actor_template == "dynamic") { fail("dynamic triangle-mesh actors require convex geometry"); break; }
                if (source_shape.physx_cooked.empty() || source_shape.physx_cooked.size() > std::numeric_limits<PxU32>::max()) { fail("cooked triangle data is empty or exceeds SDK limits"); break; }
                std::vector<PxU8> cooked(source_shape.physx_cooked.begin(), source_shape.physx_cooked.end());
                PxDefaultMemoryInputData input(cooked.data(), static_cast<PxU32>(cooked.size()));
                triangle = physics->createTriangleMesh(input);
                if (!triangle) { fail("failed to load cooked triangle mesh"); break; }
                triangle_meshes.push_back(triangle);
                geometry.storeAny(PxTriangleMeshGeometry(triangle));
            }
            else if (source_shape.type == PhysicsShapeType::Convex) {
                if (source_shape.physx_cooked.empty() || source_shape.physx_cooked.size() > std::numeric_limits<PxU32>::max()) { fail("cooked convex data is empty or exceeds SDK limits"); break; }
                std::vector<PxU8> cooked(source_shape.physx_cooked.begin(), source_shape.physx_cooked.end());
                PxDefaultMemoryInputData input(cooked.data(), static_cast<PxU32>(cooked.size()));
                convex = physics->createConvexMesh(input);
                if (!convex) { fail("failed to load cooked convex mesh"); break; }
                convex_meshes.push_back(convex);
                geometry.storeAny(PxConvexMeshGeometry(convex));
            }
            else { fail("unsupported physics shape type"); break; }
            PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry.any(), *material);
            if (!shape) { fail("failed to create physics shape"); break; }
            shape->setName(source_shape.name.c_str()); shape->setLocalPose(transform(source_shape.local_transform));
            const PxSerialObjectId shape_id = new_id(); objects->add(*shape, shape_id);
            output.shapes.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(si), static_cast<std::uint32_t>(shape_id),
                shape_template_id32(source.actor.actor_template, source_shape.shape_template, true)});
            if (triangle) dependencies->add(*triangle, new_id());
            if (convex) dependencies->add(*convex, new_id());
        }
        if (!ok) break;
        if (auto* dynamic = actor->is<PxRigidDynamic>()) {
            if (source.actor.actor_template == "keyframed") {
                // Kinematic motion is supplied externally. PhysX cannot derive
                // mass properties from triangle meshes, which are valid here.
                dynamic->setMass(1.0f);
                dynamic->setMassSpaceInertiaTensor(PxVec3(1.0f));
            } else if (!PxRigidBodyExt::setMassAndUpdateInertia(*dynamic, source.actor.mass)) {
                fail("failed to compute body mass and inertia");
                break;
            }
        }
    }
    for (std::size_t ji = 0; ok && ji < joints.size(); ++ji) {
        const auto& source = joints[ji]; auto* joint = PxD6JointCreate(*physics, actors[source.body0], transform(source.local_frame0), actors[source.body1], transform(source.local_frame1));
        if (!joint) { fail("failed to create D6 joint"); break; } joint->setName(source.name.c_str());
        for (PxD6Axis::Enum axis : {PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ, PxD6Axis::eTWIST, PxD6Axis::eSWING1, PxD6Axis::eSWING2}) joint->setMotion(axis, to_motion(source.motion[static_cast<std::size_t>(axis)]));
        joint->setTwistLimit(PxJointAngularLimitPair(source.twist_lower, source.twist_upper)); joint->setSwingLimit(PxJointLimitCone(source.swing_y, source.swing_z));
        if (source.motion[0] == CollectionMotion::Limited)
            joint->setLinearLimit(PxD6Axis::eX, PxJointLinearLimitPair(physics->getTolerancesScale(), source.linear_lower, source.linear_upper));
        if (source.drive != CollectionJoint::Drive::None && (source.drive_stiffness > 0.0f || source.drive_damping > 0.0f)) {
            const PxD6JointDrive drive(source.drive_stiffness, source.drive_damping, PX_MAX_F32);
            if (source.drive == CollectionJoint::Drive::X) joint->setDrive(PxD6Drive::eX, drive);
            else joint->setDrive(PxD6Drive::eTWIST, drive);
            if (source.drive == CollectionJoint::Drive::SwingTwist) joint->setDrive(PxD6Drive::eSWING, drive);
        }
        if (source.break_force > 0.0f || source.break_torque > 0.0f)
            joint->setBreakForce(source.break_force > 0.0f ? source.break_force : PX_MAX_F32,
                                 source.break_torque > 0.0f ? source.break_torque : PX_MAX_F32);
        joint->setConstraintFlag(PxConstraintFlag::eCOLLISION_ENABLED, source.collision_enabled); objects->add(*joint, new_id()); joints_owned.push_back(joint);
    }
    if (ok) {
        PxSerialization::complete(*dependencies, *registry, nullptr, true);
        PxSerialization::complete(*objects, *registry, dependencies, true);
        OutputStream dep, obj;
        ok = PxSerialization::isSerializable(*dependencies, *registry) &&
             PxSerialization::isSerializable(*objects, *registry, dependencies) &&
             PxSerialization::serializeCollectionToBinaryDeterministic(dep, *dependencies, *registry, nullptr, true) &&
             PxSerialization::serializeCollectionToBinaryDeterministic(obj, *objects, *registry, dependencies, true);
        if (ok) {
            AlignedBuffer dep_memory(dep.bytes.size()), obj_memory(obj.bytes.size());
            if (!dep_memory.data || !obj_memory.data) { fail("aligned collection reload buffer allocation failed"); }
            else {
                std::memcpy(dep_memory.data, dep.bytes.data(), dep.bytes.size());
                std::memcpy(obj_memory.data, obj.bytes.data(), obj.bytes.size());
                ReloadedCollections loaded;
                PxCollection* loaded_dependencies = loaded.dependencies = PxSerialization::createCollectionFromBinary(dep_memory.data, *registry);
                PxCollection* loaded_objects = loaded.objects = loaded_dependencies ? PxSerialization::createCollectionFromBinary(obj_memory.data, *registry, loaded_dependencies) : nullptr;
                if (!loaded_dependencies || !loaded_objects) {
                    fail("serialized physics collections could not be reloaded for metadata mapping");
                } else {
                    const auto dependency_indices = collection_ordinals(*loaded_dependencies);
                    const auto object_indices = collection_ordinals(*loaded_objects);
                    auto dependency_index = [&](std::uint32_t id, std::uint32_t& ordinal) {
                        const auto it = dependency_indices.find(id); if (it == dependency_indices.end()) return false; ordinal = it->second; return true;
                    };
                    auto object_index = [&](std::uint32_t id, std::uint32_t& ordinal) {
                        const auto it = object_indices.find(id); if (it == object_indices.end()) return false; ordinal = it->second; return true;
                    };
                    for (auto& index : output.body_object_indices) { std::uint32_t ordinal = 0; if (!object_index(index, ordinal)) { fail("body object ID missing after collection reload"); break; } index = ordinal; }
                    for (auto& binding : output.shapes) { std::uint32_t ordinal = 0; if (!object_index(binding.object_index, ordinal)) { fail("shape object ID missing after collection reload"); break; } binding.object_index = ordinal; }
                    for (auto& binding : output.materials) { std::uint32_t ordinal = 0; if (!dependency_index(binding.object_index, ordinal)) { fail("material object ID missing after collection reload"); break; } binding.object_index = ordinal; }
                }
            }
            if (ok) { output.dependencies = std::move(dep.bytes); output.objects = std::move(obj.bytes); }
        } else fail("PhysX collection serialization failed");
    }
    resources.clear();
    if (!physics_last_error().empty() && ok) { error = physics_last_error(); ok = false; }
    if (!ok) output = {};
    return ok;
}

bool describe_physics_joints(const std::vector<std::uint8_t>& dependencies, const std::vector<std::uint8_t>& objects,
                             std::string& out, std::string& error) {
    std::lock_guard<std::recursive_mutex> guard(physx_context_mutex());
    PxFoundation* foundation = physics_foundation(error);
    if (!foundation) return false;
    PxTolerancesScale scale;
    SdkObjects resources;
    PxPhysics* physics = resources.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale, true, nullptr);
    if (!physics) { error = "PxCreatePhysics failed"; return false; }
    resources.extensions = PxInitExtensions(*physics, nullptr);
    PxSerializationRegistry* registry = resources.registry = resources.extensions ? PxSerialization::createSerializationRegistry(*physics) : nullptr;
    if (!registry) { error = "PhysX serialization registry initialization failed"; return false; }
    AlignedBuffer dep_memory(dependencies.size()), obj_memory(objects.size());
    std::memcpy(dep_memory.data, dependencies.data(), dependencies.size());
    std::memcpy(obj_memory.data, objects.data(), objects.size());
    ReloadedCollections loaded;
    loaded.dependencies = PxSerialization::createCollectionFromBinary(dep_memory.data, *registry);
    loaded.objects = loaded.dependencies ? PxSerialization::createCollectionFromBinary(obj_memory.data, *registry, loaded.dependencies) : nullptr;
    if (!loaded.objects) { error = "the physics collections do not load"; return false; }
    static const char* motions[] = {"locked", "limited", "free"};
    static const char* axes[] = {"x", "y", "z", "twist", "swing1", "swing2"};
    static const char* drives[] = {"x", "y", "z", "swing", "twist", "slerp"};
    std::ostringstream text;
    for (PxU32 i = 0; i < loaded.objects->getNbObjects(); ++i) {
        auto* joint = loaded.objects->getObject(i).is<PxD6Joint>();
        if (!joint) continue;
        text << "joint " << (joint->getName() ? joint->getName() : "") << ":";
        for (int axis = 0; axis < 6; ++axis)
            text << " " << axes[axis] << "=" << motions[joint->getMotion(static_cast<PxD6Axis::Enum>(axis))];
        const auto twist = joint->getTwistLimit();
        const auto swing = joint->getSwingLimit();
        const auto linear = joint->getLinearLimit(PxD6Axis::eX);
        text << " twist " << twist.lower << ".." << twist.upper << " swing " << swing.yAngle << "/" << swing.zAngle
             << " x " << linear.lower << ".." << linear.upper;
        for (int d = 0; d < 6; ++d) {
            const auto drive = joint->getDrive(static_cast<PxD6Drive::Enum>(d));
            if (drive.stiffness > 0.0f || drive.damping > 0.0f)
                text << " drive " << drives[d] << " " << drive.stiffness << "/" << drive.damping;
        }
        PxReal force = 0, torque = 0;
        joint->getBreakForce(force, torque);
        if (force < PX_MAX_F32 || torque < PX_MAX_F32) text << " breaks " << force << "/" << torque;
        text << '\n';
    }
    out = text.str();
    return true;
}

bool describe_cooked_meshes(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& meshes,
                            std::string& out, std::string& error) {
    std::lock_guard<std::recursive_mutex> guard(physx_context_mutex());
    PxFoundation* foundation = physics_foundation(error);
    if (!foundation) return false;
    PxTolerancesScale scale;
    SdkObjects resources;
    PxPhysics* physics = resources.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale, true, nullptr);
    if (!physics) { error = "PxCreatePhysics failed"; return false; }
    std::ostringstream text;
    text.precision(9);
    text << "[";
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        const auto& [type, cooked] = meshes[m];
        std::vector<std::uint8_t> bytes = cooked;
        PxDefaultMemoryInputData input(bytes.data(), static_cast<PxU32>(bytes.size()));
        text << (m ? "," : "") << "{\"vertices\":[";
        if (type == 4) {
            PxConvexMesh* mesh = physics->createConvexMesh(input);
            if (!mesh) { error = "a cooked convex mesh does not load"; return false; }
            resources.convex_meshes.push_back(mesh);
            const PxVec3* vertices = mesh->getVertices();
            for (PxU32 v = 0; v < mesh->getNbVertices(); ++v)
                text << (v ? "," : "") << vertices[v].x << "," << vertices[v].y << "," << vertices[v].z;
            text << "],\"triangles\":[";
            const PxU8* indices = mesh->getIndexBuffer();
            bool first = true;
            for (PxU32 polygon = 0; polygon < mesh->getNbPolygons(); ++polygon) {
                PxHullPolygon data;
                mesh->getPolygonData(polygon, data);
                for (PxU16 corner = 2; corner < data.mNbVerts; ++corner) {
                    text << (first ? "" : ",") << int(indices[data.mIndexBase]) << "," << int(indices[data.mIndexBase + corner - 1])
                         << "," << int(indices[data.mIndexBase + corner]);
                    first = false;
                }
            }
        } else if (type == 3) {
            PxTriangleMesh* mesh = physics->createTriangleMesh(input);
            if (!mesh) { error = "a cooked triangle mesh does not load"; return false; }
            resources.triangle_meshes.push_back(mesh);
            const PxVec3* vertices = mesh->getVertices();
            for (PxU32 v = 0; v < mesh->getNbVertices(); ++v)
                text << (v ? "," : "") << vertices[v].x << "," << vertices[v].y << "," << vertices[v].z;
            text << "],\"triangles\":[";
            const bool wide = !mesh->getTriangleMeshFlags().isSet(PxTriangleMeshFlag::e16_BIT_INDICES);
            const void* triangles = mesh->getTriangles();
            for (PxU32 i = 0; i < mesh->getNbTriangles() * 3; ++i)
                text << (i ? "," : "") << (wide ? static_cast<const PxU32*>(triangles)[i] : static_cast<const PxU16*>(triangles)[i]);
        } else {
            error = "cooked mesh type must be 3 (triangles) or 4 (convex)";
            return false;
        }
        text << "]}";
    }
    text << "]\n";
    out = text.str();
    return true;
}

bool describe_physics_collection(const std::vector<std::uint8_t>& scene, std::string& out, std::string& error) {
    if (scene.size() < 128) { error = "the physics scene is truncated"; return false; }
    const std::size_t header = scene.size() - 128;
    const auto word = [&](std::size_t offset) { std::uint32_t v; std::memcpy(&v, scene.data() + offset, 4); return v; };
    const auto slice = [&](std::uint32_t offset, std::uint32_t length) {
        return offset <= scene.size() && length <= scene.size() - offset
            ? std::vector<std::uint8_t>(scene.data() + offset, scene.data() + offset + length) : std::vector<std::uint8_t>();
    };
    const auto dependencies = slice(word(header + 8), word(header + 12));
    const auto objects = slice(word(header + 80), word(header + 84));
    const std::uint32_t body_offset = word(header + 88), body_count = word(header + 92);
    const std::uint32_t template_offset = word(header + 112), template_count = word(header + 116);
    if (dependencies.empty() || objects.empty() || std::uint64_t(body_offset) + body_count * 72ull > header ||
        std::uint64_t(template_offset) + template_count * 8ull > header) {
        error = "the physics scene header does not match";
        return false;
    }
    std::lock_guard<std::recursive_mutex> guard(physx_context_mutex());
    PxFoundation* foundation = physics_foundation(error);
    if (!foundation) return false;
    PxTolerancesScale scale;
    SdkObjects resources;
    PxPhysics* physics = resources.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale, true, nullptr);
    if (!physics) { error = "PxCreatePhysics failed"; return false; }
    resources.extensions = PxInitExtensions(*physics, nullptr);
    PxSerializationRegistry* registry = resources.registry = resources.extensions ? PxSerialization::createSerializationRegistry(*physics) : nullptr;
    if (!registry) { error = "PhysX serialization registry initialization failed"; return false; }
    AlignedBuffer dep_memory(dependencies.size()), obj_memory(objects.size());
    std::memcpy(dep_memory.data, dependencies.data(), dependencies.size());
    std::memcpy(obj_memory.data, objects.data(), objects.size());
    ReloadedCollections loaded;
    loaded.dependencies = PxSerialization::createCollectionFromBinary(dep_memory.data, *registry);
    loaded.objects = loaded.dependencies ? PxSerialization::createCollectionFromBinary(obj_memory.data, *registry, loaded.dependencies) : nullptr;
    if (!loaded.objects) { error = "the physics collections do not load"; return false; }

    std::unordered_map<const PxBase*, std::uint32_t> shape_templates;
    for (std::uint32_t i = 0; i < template_count; ++i) {
        const auto index = word(template_offset + i * 8);
        if (index < loaded.objects->getNbObjects()) shape_templates[&loaded.objects->getObject(index)] = word(template_offset + i * 8 + 4);
    }
    const auto pose = [](std::ostringstream& text, const PxTransform& t) {
        text << "{\"p\":[" << t.p.x << "," << t.p.y << "," << t.p.z << "],\"q\":[" << t.q.x << "," << t.q.y << "," << t.q.z
             << "," << t.q.w << "]}";
    };
    std::unordered_map<const PxRigidActor*, std::uint32_t> body_index;
    std::ostringstream text;
    text.precision(9);
    text << "{\"bodies\":[";
    for (std::uint32_t b = 0; b < body_count; ++b) {
        const std::size_t record = body_offset + b * 72u;
        const auto object = word(record);
        auto* actor = object < loaded.objects->getNbObjects() ? loaded.objects->getObject(object).is<PxRigidActor>() : nullptr;
        if (!actor) { error = "a physics body record names no rigid actor"; return false; }
        body_index[actor] = b;
        auto* rigid = actor->is<PxRigidDynamic>();
        text << (b ? "," : "") << "{\"name\":" << word(record + 4) << ",\"node\":" << word(record + 8)
             << ",\"enabled\":" << word(record + 12) << ",\"dynamic\":" << (rigid ? "true" : "false")
             << ",\"kinematic\":" << (rigid && rigid->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC) ? "true" : "false")
             << ",\"mass\":" << (rigid ? rigid->getMass() : 0.0f) << ",\"pose\":";
        pose(text, actor->getGlobalPose());
        text << ",\"shapes\":[";
        std::vector<PxShape*> shapes(actor->getNbShapes());
        actor->getShapes(shapes.data(), static_cast<PxU32>(shapes.size()));
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            const PxGeometryHolder geometry = shapes[s]->getGeometry();
            text << (s ? "," : "") << "{\"type\":";
            switch (geometry.getType()) {
            case PxGeometryType::eSPHERE: text << "\"sphere\",\"radius\":" << geometry.sphere().radius; break;
            case PxGeometryType::eCAPSULE:
                text << "\"capsule\",\"radius\":" << geometry.capsule().radius << ",\"half_height\":" << geometry.capsule().halfHeight; break;
            case PxGeometryType::eBOX: {
                const auto h = geometry.box().halfExtents;
                text << "\"box\",\"half_extents\":[" << h.x << "," << h.y << "," << h.z << "]"; break;
            }
            case PxGeometryType::eCONVEXMESH: {
                // the hull as triangles (polygon fans), scaled as the shape uses it
                const auto& convex = geometry.convexMesh();
                const PxConvexMesh* mesh = convex.convexMesh;
                const PxVec3* vertices = mesh->getVertices();
                const PxU8* indices = mesh->getIndexBuffer();
                text << "\"convex\",\"vertices\":[";
                for (PxU32 v = 0; v < mesh->getNbVertices(); ++v) {
                    const PxVec3 p = convex.scale.toMat33() * vertices[v];
                    text << (v ? "," : "") << p.x << "," << p.y << "," << p.z;
                }
                text << "],\"triangles\":[";
                bool first_index = true;
                for (PxU32 polygon = 0; polygon < mesh->getNbPolygons(); ++polygon) {
                    PxHullPolygon data;
                    mesh->getPolygonData(polygon, data);
                    for (PxU16 corner = 2; corner < data.mNbVerts; ++corner) {
                        text << (first_index ? "" : ",") << int(indices[data.mIndexBase]) << ","
                             << int(indices[data.mIndexBase + corner - 1]) << "," << int(indices[data.mIndexBase + corner]);
                        first_index = false;
                    }
                }
                text << "]";
                break;
            }
            case PxGeometryType::eTRIANGLEMESH: text << "\"geometry\""; break;
            default: text << "\"other\""; break;
            }
            const auto found = shape_templates.find(shapes[s]);
            text << ",\"shape_template\":" << (found == shape_templates.end() ? 0u : found->second) << ",\"local\":";
            pose(text, shapes[s]->getLocalPose());
            text << "}";
        }
        text << "]}";
    }
    text << "],\"joints\":[";
    static const char* motions[] = {"locked", "limited", "free"};
    bool first = true;
    for (PxU32 i = 0; i < loaded.objects->getNbObjects(); ++i) {
        auto* joint = loaded.objects->getObject(i).is<PxD6Joint>();
        if (!joint) continue;
        PxRigidActor *a0 = nullptr, *a1 = nullptr;
        joint->getActors(a0, a1);
        const auto i0 = body_index.find(a0), i1 = body_index.find(a1);
        text << (first ? "" : ",") << "{\"body0\":" << (i0 == body_index.end() ? -1 : static_cast<int>(i0->second))
             << ",\"body1\":" << (i1 == body_index.end() ? -1 : static_cast<int>(i1->second)) << ",\"frame0\":";
        first = false;
        pose(text, joint->getLocalPose(PxJointActorIndex::eACTOR0));
        text << ",\"frame1\":";
        pose(text, joint->getLocalPose(PxJointActorIndex::eACTOR1));
        text << ",\"motion\":[";
        for (int axis = 0; axis < 6; ++axis)
            text << (axis ? "," : "") << "\"" << motions[joint->getMotion(static_cast<PxD6Axis::Enum>(axis))] << "\"";
        const auto twist = joint->getTwistLimit();
        const auto swing = joint->getSwingLimit();
        const auto swing_drive = joint->getDrive(PxD6Drive::eSWING), twist_drive = joint->getDrive(PxD6Drive::eTWIST);
        PxReal force = 0, torque = 0;
        joint->getBreakForce(force, torque);
        text << "],\"twist\":[" << twist.lower << "," << twist.upper << "],\"swing\":[" << swing.yAngle << "," << swing.zAngle
             << "],\"swing_drive\":[" << swing_drive.stiffness << "," << swing_drive.damping << "],\"twist_drive\":["
             << twist_drive.stiffness << "," << twist_drive.damping << "],\"break\":[" << force << "," << torque
             << "],\"collision\":" << (joint->getConstraintFlags().isSet(PxConstraintFlag::eCOLLISION_ENABLED) ? "true" : "false")
             << "}";
    }
    text << "]}\n";
    out = text.str();
    return true;
}

} // namespace dtglb::stingray::physics
