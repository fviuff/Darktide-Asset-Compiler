#pragma once

#include "gltf/json_value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::unit {

// UNIT script data (what Unit.get_data(unit, key, ...) reads): a JSON object of booleans, numbers, strings,
// nested objects and arrays. Arrays are stored with index keys 0, 1, ... (Lua reads them from index 1).
// Binary: {u32 first entry (0xffffffff = none), u32 0}, entries {u32 key IdString32, u32 next sibling, u32 type
// (1 bool u32, 2 number f32, 3 string NUL-terminated padded to 4, 0xffffffff table: u32 first child), u32 size,
// value}, written in pre-order like the game's units (all 2866 with data re-encode identically).
// Object keys starting with '#' are raw 8-digit hex ids.
bool encode_script_data(const json::Value& data, std::vector<std::uint8_t>& out, std::string& error);
bool decode_script_data(const std::vector<std::uint8_t>& bytes, json::Value& data, std::string& error);

}
