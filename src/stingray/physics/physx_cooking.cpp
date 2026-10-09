#include "stingray/physics/physx_cooking.h"
#include "stingray/physics/physx_context.h"
#include "stingray/binary_writer.h"
#include "stingray/murmur_hash.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

#if __has_include(<PxFoundation.h>)
#define DTGLB_HAS_PHYSX 1
#include <PxFoundation.h>
#include <PxPhysicsVersion.h>
#include <cooking/PxConvexMeshDesc.h>
#include <cooking/PxCooking.h>
#include <cooking/PxTriangleMeshDesc.h>
#include <foundation/PxAllocatorCallback.h>
#include <foundation/PxErrorCallback.h>
#include <foundation/PxIO.h>
#else
#define DTGLB_HAS_PHYSX 0
#endif

namespace dtglb::stingray::physics {
namespace {

using Vec3 = std::array<float, 3>;

const VertexChannel* positions(const Primitive& p) {
    // UNIT rendering consumes these canonical, scaled Stingray-space channels.
    // Collision must use the same space or it diverges when a node transform or
    // asset-scale conversion has been applied.
    for (const auto& c : p.channels) {
        if (c.semantic == VertexChannel::Semantic::Position && c.components >= 3) return &c;
    }
    return nullptr;
}

struct LocalMesh {
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
};

bool finite3(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

Vec3 vertex_at(const VertexChannel& p, std::uint32_t i) {
    const std::size_t o = static_cast<std::size_t>(i) * p.components;
    return {p.values[o], p.values[o + 1], p.values[o + 2]};
}

float triangle_area2(const Vec3& a, const Vec3& b, const Vec3& c) {
    const float abx=b[0]-a[0], aby=b[1]-a[1], abz=b[2]-a[2];
    const float acx=c[0]-a[0], acy=c[1]-a[1], acz=c[2]-a[2];
    const float x=aby*acz-abz*acy;
    const float y=abz*acx-abx*acz;
    const float z=abx*acy-aby*acx;
    return x*x+y*y+z*z;
}

bool build_mesh(const Primitive& primitive, const VertexChannel& pos, LocalMesh& out) {
    const std::uint32_t tri_count = static_cast<std::uint32_t>(primitive.indices.size() / 3);
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    remap.reserve(static_cast<std::size_t>(tri_count) * 3);
    for (std::uint32_t t=0;t<tri_count;t++) {
        const std::size_t io = static_cast<std::size_t>(t) * 3;
        if (io + 2 >= primitive.indices.size()) break;
        const std::uint32_t ia=primitive.indices[io], ib=primitive.indices[io+1], ic=primitive.indices[io+2];
        const std::size_t vc=pos.values.size()/pos.components;
        if (ia>=vc || ib>=vc || ic>=vc) continue;
        const Vec3 a=vertex_at(pos,ia), b=vertex_at(pos,ib), c=vertex_at(pos,ic);
        if (!finite3(a) || !finite3(b) || !finite3(c) || triangle_area2(a,b,c) <= 1e-30f) continue;
        const std::uint32_t ids[3]{ia,ib,ic};
        for (int k=0;k<3;k++) {
            auto it=remap.find(ids[k]);
            if (it==remap.end()) {
                if (out.vertices.size() >= std::numeric_limits<std::uint32_t>::max()) return false;
                const auto local=static_cast<std::uint32_t>(out.vertices.size());
                remap.emplace(ids[k],local);
                out.vertices.push_back(vertex_at(pos,ids[k]));
                out.indices.push_back(local);
            } else out.indices.push_back(it->second);
        }
    }
    return out.indices.size() >= 3;
}

void append_mesh(const LocalMesh& source, LocalMesh& destination) {
    const auto offset = static_cast<std::uint32_t>(destination.vertices.size());
    destination.vertices.insert(destination.vertices.end(), source.vertices.begin(), source.vertices.end());
    destination.indices.reserve(destination.indices.size() + source.indices.size());
    for (const auto index : source.indices) destination.indices.push_back(offset + index);
}

#if DTGLB_HAS_PHYSX
class Allocator final : public physx::PxAllocatorCallback {
// Some Release SDK structures serialize padding not rewritten by the binary
// converter. Start allocations initialized so repeated builds are byte-stable.
public: void* allocate(size_t size, const char*, const char*, int) override { return std::calloc(1, size); }
        void deallocate(void* p) override { std::free(p); }
};
class Errors final : public physx::PxErrorCallback {
public: void reportError(physx::PxErrorCode::Enum code, const char* msg, const char*, int) override {
        last = msg ? msg : "PhysX error";
        if (code != physx::PxErrorCode::eDEBUG_INFO && code != physx::PxErrorCode::eDEBUG_WARNING &&
            code != physx::PxErrorCode::ePERF_WARNING) failure = last;
    }
    void clear() { last.clear(); failure.clear(); }
    std::string last;
    std::string failure;
};
class Output final : public physx::PxOutputStream {
public: physx::PxU32 write(const void* src, physx::PxU32 count) override { const auto* p = static_cast<const std::uint8_t*>(src); data.insert(data.end(), p, p + count); return count; }
    std::vector<std::uint8_t> data;
};

class CookingContext {
public:
    CookingContext() {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, errors);
        if (!foundation) return;
        physx::PxCookingParams params{physx::PxTolerancesScale{}};
        // Darktide's returned UNIT corpus uses MESH v15 with BVH34 midphase.
        params.midphaseDesc = physx::PxMeshMidPhase::eBVH34;
        cooking = PxCreateCooking(PX_PHYSICS_VERSION, *foundation, params);
    }
    ~CookingContext() {
        if (cooking) cooking->release();
        if (foundation) foundation->release();
    }
    CookingContext(const CookingContext&) = delete;
    CookingContext& operator=(const CookingContext&) = delete;

    physx::PxFoundation* get_foundation(std::string& error) const {
        if (!foundation) error = "PxCreateFoundation failed";
        return foundation;
    }
    void clear_errors() { errors.clear(); }
    std::string last_error() const { return errors.failure; }

    bool triangle(const LocalMesh& mesh, std::vector<std::uint8_t>& bytes, std::string& error) {
        if (!ready(error)) return false;
        errors.clear();
        const auto vertices = native_vertices(mesh);
        std::vector<physx::PxU32> indices(mesh.indices.begin(), mesh.indices.end());
        if (vertices.size() > std::numeric_limits<physx::PxU32>::max() ||
            indices.size() / 3 > std::numeric_limits<physx::PxU32>::max()) {
            error = "physics triangle mesh exceeds PhysX descriptor limits";
            return false;
        }
    physx::PxTriangleMeshDesc desc;
    desc.points.count = static_cast<physx::PxU32>(vertices.size()); desc.points.stride = sizeof(physx::PxVec3); desc.points.data = vertices.data();
    desc.triangles.count = static_cast<physx::PxU32>(indices.size() / 3); desc.triangles.stride = 3 * sizeof(physx::PxU32); desc.triangles.data = indices.data();
        Output output;
        if (!cooking->cookTriangleMesh(desc, output)) {
            error = errors.last.empty() ? "PhysX triangle mesh cooking failed" : errors.last;
            return false;
        }
        bytes = std::move(output.data);
        return true;
    }

    bool convex(const LocalMesh& mesh, std::vector<std::uint8_t>& bytes, std::string& error) {
        if (!ready(error)) return false;
        errors.clear();
        const auto vertices = native_vertices(mesh);
        if (vertices.size() < 4 || vertices.size() > std::numeric_limits<physx::PxU32>::max()) {
            error = "convex collision requires at least four finite vertices within PhysX limits";
            return false;
        }
        physx::PxConvexMeshDesc desc;
        desc.points.count = static_cast<physx::PxU32>(vertices.size());
        desc.points.stride = sizeof(physx::PxVec3);
        desc.points.data = vertices.data();
        desc.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX;
        desc.vertexLimit = 255;
        Output output;
        if (!cooking->cookConvexMesh(desc, output)) {
            error = errors.last.empty() ? "PhysX convex mesh cooking failed" : errors.last;
            return false;
        }
        bytes = std::move(output.data);
        return true;
    }

private:
    bool ready(std::string& error) const {
        if (!foundation) { error = "PxCreateFoundation failed"; return false; }
        if (!cooking) { error = "PxCreateCooking failed"; return false; }
        return true;
    }
    static std::vector<physx::PxVec3> native_vertices(const LocalMesh& mesh) {
        std::vector<physx::PxVec3> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto& value : mesh.vertices) vertices.emplace_back(value[0], value[1], value[2]);
        return vertices;
    }
    Allocator allocator;
    Errors errors;
    physx::PxFoundation* foundation = nullptr;
    physx::PxCooking* cooking = nullptr;
};

CookingContext& cooking_context() {
    static CookingContext context;
    return context;
}
#endif

} // namespace

