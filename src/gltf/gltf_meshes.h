#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct cgltf_data;
struct cgltf_primitive;

namespace dtglb::gltf {

struct DracoPrimitiveData {
    std::size_t vertex_count = 0;
    bool used_core_fallback = false;
    // One entry per cgltf primitive attribute. Empty entries were not carried
    // by KHR_draco_mesh_compression and must use the ordinary accessor path.
    std::vector<std::vector<float>> attribute_values;
    // Draco mesh faces are already a canonical triangle list, including when
    // the source glTF primitive declared TRIANGLE_STRIP.
    std::vector<std::uint32_t> indices;
};

bool decode_draco_primitive(const cgltf_data* data,
                            const cgltf_primitive& primitive,
                            DracoPrimitiveData& out,
                            std::string& error);

} // namespace dtglb::gltf
