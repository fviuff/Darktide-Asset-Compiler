#pragma once

#include "gltf/json_value.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::material {

// The shader section of a shader provider / parent material stream (version 0x2b): the compiled shaders of every
// context and pass, as the game's D3D12 renderer loads them. decode() and encode() are exact inverses for every
// shader section the game ships. Names are IdString32 values.
struct RenderState {
    std::uint32_t key = 0;
    std::uint8_t kind = 0;
    std::uint32_t value = 0;
    std::uint32_t extra = 0;
};

struct SamplerState {
    std::uint32_t name = 0;
    std::vector<RenderState> states;
};

// One compiled stage: Oodle Kraken compressed DXBC (DXIL) plus the bindings the engine builds the root signature
// from. Root parameter indices are numbered across a pass's stages.
struct ShaderVariant {
    std::vector<std::uint8_t> bytecode; // compressed
    std::uint32_t compression = 5;
    std::uint32_t size = 0;             // uncompressed
    std::uint64_t hash = 0;             // murmur64 of the compressed bytes
    // {name, root parameter, size, register, count, space}; the second list is never used by the game
    std::array<std::vector<std::array<std::uint32_t, 6>>, 2> constant_buffers;
    // {name, root parameter, register, count, space, stride (0 raw, n structured, 0xffffffff texture), 0}:
    // 0 unused, 1 SRVs, 2 bindless SRV arrays (count 0xffffffff), 3 UAVs, 4 bindless UAV arrays
    std::array<std::vector<std::array<std::uint32_t, 7>>, 5> resources;
    std::vector<std::array<std::uint32_t, 4>> static_samplers; // {name, register, count, space}
    std::vector<std::array<std::uint32_t, 3>> inputs;          // input signature {semantic, index, register}
    std::vector<std::array<std::uint32_t, 4>> sampler_arrays;  // bindless {name, register, 0, space}
};

struct ShaderPass {
    std::vector<RenderState> render_states;
    std::vector<SamplerState> sampler_states;
    std::array<std::vector<ShaderVariant>, 6> stages; // vertex, domain, hull, geometry, pixel, compute
    std::vector<std::array<std::uint32_t, 4>> records16;
    std::array<std::uint32_t, 4> words{};
    std::vector<std::array<std::uint32_t, 5>> stream_out;
};

struct DeviceShader {
    std::vector<std::vector<ShaderPass>> groups; // one per shader_data entry, each a list of passes
    std::uint32_t tail = 0;
};

struct ConstantBufferReflection {
    std::vector<std::array<std::uint32_t, 5>> variables; // {klass, elements, name, offset, stride}
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
};

struct PassInfo {
    std::uint32_t layer = 0;
    std::uint64_t sort_key = 0;
    std::uint32_t value = 0;
    std::uint8_t instanced = 0;
};

struct ShaderData {
    std::uint32_t name = 0;
    std::uint32_t resource_size = 0;
    std::vector<std::array<std::uint32_t, 4>> resources; // {name, type, offset, extra}
    std::vector<ConstantBufferReflection> constant_buffers;
    std::vector<std::array<std::uint32_t, 7>> records;
    std::vector<PassInfo> passes;
    std::array<std::uint32_t, 2> tail{};
};

struct ShaderContext {
    std::uint32_t name = 0;
    std::uint32_t reserved = 0;
    std::vector<std::array<std::uint32_t, 2>> shaders; // {shader name, condition offset or 0xffffffff}
};

struct ShaderSection {
    std::uint32_t version = 0x2b;
    std::uint32_t name = 0;
    std::vector<ShaderContext> contexts;
    std::vector<std::uint8_t> conditions; // condition_language bytecode, kept as is
    std::vector<std::uint64_t> render_config;
    std::vector<ShaderData> shader_data;
    std::vector<DeviceShader> device_data;
    std::vector<std::uint8_t> default_data;
};

bool decode_shader_section(const std::vector<std::uint8_t>& bytes, ShaderSection& out, std::string& error);
std::vector<std::uint8_t> encode_shader_section(const ShaderSection& section);
// Summary for --shader: contexts, passes with their stages (compressed / uncompressed sizes, hash) and bindings.
json::Value describe_shader_section(const ShaderSection& section);
json::Value describe_shader_variant(const ShaderVariant& variant);

}
