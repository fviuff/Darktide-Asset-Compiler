#pragma once

#include "gltf/json_value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::flow {

// A unit's compiled flow graph (the UNIT `flow` and `flow_dynamic_data` byte arrays) and its JSON description:
// external events (what Unit.flow_event(unit, name) triggers: {event, node, in}), nodes ({type, words}: the
// node type number and its payload words, mostly byte offsets into the dynamic data), the variable, query,
// subroutine and list tables, and the dynamic data (variable storage and constants) as hex.
// decode() and encode() are exact inverses for every unit flow the game ships.
bool decode(const std::vector<std::uint8_t>& flow, const std::vector<std::uint8_t>& dynamic_data,
            json::Value& graph, std::string& error);
bool encode(const json::Value& graph, std::vector<std::uint8_t>& flow, std::vector<std::uint8_t>& dynamic_data,
            std::string& error);

}
