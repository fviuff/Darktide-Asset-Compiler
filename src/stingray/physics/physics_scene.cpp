#include "stingray/physics/physics_scene.h"
#include "stingray/binary_writer.h"
#include "stingray/murmur_hash.h"

#include <limits>
#include <set>

namespace dtglb::stingray::physics {

bool serialize_physics_scene(const std::vector<CollectionBody>& bodies,
                             const std::vector<CollectionJoint>& joints,
                             std::vector<std::uint8_t>& bytes,
                             std::string& error) {
    bytes.clear();
    error.clear();
    std::set<std::uint32_t> names;
    std::set<std::uint32_t> nodes;
    for (const auto& body : bodies) {
        if (body.actor.name.empty() || body.actor.node.empty()) {
            error = "physics scene bodies require actor and UNIT node names";
            return false;
        }
        const auto name = body.actor.name_hash ? body.actor.name_hash : id32_from_id64(body.actor.name);
        const auto node = body.actor.node_hash ? body.actor.node_hash : id32_from_id64(body.actor.node);
        if (!names.insert(name).second || !nodes.insert(node).second) {
            error = "physics scene bodies require distinct actor and UNIT node identities";
            return false;
        }
    }
    PhysicsCollections collections;
    if (!serialize_physics_collections(bodies, joints, collections, error)) return false;

    // Every offset in physics_scene_data is a byte offset from the start of
    // this bytearray. The two native Darktide samples use 128-byte alignment
    // for the second SEBD collection and a fixed 128-byte header at the end.
    const std::uint64_t second_offset = (std::uint64_t(collections.dependencies.size()) + 127u) & ~std::uint64_t(127u);
    const std::uint64_t body_offset = second_offset + collections.objects.size();
    const std::uint64_t shape_offset = body_offset + std::uint64_t(bodies.size()) * 72u;
    const std::uint64_t template_offset = shape_offset + std::uint64_t(collections.shapes.size()) * 16u;
    const std::uint64_t material_offset = template_offset + std::uint64_t(collections.shapes.size()) * 8u;
    const std::uint64_t total = material_offset + std::uint64_t(collections.materials.size()) * 8u + 128u;
    if (total > std::numeric_limits<std::uint32_t>::max() ||
        collections.body_object_indices.size() != bodies.size()) {
        error = "physics scene exceeds native offsets or has incomplete body bindings";
        return false;
    }

    BinaryWriter writer;
    writer.bytes(collections.dependencies.data(), collections.dependencies.size());
    while (writer.data().size() < second_offset) writer.u8(0);
    writer.bytes(collections.objects.data(), collections.objects.size());
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const auto& actor = bodies[i].actor;
        writer.u32(collections.body_object_indices[i]);
        writer.u32(actor.name_hash ? actor.name_hash : id32_from_id64(actor.name));
        writer.u32(actor.node_hash ? actor.node_hash : id32_from_id64(actor.node));
        writer.u32(actor.enabled ? 1u : 0u);
        for (int axis = 0; axis < 6; ++axis) writer.f32(0.0f); // Linear then angular velocity.
        writer.u32(static_cast<std::uint32_t>(body_offset));
        writer.u32(0); // No free-joint indices.
        for (int callback = 0; callback < 6; ++callback) writer.u32(0xffffffffu);
    }
    for (const auto& shape : collections.shapes) {
        writer.u32(shape.body_index);
        writer.u32(shape.local_shape_index);
        writer.u32(shape.object_index);
        // Native bits are trigger=1, simulation=2, scene query=4. They are
        // reordered from PxShapeFlags by repx_compiler's initial-state writer.
        writer.u32(6u);
    }
    for (const auto& shape : collections.shapes) {
        writer.u32(shape.object_index);
        writer.u32(shape.shape_template_hash);
    }
    for (const auto& material : collections.materials) {
        writer.u32(material.object_index);
        writer.u32(material.material_hash);
    }

    writer.u32(41200u);
    writer.u32(0);
    writer.u32(0); // Dependency collection starts at the beginning.
    writer.u32(static_cast<std::uint32_t>(collections.dependencies.size()));
    for (int i = 0; i < 16; ++i) writer.f32(i % 5 == 0 ? 1.0f : 0.0f);
    writer.u32(static_cast<std::uint32_t>(second_offset));
    writer.u32(static_cast<std::uint32_t>(collections.objects.size()));
    writer.u32(static_cast<std::uint32_t>(body_offset));
    writer.u32(static_cast<std::uint32_t>(bodies.size()));
    writer.u32(static_cast<std::uint32_t>(body_offset));
    writer.u32(0); // Empty free-joint lookup table; connected D6 joints live in SEBD.
    writer.u32(static_cast<std::uint32_t>(shape_offset));
    writer.u32(static_cast<std::uint32_t>(collections.shapes.size()));
    writer.u32(static_cast<std::uint32_t>(template_offset));
    writer.u32(static_cast<std::uint32_t>(collections.shapes.size()));
    writer.u32(static_cast<std::uint32_t>(material_offset));
    writer.u32(static_cast<std::uint32_t>(collections.materials.size()));
    bytes = writer.data();
    return true;
}

} // namespace dtglb::stingray::physics