std::recursive_mutex& physx_context_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

physx::PxFoundation* physics_foundation(std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
#if DTGLB_HAS_PHYSX
    return cooking_context().get_foundation(error);
#else
    error = "PhysX SDK is unavailable";
    return nullptr;
#endif
}

void physics_clear_errors() {
    std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
#if DTGLB_HAS_PHYSX
    cooking_context().clear_errors();
#endif
}

std::string physics_last_error() {
    std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
#if DTGLB_HAS_PHYSX
    return cooking_context().last_error();
#else
    return "PhysX SDK is unavailable";
#endif
}

bool cook_triangle_mesh(const Primitive& primitive,
                        CookedTriangleMesh& cooked,
                        std::string& error) {
    cooked = {};
    const auto* pos=positions(primitive);
    if (!pos) { error="physics triangle mesh has no POSITION channel"; return false; }
    if (primitive.indices.size()<3) { error="physics triangle mesh has no triangles"; return false; }
    LocalMesh local;
    if (!build_mesh(primitive, *pos, local)) { error="physics mesh contained no finite non-degenerate triangles"; return false; }
    cooked.source_triangle_count = static_cast<std::uint32_t>(local.indices.size() / 3);
#if DTGLB_HAS_PHYSX
    std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
    if (!cooking_context().triangle(local, cooked.bytes, error)) return false;
#else
    error = "PhysX 4.1 headers are unavailable; native triangle mesh cooking is required";
    return false;
#endif
    return true;
}

