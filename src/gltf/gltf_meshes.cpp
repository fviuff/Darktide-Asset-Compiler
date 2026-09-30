#include "gltf/gltf_meshes.h"

#include "cgltf.h"
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace dtglb::gltf {
namespace {

const cgltf_attribute* find_draco_mapping(const cgltf_primitive& primitive, const char* name) {
    if (!name) return nullptr;
    const auto& compression = primitive.draco_mesh_compression;
    for (cgltf_size i = 0; i < compression.attributes_count; ++i) {
        const auto& mapping = compression.attributes[i];
        if (mapping.name && std::strcmp(mapping.name, name) == 0) return &mapping;
    }
    return nullptr;
}

bool draco_unique_id(const cgltf_data* data, const cgltf_attribute& mapping, std::uint32_t& out) {
    (void)data;
    if (mapping.draco_id < 0 || static_cast<std::uint64_t>(mapping.draco_id) > std::numeric_limits<std::uint32_t>::max()) return false;
    out = static_cast<std::uint32_t>(mapping.draco_id);
    return true;
}

void apply_accessor_normalization(const cgltf_accessor& accessor, std::vector<float>& values) {
    if (!accessor.normalized) return;
    switch (accessor.component_type) {
        case cgltf_component_type_r_8:
            for (float& value : values) value = std::max(value / 127.0f, -1.0f);
            break;
        case cgltf_component_type_r_8u:
            for (float& value : values) value /= 255.0f;
            break;
        case cgltf_component_type_r_16:
            for (float& value : values) value = std::max(value / 32767.0f, -1.0f);
            break;
        case cgltf_component_type_r_16u:
            for (float& value : values) value /= 65535.0f;
            break;
        default:
            break;
    }
}

bool decode_core_fallback(const cgltf_primitive& primitive, DracoPrimitiveData& out) {
    const cgltf_accessor* position = nullptr;
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const auto& attribute = primitive.attributes[i];
        if (attribute.type == cgltf_attribute_type_position) position = attribute.data;
    }
    if (!position || position->count == 0 || cgltf_num_components(position->type) < 3) return false;

    const auto vertex_count = static_cast<std::size_t>(position->count);
    out.attribute_values.resize(static_cast<std::size_t>(primitive.attributes_count));
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const auto* accessor = primitive.attributes[i].data;
        if (!accessor || accessor->count != position->count) return false;
        const auto components = cgltf_num_components(accessor->type);
        if (components == 0 || components > 4) return false;
        auto& values = out.attribute_values[static_cast<std::size_t>(i)];
        values.resize(vertex_count * static_cast<std::size_t>(components));
        if (!cgltf_accessor_unpack_floats(accessor, values.data(), values.size())) return false;
    }

    if (!primitive.indices) {
        out.indices.resize(vertex_count);
        for (std::size_t i = 0; i < vertex_count; ++i) out.indices[i] = static_cast<std::uint32_t>(i);
    } else {
        const auto* indices = primitive.indices;
        if (indices->type != cgltf_type_scalar || cgltf_component_size(indices->component_type) == 0 ||
            cgltf_component_size(indices->component_type) > sizeof(std::uint32_t)) return false;
        out.indices.resize(static_cast<std::size_t>(indices->count));
        for (cgltf_size i = 0; i < indices->count; ++i) {
            const auto value = cgltf_accessor_read_index(indices, i);
            if (value >= vertex_count || value > std::numeric_limits<std::uint32_t>::max()) return false;
            out.indices[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(value);
        }
    }
    out.vertex_count = vertex_count;
    out.used_core_fallback = true;
    return true;
}

bool draco_is_required(const cgltf_data* data) {
    if (!data) return false;
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        if (data->extensions_required[i] && std::strcmp(data->extensions_required[i], "KHR_draco_mesh_compression") == 0)
            return true;
    }
    return false;
}

} // namespace

