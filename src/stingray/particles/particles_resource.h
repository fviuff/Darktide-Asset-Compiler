#pragma once

#include "gltf/json_value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::particles {

// The game's particle effect resource (version 102) and its JSON description: effect settings, variables and
// systems (particle channels, initializers, simulators, visualizers with vertex writers), named after the
// Stingray particle editor's components. decode() and encode() are exact inverses for every effect the game ships.
bool decode(const std::vector<std::uint8_t>& body, json::Value& effect, std::string& error);
bool encode(const json::Value& effect, std::vector<std::uint8_t>& body, std::string& error);

// The component types (initializers, simulators, vertex writers, visualizers) and their fields with kinds:
// channel, vertex, uint, int, float, byte, bool, curve, gradient, vector3, name, resource, bytes, and the
// lists bursts, control_points, points. Editors build their forms from it.
json::Value schema();

// Resources an effect description names by id ("#<16 hex>"): the materials its visualizers draw with and the
// units mesh particles spawn.
struct Reference { std::string type; std::uint64_t id; };
std::vector<Reference> resource_references(const json::Value& effect);

}