bool cook_convex_mesh(const Primitive& primitive,
                      std::vector<std::uint8_t>& bytes,
                      std::string& error) {
    const auto* pos = positions(primitive);
    if (!pos) { error = "physics convex mesh has no POSITION channel"; return false; }
    LocalMesh local;
    if (!build_mesh(primitive, *pos, local)) {
        error = "physics convex mesh contained no finite non-degenerate triangles";
        return false;
    }
#if DTGLB_HAS_PHYSX
    std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
    return cooking_context().convex(local, bytes, error);
#else
    error = "PhysX 4.1 headers are unavailable; native convex cooking is required";
    return false;
#endif
}


bool prepare_scene_physics(const std::vector<Primitive>& primitives,
                           std::vector<PhysicsActor>& actors, std::string& error) {
    for (auto& actor : actors) {
        std::vector<PhysicsShape> ready;
        for (const auto& shape : actor.shapes) {
            // Fitted/grouped shapes already contain the complete cooked mesh.
            // Their primitive field identifies only the first contributing draw.
            if (!shape.physx_cooked.empty() &&
                (shape.type == PhysicsShapeType::TriangleMesh || shape.type == PhysicsShapeType::Convex)) {
                ready.push_back(shape);
                continue;
            }
            if (shape.type == PhysicsShapeType::TriangleMesh) {
                if (shape.primitive < 0 || static_cast<std::size_t>(shape.primitive) >= primitives.size()) {
                    error = "physics shape references an invalid primitive";
                    return false;
                }
                CookedTriangleMesh mesh;
                if (!cook_triangle_mesh(primitives[shape.primitive], mesh, error)) return false;
                PhysicsShape cooked = shape;
                cooked.physx_cooked = std::move(mesh.bytes);
                ready.push_back(std::move(cooked));
                continue;
            }
            if (shape.type == PhysicsShapeType::Convex || shape.type == PhysicsShapeType::Unknown7) {
                if (shape.type == PhysicsShapeType::Unknown7) {
                    error = "unknown physics shape type 7 cannot be generated from glTF geometry";
                    return false;
                }
                if (shape.primitive < 0 || static_cast<std::size_t>(shape.primitive) >= primitives.size()) {
                    error = "convex physics shape references an invalid primitive";
                    return false;
                }
                PhysicsShape cooked = shape;
                if (!cook_convex_mesh(primitives[shape.primitive], cooked.physx_cooked, error)) return false;
                ready.push_back(std::move(cooked));
                continue;
            }
            if (shape.type == PhysicsShapeType::HeightField) {
                error = "height-field cooking is selected but the Darktide PhysX height-field cooker is not implemented yet";
                return false;
            }
            ready.push_back(shape);
        }
        actor.shapes=std::move(ready);
    }
    return true;
}

