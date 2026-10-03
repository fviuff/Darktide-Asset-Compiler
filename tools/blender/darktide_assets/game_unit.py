"""Import the nodes of any game unit (gear, weapon parts, props, characters) as named empties.

The game finds attach points (ap_*), effect points (fx_*) and animated nodes by name, so a replacement unit
has to carry the same names in the same places. Names come from every BONES file in the extract folder plus
common attach/effect words; a node whose name is unknown keeps its hash as "#1234abcd", which the compiler
writes back as that hash.
"""
import glob
import os

import bpy
from mathutils import Matrix

from .reference_skeleton import ReferenceSkeletonError, _murmur64, _parse_bones, _parse_unit

_PART_WORDS = (
    "barrel", "muzzle", "magazine", "sight", "stock", "grip", "receiver", "flashlight", "bayonet", "bullet",
    "rail", "underbarrel", "trinket", "handle", "body", "blade", "head", "shaft", "pommel", "guard", "chain",
    "emblem", "scope", "lens", "slide", "hammer", "trigger", "bolt", "charge", "shield", "sling", "strap",
    "anim", "recharge", "release", "safety", "light", "eject", "reload", "overheat", "sweep", "block",
)
_EXTRA_NAMES = (
    "root_point", "lod", "lod_shadow", "ap_emblem_left", "ap_emblem_right", "ap_emblem_front", "ap_emblem_back",
    "fx_muzzle", "fx_eject", "fx_reload", "fx_overheat", "fx_sweep", "fx_block", "fx_special_active",
    "fx_weapon_special", "fx_charge", "fx_blade", "fx_shaft", "fx_shield", "fx_emit", "fx_source", "fx_base",
    "fx_display", "fx_data", "fx_left", "fx_right", "fx_center", "fx_engine", "fx_wpn_node_1", "fx_wpn_node_2",
)
_name_cache = {}


def _id32(name):
    return _murmur64(name) >> 32


def name_table(folder):
    """IdString32 -> name for every BONES name in the folder plus common attach/effect names."""
    key = os.path.normcase(os.path.abspath(folder))
    names = _name_cache.get(key)
    if names is None:
        names = {}
        for word in _PART_WORDS:
            for prefix in ("ap_", "fx_", "rp_"):
                for suffix in ("", "_01", "_02", "_03", "_04", "_05", "_06", "_07", "_08", "_09", "_10"):
                    names[_id32(prefix + word + suffix)] = prefix + word + suffix
        for name in _EXTRA_NAMES:
            names[_id32(name)] = name
        for path in glob.glob(os.path.join(folder, "*.bones")):
            try:
                with open(path, "rb") as source:
                    bone_names, _ = _parse_bones(source.read())
            except (OSError, ReferenceSkeletonError):
                continue
            for name in bone_names:
                names[_id32(name)] = name
        _name_cache[key] = names
    return names


def import_nodes(context, collection, folder, resource):
    """Create one empty per non-mesh node of the unit, named and placed like the game's."""
    stem = "%016x" % _murmur64(resource)
    unit_path = os.path.join(folder, stem + ".unit")
    if not os.path.isfile(unit_path):
        raise ValueError(resource + " is not in the extract folder (looked for " + stem + ".unit)")
    with open(unit_path, "rb") as source:
        nodes = _parse_unit(source.read())
    names = dict(name_table(folder))
    part = resource.rsplit("/", 1)[-1]
    names[_id32("rp_" + part)] = "rp_" + part

    taken = [names.get(node["hash"], "#%08x" % node["hash"]) for node in nodes if not node["mesh"]]
    clashes = sorted(name for name in taken if name in bpy.data.objects)
    if clashes:
        raise ValueError("Objects with these names already exist (the names must stay exact): " +
                         ", ".join(clashes[:6]))

    empties = {}
    for node in nodes:
        if node["mesh"]:
            continue
        name = names.get(node["hash"], "#%08x" % node["hash"])
        empty = bpy.data.objects.new(name, None)
        empty.empty_display_type = "ARROWS"
        empty.empty_display_size = 0.03
        collection.objects.link(empty)
        empties[node["index"]] = empty
    for node in nodes:
        empty = empties.get(node["index"])
        if empty is None:
            continue
        parent_type, parent_index = node["parent_type"], node["parent_index"]
        # mesh nodes are not imported; hang a child of one on its nearest imported ancestor
        for _ in range(len(nodes)):
            if parent_type != 1 or parent_index in empties:
                break
            parent_type, parent_index = nodes[parent_index]["parent_type"], nodes[parent_index]["parent_index"]
        if node["index"] and parent_type == 1 and parent_index in empties:
            empty.parent = empties[parent_index]
        values = node["world"]
        empty.matrix_world = Matrix(tuple(tuple(values[row + column * 4] for column in range(4))
                                          for row in range(4)))
    context.view_layer.update()
    unknown = sum(1 for empty in empties.values() if empty.name.startswith("#"))
    return len(empties), unknown
