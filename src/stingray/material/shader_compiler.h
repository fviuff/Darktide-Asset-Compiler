#pragma once

#include "stingray/material/shader_section.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::material {

// Compiles HLSL with Microsoft's DirectX shader compiler (dxcompiler.dll + dxil.dll, found next to this program or
// in the Windows SDK) the way the game's shaders are built: shader model 6.0 DXIL, validator 1.7, reflection kept.
// profile is e.g. "vs_6_0" / "ps_6_0".
bool compile_hlsl(const std::string& source, const std::string& source_name, const std::string& entry,
                  const std::string& profile, std::vector<std::uint8_t>& dxbc, std::string& error);

// Root parameter names of one shader group (a shader_data entry and its passes): the game numbers every distinct
// constant buffer / resource name of a group in the order its passes and stages first bind it (dxc reflection order).
using RootTable = std::vector<std::uint32_t>;

// A pass stage from compiled DXBC: Kraken-compressed bytecode, its hash, and the binding records read from the
// shader's reflection (constant buffers, SRVs, UAVs, bindless arrays, samplers, input signature). Build the stages
// of a group in pass and stage order with one RootTable.
bool make_shader_variant(const std::vector<std::uint8_t>& dxbc, RootTable& roots, ShaderVariant& out,
                         std::string& error);

}