bool decode_draco_primitive(const cgltf_data* data,
                            const cgltf_primitive& primitive,
                            DracoPrimitiveData& out,
                            std::string& error) {
    out = {};
    if (!data || !primitive.has_draco_mesh_compression) {
        error = "KHR_draco_mesh_compression adapter called for an uncompressed primitive";
        return false;
    }
    const auto fail_or_core_fallback = [&](std::string message) {
        if (!draco_is_required(data)) {
            DracoPrimitiveData fallback;
            if (decode_core_fallback(primitive, fallback)) {
                out = std::move(fallback);
                return true;
            }
        }
        error = std::move(message);
        return false;
    };

    const auto* view = primitive.draco_mesh_compression.buffer_view;
    if (!view) return fail_or_core_fallback("KHR_draco_mesh_compression is missing its bufferView");
    const auto* bytes = cgltf_buffer_view_data(view);
    if (!bytes && view->size)
        return fail_or_core_fallback("KHR_draco_mesh_compression bufferView data is unavailable");

    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(view->size));
    draco::Decoder decoder;
    auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
    if (!decoded.ok())
        return fail_or_core_fallback("KHR_draco_mesh_compression decode failed: " + decoded.status().error_msg_string());
    std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();
    if (!mesh) return fail_or_core_fallback("KHR_draco_mesh_compression decoder returned no mesh");

    out.vertex_count = static_cast<std::size_t>(mesh->num_points());
    out.attribute_values.resize(static_cast<std::size_t>(primitive.attributes_count));

    for (cgltf_size attribute_index = 0; attribute_index < primitive.attributes_count; ++attribute_index) {
        const auto& source_attribute = primitive.attributes[attribute_index];
        const auto* mapping = find_draco_mapping(primitive, source_attribute.name);
        if (!mapping) continue;
        if (!source_attribute.data)
            return fail_or_core_fallback("KHR_draco_mesh_compression attribute is missing its glTF accessor metadata");

        std::uint32_t unique_id = 0;
        if (!draco_unique_id(data, *mapping, unique_id))
            return fail_or_core_fallback("KHR_draco_mesh_compression attribute has an invalid Draco unique id");
        const auto* attribute = mesh->GetAttributeByUniqueId(unique_id);
        if (!attribute)
            return fail_or_core_fallback("KHR_draco_mesh_compression decoded mesh is missing attribute id " + std::to_string(unique_id));

        const std::uint32_t components = static_cast<std::uint32_t>(cgltf_num_components(source_attribute.data->type));
        if (components == 0 || components > 4 || attribute->num_components() != static_cast<std::int8_t>(components))
            return fail_or_core_fallback("KHR_draco_mesh_compression attribute component count disagrees with its glTF accessor");
        if (source_attribute.data->count != mesh->num_points())
            return fail_or_core_fallback("KHR_draco_mesh_compression attribute count disagrees with decoded point count");

        auto& values = out.attribute_values[static_cast<std::size_t>(attribute_index)];
        values.resize(out.vertex_count * components);
        for (std::size_t point_index = 0; point_index < out.vertex_count; ++point_index) {
            const auto value_index = attribute->mapped_index(draco::PointIndex(static_cast<std::uint32_t>(point_index)));
            if (!attribute->ConvertValue<float>(value_index,
                                                static_cast<std::int8_t>(components),
                                                values.data() + point_index * components))
                return fail_or_core_fallback("KHR_draco_mesh_compression failed to convert decoded attribute values");
        }
        apply_accessor_normalization(*source_attribute.data, values);
    }

    out.indices.reserve(static_cast<std::size_t>(mesh->num_faces()) * 3);
    for (std::uint32_t face_index = 0; face_index < mesh->num_faces(); ++face_index) {
        const auto& face = mesh->face(draco::FaceIndex(face_index));
        out.indices.push_back(face[0].value());
        out.indices.push_back(face[1].value());
        out.indices.push_back(face[2].value());
    }

    return true;
}

} // namespace dtglb::gltf