bool build_actor(const std::vector<Primitive>& primitives,
                                  std::string actor_name,
                                  std::string root_node,
                                  std::uint32_t root_node_hash,
                                  const FittedActorOptions& options,
                                  PhysicsActor& actor,
                                  std::string& error) {
    error.clear();
    if (options.shape != PhysicsShapeType::Sphere && options.shape != PhysicsShapeType::Capsule && options.shape != PhysicsShapeType::Box &&
        options.shape != PhysicsShapeType::TriangleMesh && options.shape != PhysicsShapeType::Convex) {
        error = "UNIT physics supports geometry, convex, sphere, capsule, or box collision";
        return false;
    }
    if (options.actor_template.empty()) {
        error = "physics actor template must not be empty";
        return false;
    }
    if (actor_name.empty() || root_node.empty() || options.material.empty() ||
        options.shape_template.empty() || !std::isfinite(options.mass) || options.mass < 0.0f) {
        error = "physics actor names/templates must be non-empty and mass must be finite and nonnegative";
        return false;
    }

    if (options.shape == PhysicsShapeType::TriangleMesh || options.shape == PhysicsShapeType::Convex) {
        // PhysX only permits triangle meshes on static/kinematic actors.  For a
        // dynamic actor, "geometry" intentionally means one convex hull per
        // immutable source object/instance group: it remains source-driven and
        // is valid at runtime.
        const PhysicsShapeType effective_shape =
            options.shape == PhysicsShapeType::TriangleMesh && options.actor_template == "dynamic"
                ? PhysicsShapeType::Convex : options.shape;
        actor = {};
        actor.name = std::move(actor_name);
        actor.name_hash = id32_from_id64(actor.name);
        actor.actor_template = options.actor_template;
        actor.node = std::move(root_node);
        actor.node_hash = root_node_hash;
        actor.mass = options.mass;
        actor.enabled = true;
        struct Group {
            LocalMesh mesh;
            std::size_t first_primitive = 0;
        };
        std::map<std::pair<int, int>, Group> groups;
        for (std::size_t index = 0; index < primitives.size(); ++index) {
            const auto* position = positions(primitives[index]);
            if (!position || primitives[index].indices.size() < 3) continue;
            const int object = primitives[index].collision_object;
            const int instance = primitives[index].collision_instance;
            const auto key = object >= 0 && instance >= 0
                ? std::make_pair(object, instance)
                : std::make_pair(-1, static_cast<int>(index));
            auto [it, inserted] = groups.emplace(key, Group{});
            if (inserted) it->second.first_primitive = index;
            LocalMesh local;
            if (build_mesh(primitives[index], *position, local)) append_mesh(local, it->second.mesh);
        }
        actor.shapes.reserve(groups.size());
        for (const auto& entry : groups) {
            const auto& group = entry.second;
            if (group.mesh.indices.empty()) continue;
            PhysicsShape shape;
            shape.name = actor.name + "_geometry_" + std::to_string(group.first_primitive);
            shape.node = actor.node;
            shape.node_hash = actor.node_hash;
            shape.type = effective_shape;
            shape.material = options.material;
            shape.shape_template = options.shape_template;
            shape.primitive = static_cast<int>(group.first_primitive);
            shape.computed_convex = effective_shape == PhysicsShapeType::Convex;
#if DTGLB_HAS_PHYSX
            std::lock_guard<std::recursive_mutex> lock(physx_context_mutex());
            if (effective_shape == PhysicsShapeType::Convex) {
                if (!cooking_context().convex(group.mesh, shape.physx_cooked, error)) return false;
            } else {
                CookedTriangleMesh cooked;
                if (!cooking_context().triangle(group.mesh, cooked.bytes, error)) return false;
                shape.physx_cooked = std::move(cooked.bytes);
            }
#else
            error = "PhysX 4.1 headers are unavailable; native geometry cooking is required";
            return false;
#endif
            actor.shapes.push_back(std::move(shape));
        }
        if (actor.shapes.empty()) {
            error = "geometry collision requires at least one emitted triangle primitive";
            return false;
        }
        return true;
    }

    Vec3 minimum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::infinity()};
    Vec3 maximum{-minimum[0], -minimum[1], -minimum[2]};
    bool found = false;
    for (const auto& primitive : primitives) {
        const VertexChannel* position = nullptr;
        for (const auto& channel : primitive.channels) {
            if (channel.semantic == VertexChannel::Semantic::Position && channel.components >= 3) {
                position = &channel;
                break;
            }
        }
        if (!position || position->components == 0 || position->values.size() % position->components != 0) continue;
        for (std::size_t offset = 0; offset < position->values.size(); offset += position->components) {
            const Vec3 value{position->values[offset], position->values[offset + 1], position->values[offset + 2]};
            if (!finite3(value)) { error = "physics fit encountered a non-finite POSITION"; return false; }
            for (int axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], value[axis]);
                maximum[axis] = std::max(maximum[axis], value[axis]);
            }
            found = true;
        }
    }
    if (!found) { error = "physics fit requires emitted POSITION geometry"; return false; }

    const Vec3 center{(minimum[0] + maximum[0]) * 0.5f,
                      (minimum[1] + maximum[1]) * 0.5f,
                      (minimum[2] + maximum[2]) * 0.5f};
    PhysicsShape shape;
    shape.name = actor_name + "_shape";
    shape.node = root_node;
    shape.node_hash = root_node_hash;
    shape.type = options.shape;
    shape.material = options.material;
    shape.shape_template = options.shape_template;
    shape.local_transform = {1,0,0,0, 0,1,0,0, 0,0,1,0,
                             center[0],center[1],center[2],1};
    if (options.shape == PhysicsShapeType::Box) {
        for (int axis = 0; axis < 3; ++axis) {
            shape.half_extents[axis] = (maximum[axis] - minimum[axis]) * 0.5f;
            if (!std::isfinite(shape.half_extents[axis]) || shape.half_extents[axis] <= 0.0f) {
                error = "fitted box collision requires positive extent on every axis; use sphere collision for planar geometry";
                return false;
            }
        }
    } else if (options.shape == PhysicsShapeType::Sphere) {
        // the Stingray physics compiler's sphere: the largest half extent of the bounding box
        shape.radius = std::max({maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2]}) * 0.5f;
        if (!std::isfinite(shape.radius) || shape.radius <= 0.0f) {
            error = "fitted sphere collision requires nonzero geometry extent";
            return false;
        }
    } else {
        // Capsule fitting uses the longest AABB axis in the geometry's current
        // frame (the UNIT root in v1, the body frame in v2). PhysX capsules use
        // local +X, so rotate that axis onto the selected frame axis.
        int axis = 0;
        for (int candidate = 1; candidate < 3; ++candidate)
            if (maximum[candidate] - minimum[candidate] > maximum[axis] - minimum[axis]) axis = candidate;
        // as the Stingray physics compiler fits a capsule to a mesh's bounding box: the radius is the larger
        // half extent across the axis, the cylinder takes the rest of the length
        const float half_axis = (maximum[axis] - minimum[axis]) * 0.5f;
        shape.radius = std::max((maximum[(axis + 1) % 3] - minimum[(axis + 1) % 3]) * 0.5f,
                                (maximum[(axis + 2) % 3] - minimum[(axis + 2) % 3]) * 0.5f);
        if (!std::isfinite(shape.radius) || shape.radius <= 0.0f) {
            error = "fitted capsule collision requires positive radial geometry extent";
            return false;
        }
        shape.height = std::max(0.0f, (half_axis - shape.radius) * 2.0f);
        if (axis == 1) {
            shape.local_transform[0] = 0.0f; shape.local_transform[1] = 1.0f; shape.local_transform[2] = 0.0f;
            shape.local_transform[4] = -1.0f; shape.local_transform[5] = 0.0f;
        } else if (axis == 2) {
            shape.local_transform[0] = 0.0f; shape.local_transform[1] = 0.0f; shape.local_transform[2] = 1.0f;
            shape.local_transform[8] = -1.0f; shape.local_transform[10] = 0.0f;
        }
    }

    actor = {};
    actor.name = std::move(actor_name);
    actor.name_hash = id32_from_id64(actor.name);
    actor.actor_template = options.actor_template;
    actor.node = std::move(root_node);
    actor.node_hash = root_node_hash;
    actor.mass = options.mass;
    actor.enabled = true;
    actor.shapes.push_back(std::move(shape));
    return true;
}

