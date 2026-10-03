#pragma once
#include "scene/scene.h"
#include "stingray/physics/physx_cooking.h"
#include "stingray/unit/unit_resource.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dtglb::stingray::unit {
struct WriteOptions {
    // Cooked UNIT identity. Empty writes the raw v115 body, primarily for corpus tests.
    std::string resource_name;
    bool cooked_envelope = true;

    // Existing Darktide material path applied to all slots when non-empty.
    std::string material_override;
    // Optional slot-name -> material-resource path mapping. Entries take precedence
    // over material_override and are used by the generated-material pipeline.
    std::vector<std::pair<std::string, std::string>> material_bindings;
    // Optional pre-lowered material slot for each primitive. Empty computes slots from source indices.
    std::vector<std::string> primitive_material_slots;

    // Optional external BONES resource referenced by the UNIT footer.
    std::string skeleton_resource;
    // Optional same-identity STATE_MACHINE referenced by animation_state_machine.
    std::string animation_state_machine_resource;
    // Optional embedded simple animation: an ANIMATION body (no resource envelope)
    // whose tracks follow the joints of scene skin simple_animation_skin.
    std::vector<std::uint8_t> simple_animation;
    std::size_t simple_animation_skin = 0;

    // Optional donor-free primitive collider fitted to the emitted asset.
    std::optional<physics::FittedActorOptions> fitted_physics;
    // Consume scene-authored compound collision unless physics was disabled.
    // An explicit fitted_physics override takes precedence.
    bool authored_physics = true;
    // Ragdoll handoff: collection-only bodies, dynamic ones disabled until the
    // state machine's ragdoll state creates them (no duplicate UNIT records).
    bool ragdoll_handoff = false;

    // Ordinary visible inline mesh, as the game's own units write it.
    std::uint32_t mesh_flags = 0x000c2003u;
};

bool build_unit_v115(const Scene& scene, UnitResource& out, std::string& error,
                    const WriteOptions& options = {});
bool write_unit_v115(const Scene& scene, const std::filesystem::path& path,
                     std::string& error, const WriteOptions& options = {});
}
