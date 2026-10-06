#pragma once

#include "gltf/json_value.h"

#include <string>

namespace dtglb::stingray::flow {

// An authored unit flow graph:
//   {"nodes": [{"id": "fx", "type": "particle_effect", "inputs": {"effect": "content/fx/...", "object": "fx_01"}},
//              {"id": "open", "type": "external_event", "event": "open_lid"}, ...],
//    "links": [["open.out", "fx.create"], ...],          // event links: <node>.<output event> -> <node>.<input event>
//    "connections": [["fx.effect_id", "other.input"]]}  // data links: <node>.<output> -> <node>.<input>
// Node types (from the game's flow node handlers): unit_spawned, external_event, particle_effect,
// set_unit_visibility, animation_event, delay, once, gate, and lua: {"type": "lua", "function": "name", "class":
// "FlowCallbacks" (default), "params": {"name": "kind"}, "returns": {"name": "kind"}, "events": ["out", ...]} calls
// Class.function(params) (params.node_id is added by the game); the returned table fills the returns and fires an
// output event set true ("out" fires unless set false). Inputs not given and not connected stay unset (the
// node's default); every unit input left open reads the unit itself (the unit_spawned slot, always present).
// build_graph() turns it into the codec's description (flow_resource.h: nodes with type numbers and payload
// words, external events, dynamic data with the constants baked in).
bool build_graph(const json::Value& authored, json::Value& graph, std::string& error);

// The node types with their inputs (name, kind), outputs, input events and output events, for editors.
json::Value authoring_schema();

}
