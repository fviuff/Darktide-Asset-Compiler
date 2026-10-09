"""A new enemy breed from an asset: the game breed it behaves like (its scripts, attacks, sounds, gibbing), the
asset's unit as its body, its own name, health, speeds and hit zones. Written as a Lua file next to the compiled
asset; a mod loads it once and can then spawn the breed by name.

breeds.json lists the game's minion breeds (dev/trials/enemy-trial/breed_table.py reads them from the game Lua):
base unit, walk/run speed, hit zones and health per difficulty.
"""
import json
import os
import re

_BREEDS = None


def breeds():
    global _BREEDS
    if _BREEDS is None:
        with open(os.path.join(os.path.dirname(__file__), "breeds.json"), encoding="utf-8") as source:
            _BREEDS = json.load(source)
    return _BREEDS


def zone_names():
    names = []
    for entry in breeds().values():
        for zone in entry["hit_zones"]:
            if zone not in names and zone != "center_mass":
                names.append(zone)
    return names


def breed_for_unit(resource):
    """The game breed whose body is this unit (the plain one when several share it)."""
    matches = sorted(name for name, entry in breeds().items() if entry["base_unit"] == resource)
    return min(matches, key=len) if matches else ""


def _lua_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def health_values(options, base):
    """Health for the five difficulties: the typed list, one number scaled like the base breed's, or the base's."""
    text = options.breed_health.strip()
    base_health = breeds()[base].get("health") or [100.0] * 5
    if not text:
        return base_health
    values = [float(part) for part in text.replace(";", ",").split(",") if part.strip()]
    if len(values) == 1:
        return [round(values[0] * h / base_health[0], 3) for h in base_health]
    if len(values) != 5:
        raise ValueError("Health takes one number or five (one per difficulty)")
    return values


def hit_zones(options, actor_objects, base):
    """The breed's hit zones as [(zone, [actor names])], or None to keep the base breed's (the actors carry its
    names and none was given a zone of its own)."""
    base_zones = breeds()[base]["hit_zones"]
    zone_of = {}
    for zone, actors in base_zones.items():
        if zone != "center_mass":
            for actor in actors:
                zone_of.setdefault(actor, zone)
    names = {}
    custom = False
    for obj in actor_objects:
        actor = obj.dt_collider.actor_name.strip() or obj.name
        zone = obj.dt_collider.hit_zone_group
        if zone == "auto":
            zone = zone_of.get(actor, "")
        else:
            custom = True
        if not zone:
            if obj.dt_collider.unit_actor == "hit_zone":
                raise ValueError(actor + " isn't in any of " + base + "'s hit zones; give it a zone (Hit zone group)")
            continue    # an actor of another kind the breed doesn't count hits on
        names[actor] = zone
    base_actors = {actor for actor in zone_of}
    if not custom and set(names) == base_actors:
        return None
    zones = []
    for zone in list(base_zones) + [zone for zone in names.values() if zone not in base_zones]:
        if zone == "center_mass" or any(zone == z for z, _ in zones):
            continue
        actors = [actor for actor, z in names.items() if z == zone]
        if actors:
            zones.append((zone, actors))
    center = [actor for actor in base_zones.get("center_mass", []) if actor in names]
    zones.append(("center_mass", center or [actor for actor, z in names.items() if z == "torso"]))
    return zones


def validate(options):
    name = options.breed_name.strip()
    if not re.fullmatch(r"[a-z][a-z0-9_]*", name):
        raise ValueError("Breed name: lowercase letters, digits and _ only, e.g. my_walker")
    if name in breeds():
        raise ValueError("Breed name " + name + " is the game's own; pick a new one")
    if options.breed_base not in breeds():
        raise ValueError("Pick the game breed your enemy behaves like")


def lua(options, resource, actor_objects):
    validate(options)
    name, base = options.breed_name.strip(), options.breed_base
    health = health_values(options, base)
    zones = hit_zones(options, actor_objects, base)
    lines = [
        "-- The " + name + " enemy: behaves like the game's " + base + ", with " + resource + " as its body.",
        "-- Load it once from your mod (it returns the breed name), with the unit's package loaded before one spawns:",
        "--   local breed_name = mod:io_dofile(\"YourMod/scripts/mods/YourMod/" + name + "_breed\")",
        "--   Managers.state.minion_spawn:spawn_minion(breed_name, position, rotation, 2)",
        "local name = " + _lua_string(name),
        "local base = " + _lua_string(base),
        "local Breeds = require(\"scripts/settings/breed/breeds\")",
        "local BreedActions = require(\"scripts/settings/breed/breed_actions\")",
        "local DialogueBreedSettings = require(\"scripts/settings/dialogue/dialogue_breed_settings\")",
        "local MinionDifficultySettings = require(\"scripts/settings/difficulty/minion_difficulty_settings\")",
        "local NetworkLookup = require(\"scripts/network_lookup/network_lookup\")",
        "",
        "-- the network lists of names are built once at load; a new name goes on the end",
        "local function add_name(lookup)",
        "    if rawget(lookup, name) == nil then",
        "        lookup[#lookup + 1] = name",
        "        rawset(lookup, name, #lookup)",
        "    end",
        "end",
        "",
        "if not Breeds[name] then",
        "    local breed = table.clone(Breeds[base])",
        "    breed.name = name",
        "    breed.base_unit = " + _lua_string(resource),
    ]
    if options.breed_walk_speed > 0:
        lines.append("    breed.walk_speed = %g" % options.breed_walk_speed)
    if options.breed_run_speed > 0:
        lines.append("    breed.run_speed = %g" % options.breed_run_speed)
    if options.breed_body == "own":
        lines += [
            "    -- own body: the game breed's weapons only; its wounds, gibbing and dissolving work on its body pieces",
            "    for _, inventories in pairs(breed.inventory) do",
            "        for _, inventory in ipairs(inventories) do",
            "            for slot_name, slot in pairs(inventory.slots) do",
            "                if not slot.is_weapon then",
            "                    inventory.slots[slot_name] = nil",
            "                end",
            "            end",
            "        end",
            "    end",
            "    breed.use_wounds = false",
            "    breed.gib_template = nil",
            "    breed.dissolve_config = nil",
        ]
    if zones is not None:
        lines.append("    breed.hit_zones = {")
        for zone, actors in zones:
            lines.append("        { name = %s, actors = { %s } }," % (_lua_string(zone), ", ".join(_lua_string(a) for a in actors)))
        lines.append("    }")
    lines += [
        "    Breeds[name] = breed",
        "    -- what the game looks up by breed name",
        "    BreedActions[name] = BreedActions[base]",
        "    DialogueBreedSettings[name] = DialogueBreedSettings[base]",
        "    MinionDifficultySettings.health[name] = { %s }" % ", ".join("%g" % value for value in health),
        "    add_name(NetworkLookup.breed_names)",
        "    add_name(NetworkLookup.dialogue_breed_names)",
        "end",
        "",
        "return name",
        "",
    ]
    return "\n".join(lines)