std::uint32_t template_id32(const std::string& name) {
    if (name.size() == 9 && name[0] == '#' &&
        std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        return static_cast<std::uint32_t>(std::strtoul(name.c_str() + 1, nullptr, 16));
    return id32_from_id64(name);
}

std::uint32_t shape_template_id32(const std::string& actor_template,
                                  const std::string& shape_template,
                                  bool node_bound) {
    // this one took way too long, crates just kept falling through the floor
    if (shape_template == "default" && actor_template == "dynamic")
        return node_bound ? id32_from_id64("ragdoll") : 0x9800a618u;
    return template_id32(shape_template);
}

bool serialize_actor(const PhysicsActor& actor,
                               std::vector<std::uint8_t>& bytes,
                               std::string& error,
                               bool node_bound) {
    bytes.clear();
    error.clear();
    if (actor.name.empty() || actor.node.empty() ||
        actor.actor_template.empty() || !std::isfinite(actor.mass) || actor.mass < 0.0f ||
        actor.shapes.empty() || actor.shapes.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "invalid physics actor";
        return false;
    }
    BinaryWriter writer;
    writer.u32(actor.name_hash ? actor.name_hash : id32_from_id64(actor.name));
    writer.u32(template_id32(actor.actor_template));
    writer.u32(actor.node_hash ? actor.node_hash : id32_from_id64(actor.node));
    writer.f32(actor.mass);
    writer.u32(static_cast<std::uint32_t>(actor.shapes.size()));
    for (const auto& shape : actor.shapes) {
        if (shape.type != PhysicsShapeType::Sphere && shape.type != PhysicsShapeType::Capsule && shape.type != PhysicsShapeType::Box &&
            shape.type != PhysicsShapeType::TriangleMesh && shape.type != PhysicsShapeType::Convex) {
            error = "actor contains an unsupported collision shape";
            return false;
        }
        if (shape.type == PhysicsShapeType::TriangleMesh && actor.actor_template == "dynamic") {
            error = "dynamic triangle-mesh actors are unsupported; cook a convex mesh for dynamic collision";
            return false;
        }
        if (shape.material.empty() || shape.shape_template.empty() || shape.node.empty() ||
            !std::all_of(shape.local_transform.begin(), shape.local_transform.end(),
                         [](float value) { return std::isfinite(value); })) {
            error = "collision shape contains an invalid name, node, template, or transform";
            return false;
        }
        writer.u32(static_cast<std::uint32_t>(shape.type));
        writer.u32(id32_from_id64(shape.material));
        writer.u32(shape_template_id32(actor.actor_template, shape.shape_template, node_bound));
        for (float value : shape.local_transform) writer.f32(value);
        if (shape.type == PhysicsShapeType::TriangleMesh || shape.type == PhysicsShapeType::Convex) {
            if (shape.physx_cooked.empty()) { error = "cooked geometry shape has no PhysX payload"; return false; }
            writer.u32(static_cast<std::uint32_t>(shape.physx_cooked.size()));
            writer.bytes(shape.physx_cooked.data(), shape.physx_cooked.size());
            writer.u32(0); // shape-node slot
            writer.u32(shape.node_hash ? shape.node_hash : id32_from_id64(shape.node));
        } else {
            writer.u32(0); // no cooked primitive payload
            writer.u32(0); // shape-node slot
            writer.u32(shape.node_hash ? shape.node_hash : id32_from_id64(shape.node));
        }
        if (shape.type == PhysicsShapeType::Sphere) {
            if (!std::isfinite(shape.radius) || shape.radius <= 0.0f) {
                error = "primitive sphere radius must be finite and positive";
                return false;
            }
            writer.f32(shape.radius);
        } else if (shape.type == PhysicsShapeType::Capsule) {
            if (!std::isfinite(shape.radius) || shape.radius <= 0.0f ||
                !std::isfinite(shape.height) || shape.height < 0.0f) {
                error = "primitive capsule radius and cylinder height must be finite and valid";
                return false;
            }
            writer.f32(shape.radius);
            writer.f32(shape.height);
        } else if (shape.type == PhysicsShapeType::Box) {
            for (float extent : shape.half_extents) {
                if (!std::isfinite(extent) || extent <= 0.0f) {
                    error = "primitive box half-extents must be finite and positive";
                    return false;
                }
                writer.f32(extent);
            }
        }
    }
    for (int i = 0; i < 6; ++i) writer.u32(0xffffffffu);
    writer.u8(actor.enabled ? 1u : 0u);
    writer.u8(0); writer.u8(0); writer.u8(0);
    bytes = writer.data();
    return true;
}

} // namespace dtglb::stingray::physics
