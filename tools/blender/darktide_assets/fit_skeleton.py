"""Fit an arbitrary rigged character onto a native Darktide skeleton, plus starter templates.

Workflow: Auto-map bones (source bone -> reference bone rows), then Fit to skeleton. The fit
warps every skinned vertex (and shape key) into the reference rest pose, rebuilds the armature
with native bones plus grafted extra bones, and renames vertex groups. The ordinary Build then
compiles the result against the preset's reference BONES.
"""

import json
import math
import os
import re

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector
from bpy.props import CollectionProperty, EnumProperty, IntProperty, StringProperty

from .reference_skeleton import ReferenceSkeletonError, load_reference
from .skeleton_presets import STARTER_PRESETS


# --------------------------------------------------------------------------------------
# Property groups
# --------------------------------------------------------------------------------------

class DarktideBoneMapRow(bpy.types.PropertyGroup):
    source_bone: StringProperty(name="Source bone")
    target_bone: StringProperty(name="Darktide bone", description="Bone of the reference skeleton; empty leaves the bone unmapped")


class DarktideReferenceBoneName(bpy.types.PropertyGroup):
    name: StringProperty(name="Name")


# --------------------------------------------------------------------------------------
# Names
# --------------------------------------------------------------------------------------

def sanitize_name(name, taken):
    """Native bone identity: lowercase [a-z0-9_], unique against `taken` (which is updated)."""
    base = re.sub(r"[^a-z0-9_]", "_", name.lower()) or "extra"
    result, suffix = base, 1
    while result in taken:
        result = base + "_" + str(suffix)
        suffix += 1
    taken.add(result)
    return result


def refresh_reference_names(identity):
    """Fill the target-bone search list from the loaded reference; returns the node list."""
    nodes = load_reference_nodes(identity)
    identity.reference_bone_names.clear()
    for node in nodes:
        identity.reference_bone_names.add().name = node["name"]
    return nodes


def load_reference_nodes(identity):
    unit = bpy.path.abspath(identity.reference_unit.strip())
    bones = bpy.path.abspath(identity.reference_bones.strip())
    resource = identity.skeleton_resource.strip()
    if not unit or not bones or not resource:
        raise ValueError("Choose a skeleton preset (or a reference UNIT, BONES and resource) first")
    if not os.path.isfile(unit) or not os.path.isfile(bones):
        raise ValueError("Reference UNIT or BONES file was not found")
    try:
        return load_reference(unit, bones, resource)[0]
    except (OSError, ReferenceSkeletonError) as exc:
        raise ValueError("Reference skeleton failed to load: " + str(exc)[:160]) from exc


# --------------------------------------------------------------------------------------
# Auto-map heuristics (pure python: operates on names, parent indices)
# --------------------------------------------------------------------------------------

_SIDE_TOKENS = {"l": "left", "left": "left", "lt": "left", "lft": "left",
                "r": "right", "right": "right", "rt": "right", "rgt": "right"}
_COMPOUNDS = {("upper", "arm"): "upperarm", ("lower", "arm"): "lowerarm", ("fore", "arm"): "forearm",
              ("up", "arm"): "upperarm", ("up", "leg"): "upleg", ("upper", "leg"): "upperleg",
              ("lower", "leg"): "lowerleg", ("low", "leg"): "lowerleg", ("toe", "base"): "toebase"}
_EXCLUDE = {"twist", "roll", "jiggle", "cloth", "skirt", "cape", "hair", "ik", "pole", "ctrl", "control",
            "helper", "hlp", "end", "nub", "tip", "prop", "prp", "weapon", "effect", "fx", "mvm", "medal",
            "dyn", "phys", "corrective", "jaw", "eye", "eyes", "eyelid", "lid", "brow", "lip", "lips",
            "cheek", "ear", "tongue", "teeth", "nose", "twistroll", "bulge", "fk", "mouth", "chin", "face"}
_ARM = {"clavicle": 0, "collar": 0, "collarbone": 0, "shoulder": 0, "scapula": 0,
        "upperarm": 1, "uparm": 1, "humerus": 1,
        "forearm": 2, "lowerarm": 2, "loarm": 2, "elbow": 2, "radius": 2,
        "hand": 3, "wrist": 3, "palm": 3, "arm": None}
_LEG = {"thigh": 0, "upleg": 0, "upperleg": 0, "femur": 0,
        "calf": 1, "shin": 1, "lowerleg": 1, "lowleg": 1, "knee": 1,
        "foot": 2, "ankle": 2, "toe": 3, "toes": 3, "ball": 3, "toebase": 3, "leg": None}
_FINGERS = {"thumb": "thumb", "index": "index", "pointer": "index", "middle": "middle", "ring": "ring",
            "pinky": "pinky", "pinkie": "pinky", "little": "pinky"}
_FINGER_ORDER = ("thumb", "index", "middle", "ring", "pinky")
_SPINE = {"spine", "back", "chest", "abdomen", "torso", "waist", "belly"}
_ARM_LADDER = ("shoulder", "arm", "forearm", "hand")
_LEG_LADDER = ("upleg", "leg", "foot", "toebase")


# MMD's standard bone names (Japanese) in the words the rules below read. Controls without weights (center, groove,
# waist, IK, shoulder P/C, dummy, twist) get a word the rules leave out; "D" leg bones carry the leg weights in models
# that have them, so the plain leg bones are left out there (see _mmd_english).
_MMD_WORDS = {
    "全ての親": "root ctrl", "センター": "center ctrl", "センター2": "center ctrl", "グルーブ": "groove ctrl",
    "腰": "waist ctrl", "腰キャンセル": "waist ctrl", "下半身": "hips", "上半身": "spine", "上半身1": "spine",
    "上半身2": "chest", "上半身3": "chest", "首": "neck", "頭": "head", "両目": "eyes", "目": "eye",
    "肩": "shoulder", "肩P": "shoulder ctrl", "肩C": "shoulder ctrl", "腕": "upperarm", "腕捩": "upperarm twist",
    "ひじ": "elbow", "手捩": "forearm twist", "手首": "wrist", "ダミー": "dummy ctrl",
    "親指0": "thumb finger 0", "親指1": "thumb finger 1", "親指2": "thumb finger 2",
    "人指1": "index finger 1", "人指2": "index finger 2", "人指3": "index finger 3",
    "中指1": "middle finger 1", "中指2": "middle finger 2", "中指3": "middle finger 3",
    "薬指1": "ring finger 1", "薬指2": "ring finger 2", "薬指3": "ring finger 3",
    "小指1": "little finger 1", "小指2": "little finger 2", "小指3": "little finger 3",
    "足": "thigh", "ひざ": "knee", "足首": "ankle", "つま先": "toe", "足D": "thigh", "ひざD": "knee",
    "足首D": "ankle", "足先EX": "toe", "足IK": "leg ik", "つま先IK": "toe ik",
}
_MMD_D_BONES = {"足": "足D", "ひざ": "ひざD", "足首": "足首D", "つま先": "足先EX"}
_FULL_WIDTH = str.maketrans("０１２３４５６７８９ＩＫＤＥＸＰＣ", "0123456789IKDEXPC")


def _mmd_english(names):
    """English words for MMD-named bones (others unchanged): 左腕 -> left upperarm, 右ひざD -> right knee. mmd tools
    renames the sides by default (左腕 -> 腕.L), read the same way."""
    plain = [name.translate(_FULL_WIDTH).strip() for name in names]
    present = set(plain)
    out = []
    for name in plain:
        side, body, styled = "", name, (lambda b: b)
        if name[:1] in ("左", "右"):
            side, body = ("left " if name[0] == "左" else "right "), name[1:]
            styled = (lambda b, s=name[0]: s + b)
        elif name[-1:] in ("左", "右"):
            side, body = ("left " if name[-1] == "左" else "right "), name[:-1]
        elif re.search(r"[._][LR]$", name):
            side, body = ("left " if name[-1] == "L" else "right "), name[:-2]
            styled = (lambda b, s=name[-2:]: b + s)
        body = body.rstrip("_.")
        words = _MMD_WORDS.get(body)
        if words is None and body[:-1] in ("腕捩", "手捩"):          # 腕捩1, 腕捩2 ... twist helpers
            words = _MMD_WORDS[body[:-1]]
        if words is None:
            out.append(name)
            continue
        twin = _MMD_D_BONES.get(body)
        if twin and styled(twin) in present:
            words = "fk ctrl"      # the D bone next to it carries the weights
        out.append(side + words)
    return out


def _tokens(name):
    text = re.sub(r"([a-z])([A-Z])", r"\1 \2", name)
    text = re.sub(r"([A-Za-z])(\d)", r"\1 \2", text)
    text = re.sub(r"(\d)([A-Za-z])", r"\1 \2", text)
    tokens = []
    for token in re.split(r"[^A-Za-z0-9]+", text.lower()):
        if not token:
            continue
        for prefix in ("left", "right"):
            if token.startswith(prefix) and len(token) > len(prefix):
                tokens.extend((prefix, token[len(prefix):]))
                break
        else:
            tokens.append(token)
    merged, i = [], 0
    while i < len(tokens):
        pair = (tokens[i], tokens[i + 1]) if i + 1 < len(tokens) else None
        if pair in _COMPOUNDS:
            merged.append(_COMPOUNDS[pair])
            i += 2
        else:
            merged.append(tokens[i])
            i += 1
    return merged


# names are useless half the time (ARM2L, HANDL, R_foreArmTw001_jnt...), the hierarchy usually isnt
def _structural_limbs(names, parent, children, excluded, side, heads, hips_index, spine_set, assign):
    """Map arms, hands, fingers and legs from the hierarchy (names often don't say which is which).

    A hand is the bone where three or more multi-joint chains start (fingers). An arm runs from a
    spine child down to its hand; a leg is one of the two deepest chains below the hips.
    """
    count = len(parent)
    below = [0] * count           # longest non-excluded chain length below each bone
    for i in sorted(range(count), key=lambda k: -len(_ancestors(parent, k))):
        kids = [c for c in children[i] if not excluded[c]]
        below[i] = 1 + max((below[c] for c in kids), default=0) if not excluded[i] else 0

    def chains(i):
        return [c for c in children[i] if not excluded[c] and below[c] >= 2]

    def lateral(i):
        s = side[i]
        if s:
            return s
        if heads and hips_index is not None:
            return "left" if heads[i][0] > heads[hips_index][0] else "right"
        return None

    # spine_set: every bone on the hips -> neck path (the torso); limbs hang off it.
    torso = set(spine_set)
    for i in list(spine_set):
        torso.update(_ancestors(parent, i))
    hands = [i for i in range(count) if not excluded[i] and i not in torso
             and len(chains(i)) >= 3 and all(below[c] <= 4 for c in chains(i))]

    def path_to(root, target):
        path, j = [], target
        while j >= 0 and j != root:
            path.append(j)
            j = parent[j]
        return list(reversed(path + [root])) if j == root else None

    # Arms: the chain from a spine bone's child to a hand.
    for hand in hands:
        j, root = hand, None
        while parent[j] >= 0:
            if parent[j] in torso:
                root = j
                break
            j = parent[j]
        if root is None:
            continue
        chain = [k for k in path_to(root, hand) if not excluded[k]]
        s = lateral(root) or lateral(hand)
        if not s or len(chain) < 3:
            continue
        ladder = (["shoulder"] if len(chain) >= 4 else []) + ["arm", "forearm", "hand"]
        picks = ([chain[0]] if len(chain) >= 4 else []) + [chain[-3], chain[-2], chain[-1]]
        for bone, part in zip(picks, ladder):
            assign(bone, "j_" + s + part)
        # Fingers named only by number (e.g. R1_fing1..R5_fing1): the hand's chains that share
        # one name pattern apart from digits, 4-5 of them with distinct first digits, ordered
        # 1..5 = thumb..pinky. Named fingers (thumb/index/...) are left to the name rules.
        groups = {}
        for k in chains(hand):
            groups.setdefault(re.sub(r"\d", "", names[k]).lower(), []).append(k)
        numbered = [g for g in groups.values() if 4 <= len(g) <= 5 and
                    len({re.findall(r"\d", names[k])[0] for k in g if re.findall(r"\d", names[k])}) == len(g)]
        if not numbered:
            continue
        fingers = sorted(numbered[0], key=lambda k: int(re.findall(r"\d", names[k])[0]))
        finger_names = _FINGER_ORDER if len(fingers) >= 5 else _FINGER_ORDER[1:]
        for finger, key in zip(fingers, finger_names):
            j = finger
            for n in range(1, 4):
                assign(j, "j_%shand%s%d" % (s, key, n))
                nxt = [c for c in children[j] if not excluded[c]]
                if not nxt:
                    break
                j = max(nxt, key=lambda c: below[c])

    # Legs: the two deepest chains below the hips that are not the spine.
    if hips_index is not None:
        roots = [c for c in children[hips_index] if not excluded[c] and c not in spine_set and below[c] >= 3]
        roots.sort(key=lambda c: -below[c])
        for root in roots[:2]:
            s = lateral(root)
            if not s:
                continue
            j = root
            for part in ("upleg", "leg", "foot", "toebase"):
                assign(j, "j_" + s + part)
                nxt = [c for c in children[j] if not excluded[c]]
                if not nxt:
                    break
                j = max(nxt, key=lambda c: below[c])


def _ancestors(parent, i):
    out, j = [], parent[i]
    while j >= 0:
        out.append(j)
        j = parent[j]
    return out


def auto_map_bones(bones, ref_names, positions=None):
    """bones: list of (name, parent_index or -1); positions: optional armature-space head
    positions. Returns {bone_index: target_name}."""
    count = len(bones)
    parent = [b[1] for b in bones]
    children = [[] for _ in range(count)]
    for i, p in enumerate(parent):
        if p >= 0:
            children[p].append(i)
    depth = [0] * count
    for i in range(count):
        j = parent[i]
        while j >= 0:
            depth[i] += 1
            j = parent[j]
    toks = [_tokens(name) for name in _mmd_english([b[0] for b in bones])]
    excluded = [bool(set(t) & _EXCLUDE) for t in toks]
    ref = set(ref_names)
    assigned, used = {}, set()

    def assign(i, target):
        if i not in assigned and target in ref and target not in used:
            assigned[i] = target
            used.add(target)

    lower_ref = {}
    for name in ref_names:
        lower_ref.setdefault(name.lower(), []).append(name)
    for i, (name, _p) in enumerate(bones):
        if name in ref:
            assign(i, name)
    for i, (name, _p) in enumerate(bones):
        matches = lower_ref.get(name.lower(), ())
        if len(matches) == 1:
            assign(i, matches[0])

    # Family and side per bone.
    side = [next((_SIDE_TOKENS[t] for t in tk if t in _SIDE_TOKENS), None) for tk in toks]
    family = [None] * count   # (kind, explicit rank or None)
    finger = [None] * count   # (finger key or ("group", digit))
    def under_hand(i):
        j = parent[i]
        while j >= 0:
            if set(toks[j]) & {"hand", "wrist", "palm"}:
                return True
            j = parent[j]
        return False

    for i, tk in enumerate(toks):
        if excluded[i] or i in assigned:
            continue
        tset = set(tk)
        key = next((_FINGERS[t] for t in tk if t in _FINGERS), None)
        # index / middle / ring are also face, jewellery and helper words: a finger says so or hangs under a hand
        if key is not None and not ("finger" in tset or key in ("thumb", "pinky") or under_hand(i)):
            key = None
        if key is None and "finger" in tset:
            numbers = [t for t in tk if t.isdigit()]
            key = ("group", int(numbers[0][0])) if numbers else None
        if key is not None:
            family[i], finger[i] = ("finger", None), key
        elif "pelvis" in tset:
            family[i] = ("hips", 100)
        elif "head" in tset:
            family[i] = ("head", None)
        elif "neck" in tset:
            family[i] = ("neck", None)
        elif "hip" in tset and side[i]:
            family[i] = ("leg", 0)
        elif "hips" in tset or "hip" in tset:
            family[i] = ("hips", 90)
        else:
            leg = [_LEG[t] for t in tk if t in _LEG and (_LEG[t] is not None or t == "leg")]
            arm = [_ARM[t] for t in tk if t in _ARM]
            explicit_leg = [r for r in leg if r is not None]
            explicit_arm = [r for r in arm if r is not None]
            if explicit_arm:
                family[i] = ("arm", explicit_arm[0])
            elif explicit_leg:
                family[i] = ("leg", explicit_leg[0])
            elif "arm" in tset:
                family[i] = ("arm", None)
            elif "leg" in tset:
                family[i] = ("leg", None)
            elif tset & _SPINE:
                family[i] = ("spine", None)

    limb = {"arm", "leg", "finger"}

    def resolve_side(i):
        if side[i]:
            return side[i]
        j = parent[i]
        while j >= 0:
            if family[j] and family[j][0] in limb and side[j]:
                return side[j]
            j = parent[j]
        queue = list(children[i])
        while queue:
            j = queue.pop(0)
            if family[j] and family[j][0] in limb and side[j]:
                return side[j]
            queue.extend(children[j])
        return None

    for i in range(count):
        if family[i] and family[i][0] in limb and not side[i]:
            side[i] = resolve_side(i)

    by_depth = sorted(range(count), key=lambda i: (depth[i], i))

    # Hips: pelvis/hips names, else the common parent of the two upper legs.
    hips = [(-family[i][1], depth[i], i) for i in by_depth if family[i] and family[i][0] == "hips"]
    upleg_parents = set()

    # Limb ladders (shoulder-arm-forearm-hand, upleg-leg-foot-toe).
    limb_cands = []
    for kind, ladder in (("arm", _ARM_LADDER), ("leg", _LEG_LADDER)):
        for s in ("left", "right"):
            members = [i for i in by_depth if family[i] and family[i][0] == kind and side[i] == s]
            member_set = set(members)
            rank = {}
            for i in members:
                j = parent[i]
                while j >= 0 and j not in member_set:
                    j = parent[j]
                base = rank[j] + 1 if j >= 0 else 0
                explicit = family[i][1]
                rank[i] = max(explicit, base) if explicit is not None else base
            for i in reversed(members):
                if family[i][1] is None:
                    kids = []
                    stack = list(children[i])
                    while stack:
                        k = stack.pop()
                        if k in member_set:
                            kids.append(rank[k])
                        else:
                            stack.extend(children[k])
                    if kids:
                        rank[i] = max(rank[i], min(kids) - 1)
            for i in members:
                if rank[i] < len(ladder):
                    target = "j_" + s + ladder[rank[i]]
                    limb_cands.append((0 if family[i][1] is not None else 1, depth[i], i, target))
            if kind == "leg":
                roots = [i for i in members if rank[i] == 0]
                if roots:
                    upleg_parents.add((s, parent[min(roots, key=lambda i: depth[i])]))

    structural = None
    if len(upleg_parents) == 2:
        (_, a), (_, b) = sorted(upleg_parents)
        if a == b and a >= 0 and not excluded[a]:
            structural = a
    for _score, _d, i in hips:
        assign(i, "j_hips")
    if structural is not None and structural not in assigned:
        assign(structural, "j_hips")

    # Neck and head.
    heads = [i for i in by_depth if family[i] and family[i][0] == "head"]
    necks = [i for i in by_depth if family[i] and family[i][0] == "neck"]
    if heads:
        assign(heads[0], "j_head")
    if necks:
        assign(necks[0], "j_neck")
        if not heads and len(necks) > 1:
            below = [i for i in necks[1:]]
            assign(below[-1], "j_head")

    # Spine: bones on the path between hips and neck (or head), spread over the reference ladder.
    inverse = {target: i for i, target in assigned.items()}
    top = inverse.get("j_neck", inverse.get("j_head"))
    hips_index = inverse.get("j_hips")
    path = []
    if top is not None and hips_index is not None:
        j = parent[top]
        while j >= 0 and j != hips_index:
            path.append(j)
            j = parent[j]
        if j != hips_index:
            path = []
    if not path:
        path = [i for i in by_depth if family[i] and family[i][0] == "spine"]
    else:
        path.reverse()
    path = [i for i in path if not excluded[i] and i not in assigned]
    ladder = [n for n in ("j_spine", "j_spine1", "j_spine2") if n in ref and n not in used]
    if path and ladder:
        n, k = len(path), len(ladder)
        if n == 1 or k == 1:
            pairs = [(path[0], ladder[0])]
        elif n <= k:
            pairs = [(path[j], ladder[round(j * (k - 1) / (n - 1))]) for j in range(n)]
        else:
            pairs = [(path[round(j * (n - 1) / (k - 1))], ladder[j]) for j in range(k)]
        for i, target in pairs:
            assign(i, target)

    # Structure first (hands are where finger chains branch), names fill the rest.
    inverse = {target: i for i, target in assigned.items()}
    spine_set = {inverse[n] for n in ("j_hips", "j_spine", "j_spine1", "j_spine2", "j_neck") if n in inverse}
    if "j_neck" in inverse and "j_hips" in inverse:  # whole hips -> neck path, mapped or not
        j = parent[inverse["j_neck"]]
        while j >= 0 and j != inverse["j_hips"]:
            spine_set.add(j)
            j = parent[j]
    _structural_limbs([b[0] for b in bones], parent, children, excluded, side, positions,
                      inverse.get("j_hips"), spine_set, assign)

    for _explicit_flag, _d, i, target in sorted(limb_cands):
        assign(i, target)

    # Fingers: chain depth within a finger gives joint 1..3.
    for s in ("left", "right"):
        members = [i for i in by_depth if family[i] and family[i][0] == "finger" and side[i] == s]
        groups = sorted({finger[i][1] for i in members if isinstance(finger[i], tuple)})
        group_names = {}
        if len(groups) == 5:
            group_names = dict(zip(groups, _FINGER_ORDER))
        elif len(groups) == 4:
            group_names = dict(zip(groups, _FINGER_ORDER[1:]))
        keyed = {}
        for i in members:
            key = group_names.get(finger[i][1]) if isinstance(finger[i], tuple) else finger[i]
            if key:
                keyed.setdefault(key, []).append(i)
        for key, chain in keyed.items():
            chain_set = set(chain)
            for i in chain:
                rank, j = 0, parent[i]
                while j >= 0:
                    rank += j in chain_set
                    j = parent[j]
                if rank < 3:
                    assign(i, "j_%shand%s%d" % (s, key, rank + 1))
    return assigned


# --------------------------------------------------------------------------------------
# Source armature helpers
# --------------------------------------------------------------------------------------

def find_source_armature(context, collection):
    """The armature to fit: the active one if it is in the collection, else the only candidate."""
    candidates = [o for o in collection.all_objects if o.type == "ARMATURE"
                  and not o.get("darktide_fit_output") and "darktide_reference_unit" not in o]
    active = context.view_layer.objects.active
    if active in candidates:
        return active
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise ValueError("The asset collection has no source armature to fit")
    raise ValueError("Several armatures found; make the one to fit the active object")


def _armature_bone_list(arm):
    ordered = sorted(arm.data.bones, key=lambda b: (len(b.parent_recursive), b.name))
    index = {b.name: i for i, b in enumerate(ordered)}
    return ordered, [(b.name, index[b.parent.name] if b.parent else -1) for b in ordered]


def _lateral_pair(targets):
    for pair in (("j_leftshoulder", "j_rightshoulder"), ("j_leftarm", "j_rightarm")):
        if pair[0] in targets and pair[1] in targets:
            return pair
    return None


def missing_required(targets):
    """Names of required fit bones that no row maps to."""
    targets = set(targets)
    missing = [name for name in ("j_hips", "j_head") if name not in targets]
    if _lateral_pair(targets) is None:
        missing.append("left+right shoulder (or upper arm)")
    return missing


# --------------------------------------------------------------------------------------
# Fit
# --------------------------------------------------------------------------------------

def _unit(vector, fallback=(0.0, 0.0, 1.0)):
    return vector.normalized() if vector.length > 1e-8 else Vector(fallback)


def _np3(matrix):
    return np.array([[matrix[r][c] for c in range(3)] for r in range(3)], dtype=np.float64)


def _rest_world(obj, arm, cache):
    """World matrix of a child at the armature's rest pose (bone parents use the bone tail)."""
    if obj in cache:
        return cache[obj]
    parent = obj.parent
    if parent is None:
        result = obj.matrix_world.copy()
    elif parent == arm:
        bone = arm.data.bones.get(obj.parent_bone) if obj.parent_type == "BONE" else None
        base = arm.matrix_world
        if bone is not None:
            base = base @ bone.matrix_local @ Matrix.Translation((0.0, bone.length, 0.0))
        result = base @ obj.matrix_parent_inverse @ obj.matrix_basis
    else:
        result = _rest_world(parent, arm, cache) @ obj.matrix_parent_inverse @ obj.matrix_basis
    cache[obj] = result
    return result


def _attachment(obj, arm):
    """(True, bone name or None) when obj hangs off the armature through a parent chain."""
    bone = None
    while obj.parent is not None:
        if obj.parent == arm:
            return True, (obj.parent_bone if obj.parent_type == "BONE" and obj.parent_bone in arm.data.bones else None)
        obj = obj.parent
    return False, None


def _nearest_bone(arm, point):
    best, best_d = None, None
    for bone in arm.data.bones:
        a = arm.matrix_world @ bone.head_local
        b = arm.matrix_world @ bone.tail_local
        ab = b - a
        t = 0.0 if ab.length_squared < 1e-12 else max(0.0, min(1.0, (point - a).dot(ab) / ab.length_squared))
        d = (point - (a + ab * t)).length
        if best_d is None or d < best_d:
            best, best_d = bone.name, d
    return best


def _build_armature(context, collection, name, nodes, extras):
    """Native reference bones (native rest) plus extras [(name, parent, head, tail, zaxis)]."""
    armature = bpy.data.armatures.new(name)
    obj = bpy.data.objects.new(name, armature)
    collection.objects.link(obj)
    if context.view_layer.objects.active is not None and context.view_layer.objects.active.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    try:
        edit = armature.edit_bones
        heads, worlds, made = {}, {}, {}
        for node in nodes:
            v = node["world"]
            worlds[node["name"]] = Matrix(tuple(tuple(v[r + c * 4] for c in range(4)) for r in range(4)))
            heads[node["name"]] = Vector((v[12], v[13], v[14]))
            made[node["name"]] = edit.new(node["name"])
        kids = {node["name"]: [] for node in nodes}
        for node in nodes:
            if node["parent"]:
                kids[node["parent"]].append(node["name"])
        for node in nodes:
            bone, head = made[node["name"]], heads[node["name"]]
            length = min(((heads[c] - head).length for c in kids[node["name"]] if (heads[c] - head).length > 1e-5),
                         default=0.03)
            basis = worlds[node["name"]].to_3x3().normalized()
            bone.head = head
            bone.tail = head + basis @ Vector((0.0, 0.0, length))
            bone.align_roll(basis @ Vector((0.0, -1.0, 0.0)))
            if node["parent"]:
                bone.parent = made[node["parent"]]
        for extra_name, _parent, head, tail, zaxis in extras:
            bone = edit.new(extra_name)
            bone.head = head
            bone.tail = tail
            if zaxis.length > 1e-8 and abs(zaxis.normalized().dot((tail - head).normalized())) < 0.999:
                bone.align_roll(zaxis)
            made[extra_name] = bone
        for extra_name, parent, _head, _tail, _z in extras:
            made[extra_name].parent = made[parent]
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")
    obj["darktide_fit_output"] = True
    return obj


class FitPlan:
    """How a source rig lands on the reference skeleton: a global similarity (rot0, scale0, trans0) and per mapped
    bone a segment warp. The mesh fit and the animation retarget both use it, so animations move the fitted
    meshes the way the source rig moved the originals."""

    def __init__(self, **values):
        self.__dict__.update(values)

    def rotation(self, source_bone):
        """World rotation the fit gives the geometry carried by this source bone (3x3)."""
        warp = self.warps.get(self.carrier.get(source_bone))
        return (warp[3] @ self.rot0) if warp else self.rot0.copy()

    def place(self, point):
        """A source world point under the global similarity (used for the hips' motion)."""
        return self.rot0 @ (point * self.scale0) + self.trans0


def fit_plan(identity, arm, rows):
    """FitPlan for armature `arm` (rest pose) with rows {source bone: target bone}."""
    nodes = load_reference_nodes(identity)
    ref_pos = {n["name"]: Vector((n["world"][12], n["world"][13], n["world"][14])) for n in nodes}
    ref_root = next((n["name"] for n in nodes if not n["parent"]), nodes[0]["name"])
    bones = list(arm.data.bones)
    bone_names = {b.name for b in bones}
    mapping = {s: t for s, t in rows.items() if s in bone_names and t in ref_pos}
    missing = missing_required(mapping.values())
    if missing:
        raise ValueError("Map these required bones first: " + ", ".join(missing))
    warnings = []

    head_w = {b.name: arm.matrix_world @ b.head_local for b in bones}
    depth = {b.name: len(b.parent_recursive) for b in bones}
    src_of = {}
    for source, target in sorted(mapping.items(), key=lambda kv: (depth[kv[0]], kv[0])):
        src_of.setdefault(target, source)

    # 1. Global similarity from pelvis->head and the left/right shoulder axis.
    hips_s, head_s = head_w[src_of["j_hips"]], head_w[src_of["j_head"]]
    left_key, right_key = _lateral_pair(src_of)
    if (head_s - hips_s).length < 1e-6:
        raise ValueError("The mapped hips and head bones coincide in the source rig")
    up_s = _unit(head_s - hips_s)
    up_t = _unit(ref_pos["j_head"] - ref_pos["j_hips"])
    lat_s = head_w[src_of[left_key]] - head_w[src_of[right_key]]
    lat_t = ref_pos[left_key] - ref_pos[right_key]
    lat_s = _unit(lat_s - up_s * lat_s.dot(up_s), (1, 0, 0))
    lat_t = _unit(lat_t - up_t * lat_t.dot(up_t), (1, 0, 0))
    frame_s = Matrix((lat_s, up_s.cross(lat_s).normalized(), up_s)).transposed()
    frame_t = Matrix((lat_t, up_t.cross(lat_t).normalized(), up_t)).transposed()
    rot0 = frame_t @ frame_s.transposed()
    scale0 = (ref_pos["j_head"] - ref_pos["j_hips"]).length / (head_s - hips_s).length
    keep_proportions = identity.fit_keep_proportions
    if keep_proportions:
        # the scale that changes every mapped segment least: geometric mean of Darktide length / source length
        logs = []
        by_name = {b.name: b for b in bones}
        for source, target in mapping.items():
            p = by_name[source].parent
            while p is not None and p.name not in mapping:
                p = p.parent
            if p is None:
                continue
            length_s = (head_w[source] - head_w[p.name]).length
            length_t = (ref_pos[target] - ref_pos[mapping[p.name]]).length
            if length_s > 1e-5 and length_t > 1e-5:
                logs.append(math.log(length_t / length_s))
        if logs:
            scale0 = math.exp(sum(logs) / len(logs))
    trans0 = ref_pos["j_hips"] - rot0 @ (hips_s * scale0)
    similarity = rot0 * scale0
    q = {name: rot0 @ (p * scale0) + trans0 for name, p in head_w.items()}
    if scale0 < 0.5 or scale0 > 2.0:
        warnings.append("Source rig is %.3gx the Darktide size (pelvis-to-head); rescaled." % (1.0 / scale0))
    lateral_dot = (rot0 @ (head_w[src_of[left_key]] - head_w[src_of[right_key]]).normalized()).dot(lat_t)
    if lateral_dot < 0.9:
        warnings.append("Left/right shoulder axis only aligns to %.2f; check the L/R rows." % lateral_dot)

    # 2. Per-bone segment warp for mapped bones.
    parent = {b.name: (b.parent.name if b.parent else None) for b in bones}
    kids = {b.name: [c.name for c in b.children] for b in bones}

    def mapped_parent(name):
        p = parent[name]
        while p and p not in mapping:
            p = parent[p]
        return p

    def mapped_below(name):
        found, stack = [], list(kids[name])
        while stack:
            k = stack.pop()
            if k in mapping:
                found.append(k)
            else:
                stack.extend(kids[k])
        return found

    ref_parent = {n["name"]: n["parent"] for n in nodes}
    targets = set(mapping.values())

    def ref_incoming(target):
        """Direction the Darktide skeleton comes into this bone from its mapped parent (up for the hips)."""
        p = ref_parent.get(target)
        while p and p not in targets:
            p = ref_parent.get(p)
        return _unit(ref_pos[target] - ref_pos[p]) if p else up_t

    warps = {}
    clamped = 0
    for name, target in mapping.items():
        src, dst = q[name], ref_pos[target]
        axis, along, desired = None, 1.0, None
        candidates = [c for c in mapped_below(name) if mapping[c] != target]
        # line up with the child that carries on the Darktide skeleton's own direction (hips -> spine, hand ->
        # middle finger, chest -> neck); the farthest child tilted pelvises toward a thigh and chests toward a
        # shoulder, and the source bones' own directions are unreliable after imports
        incoming = ref_incoming(target)
        candidates.sort(key=lambda c: (_unit(ref_pos[mapping[c]] - dst).dot(incoming), (q[c] - src).length),
                        reverse=True)
        other = next((c for c in candidates
                      if (q[c] - src).length > 1e-5 and (ref_pos[mapping[c]] - dst).length > 1e-5), None)
        if identity.fit_keep_torso and target in ("j_spine", "j_spine1", "j_spine2"):
            warps[name] = (src, dst, Vector((0, 0, 1)), Matrix.Identity(3), 1.0)
            continue
        if other:
            axis = _unit(q[other] - src)
            desired = _unit(ref_pos[mapping[other]] - dst)
            ratio = (ref_pos[mapping[other]] - dst).length / (q[other] - src).length
            # was 0.35-2.5 at first, tf2 heads came out twice as tall lol
            along = max(0.7, min(1.4, ratio))
            clamped += along != ratio
        else:
            mp = mapped_parent(name)
            if mp and (src - q[mp]).length > 1e-5 and (dst - ref_pos[mapping[mp]]).length > 1e-5:
                axis = _unit(src - q[mp])
                desired = _unit(dst - ref_pos[mapping[mp]])
        if axis is None:
            axis, desired = Vector((0, 0, 1)), Vector((0, 0, 1))
        rot = axis.rotation_difference(desired).to_matrix()
        warps[name] = (src, dst, axis, rot, along)
    if clamped:
        warnings.append("%d segments needed more than the 0.7-1.4 length scale; proportions differ strongly." % clamped)
    carrier = {b.name: b.name if b.name in mapping else mapped_parent(b.name) for b in bones}
    return FitPlan(nodes=nodes, ref_pos=ref_pos, ref_root=ref_root, bones=bones, bone_names=bone_names,
                   mapping=mapping, head_w=head_w, depth=depth, src_of=src_of, rot0=rot0, scale0=scale0,
                   trans0=trans0, similarity=similarity, keep_proportions=keep_proportions, warps=warps,
                   carrier=carrier, mapped_parent=mapped_parent, warnings=warnings)


def fit_to_skeleton(context, collection, arm, rows):
    """Fit the skinned meshes of `arm` onto the collection's reference skeleton.

    rows: {source bone: target bone}. Returns (new armature object, report lines)."""
    identity = collection.dt_asset_identity
    plan_ = fit_plan(identity, arm, rows)
    nodes, ref_pos, ref_root = plan_.nodes, plan_.ref_pos, plan_.ref_root
    bones, bone_names = plan_.bones, plan_.bone_names
    parent = {b.name: (b.parent.name if b.parent else None) for b in bones}
    mapping, head_w, depth, src_of = plan_.mapping, plan_.head_w, plan_.depth, plan_.src_of
    rot0, scale0, trans0, similarity = plan_.rot0, plan_.scale0, plan_.trans0, plan_.similarity
    keep_proportions, warps, carrier = plan_.keep_proportions, plan_.warps, plan_.carrier
    mapped_parent, warnings = plan_.mapped_parent, plan_.warnings
    report = []

    # Affine per bone in source world space: point' = A p + b; normals use Rn.
    def affine(warp):
        if warp is None:
            return similarity.copy(), trans0.copy(), rot0.copy()
        origin, dst, axis, rot, along = warp
        stretch = Matrix.Identity(3)
        normal_stretch = Matrix.Identity(3)
        if keep_proportions:
            stretch = stretch * along         # the segment keeps its shape, only its size changes
        else:
            for r in range(3):
                for c in range(3):
                    stretch[r][c] += (along - 1.0) * axis[r] * axis[c]
                    normal_stretch[r][c] += (1.0 / along - 1.0) * axis[r] * axis[c]
        a = rot @ stretch
        return a @ similarity, dst + a @ (trans0 - origin), (rot @ normal_stretch) @ rot0

    mats ={name: affine(warp) for name, warp in warps.items()}
    mats[None] = affine(None)

    # 3. Output names: mapped -> target, unmapped -> sanitized unique extras.
    taken = set(ref_pos)
    new_name = {}
    extras_src = []
    for b in sorted(bones, key=lambda b: (depth[b.name], b.name)):
        if b.name in mapping:
            new_name[b.name] = mapping[b.name]
        else:
            new_name[b.name] = sanitize_name(b.name, taken)
            extras_src.append(b)

    # 4. Meshes: classify, bake rigid parts into the skin, warp.
    cache = {}
    plan = []
    skipped = []
    for obj in collection.all_objects:
        if obj.type != "MESH":
            continue
        skinned = any(m.type == "ARMATURE" and m.object == arm for m in obj.modifiers)
        attached, bone = _attachment(obj, arm)
        if skinned:
            plan.append((obj, obj.matrix_world.copy(), None))
        elif attached:
            plan.append((obj, _rest_world(obj, arm, cache), bone or "__nearest__"))
        else:
            skipped.append(obj.name)
    if not plan:
        raise ValueError("No meshes are skinned to, or parented to, the source armature")
    rigid_count = sum(1 for p in plan if p[2])
    baked_bones = {}
    unweighted_total = 0
    new_arm_name = "Darktide Fit Skeleton"
    for obj, world, rigid_bone in plan:
        if obj.data.users > 1:
            obj.data = obj.data.copy()
        mesh = obj.data
        count = len(mesh.vertices)
        co = np.empty(count * 3)
        mesh.vertices.foreach_get("co", co)
        co = co.reshape(count, 3)
        world3 = _np3(world.to_3x3())
        points = co @ world3.T + np.array(world.translation)
        if rigid_bone:
            if rigid_bone == "__nearest__":
                rigid_bone = _nearest_bone(arm, Vector(points.mean(axis=0)))
                warnings.append("%s: parented to the armature without a bone; skinned to nearest bone %s"
                                % (obj.name, rigid_bone))
            for group in list(obj.vertex_groups):
                obj.vertex_groups.remove(group)
            obj.vertex_groups.new(name=rigid_bone).add(list(range(count)), 1.0, "REPLACE")
            baked_bones[obj.name] = rigid_bone
        group_bone = {g.index: g.name for g in obj.vertex_groups}
        per_key = {}
        entries = []
        for v in mesh.vertices:
            for g in v.groups:
                name = group_bone.get(g.group)
                if name in bone_names and g.weight > 0.0:
                    per_key.setdefault(carrier[name], ([], []))
                    per_key[carrier[name]][0].append(v.index)
                    per_key[carrier[name]][1].append(g.weight)
                    entries.append((v.index, new_name[name], g.weight))
        acc_a = np.zeros((count, 3, 3))
        acc_b = np.zeros((count, 3))
        acc_n = np.zeros((count, 3, 3))
        total = np.zeros(count)
        for key, (idx, weights) in per_key.items():
            a, b, n = mats[key]
            idx, weights = np.array(idx), np.array(weights)
            np.add.at(acc_a, idx, weights[:, None, None] * _np3(a))
            np.add.at(acc_b, idx, weights[:, None] * np.array(b))
            np.add.at(acc_n, idx, weights[:, None, None] * _np3(n))
            np.add.at(total, idx, weights)
        a0, b0, n0 = mats[None]
        valid = total > 1e-9
        acc_a[valid] /= total[valid][:, None, None]
        acc_b[valid] /= total[valid][:, None]
        acc_n[valid] /= total[valid][:, None, None]
        acc_a[~valid] = _np3(a0)
        acc_b[~valid] = np.array(b0)
        acc_n[~valid] = _np3(n0)
        unweighted_total += int((~valid).sum())

        # Loop normals before touching geometry (world space, corrected per vertex).
        loop_vertex = np.empty(len(mesh.loops), dtype=np.int64)
        mesh.loops.foreach_get("vertex_index", loop_vertex)
        loop_normals = None
        if len(mesh.loops) and world.to_3x3().determinant() > 0:
            raw = np.empty(len(mesh.loops) * 3)
            mesh.corner_normals.foreach_get("vector", raw)
            normal_matrix = _np3(world.to_3x3().inverted().transposed())
            world_normals = raw.reshape(-1, 3) @ normal_matrix.T
            moved = np.einsum("nij,nj->ni", acc_n[loop_vertex], world_normals)
            length = np.linalg.norm(moved, axis=1, keepdims=True)
            loop_normals = moved / np.where(length > 1e-12, length, 1.0)

        def warp(array):
            world_points = array @ world3.T + np.array(world.translation)
            return np.einsum("nij,nj->ni", acc_a, world_points) + acc_b

        mesh.vertices.foreach_set("co", warp(co).ravel())
        if mesh.shape_keys:
            for block in mesh.shape_keys.key_blocks:
                data = np.empty(count * 3)
                block.data.foreach_get("co", data)
                block.data.foreach_set("co", warp(data.reshape(count, 3)).ravel())
        if world.to_3x3().determinant() < 0:
            mesh.flip_normals()
            warnings.append(obj.name + ": mirrored object transform; winding flipped, custom normals dropped")
        mesh.update()
        if loop_normals is not None:
            try:
                mesh.normals_split_custom_set(loop_normals.tolist())
            except (RuntimeError, TypeError, AttributeError) as exc:
                warnings.append(obj.name + ": custom normals not written (" + str(exc)[:60] + ")")

        # Rebuild vertex groups under the new names, normalized per vertex.
        sums = {}
        for vertex, name, weight in entries:
            sums.setdefault(vertex, {})
            sums[vertex][name] = sums[vertex].get(name, 0.0) + weight
        for group in list(obj.vertex_groups):
            obj.vertex_groups.remove(group)
        groups = {}
        for vertex, by_name in sums.items():
            norm = sum(by_name.values())
            for name, weight in by_name.items():
                if name not in groups:
                    groups[name] = obj.vertex_groups.new(name=name)
                groups[name].add([vertex], weight / norm, "REPLACE")
        for modifier in list(obj.modifiers):
            if modifier.type == "ARMATURE" and modifier.object == arm:
                obj.modifiers.remove(modifier)

    # 5. Output armature: native bones plus grafted extras placed by their carrier's warp.
    def place(name, point):
        a, b, _n = mats[carrier[name]]
        return a @ point + b

    extras = []
    for b in extras_src:
        head = place(b.name, head_w[b.name])
        tail_world = arm.matrix_world @ b.tail_local
        tail = place(b.name, tail_world)
        if (tail - head).length < 1e-3:
            tail = head + Vector((0.0, 0.0, 0.02))
        zaxis = mats[carrier[b.name]][0] @ (arm.matrix_world.to_3x3() @ b.matrix_local.to_3x3().col[2])
        p = parent[b.name]
        while p and p not in mapping and p not in new_name:
            p = parent[p]
        parent_name = new_name[p] if p else ref_root
        extras.append((new_name[b.name], parent_name, head, tail, zaxis))
    new_arm = _build_armature(context, collection, new_arm_name, nodes, extras)
    new_arm["darktide_reference_unit"] = bpy.path.abspath(identity.reference_unit)
    new_arm["darktide_reference_bones"] = bpy.path.abspath(identity.reference_bones)
    # what each bone was in the source rig, so its animation can be carried over later
    new_arm["darktide_fit_source"] = arm.name
    new_arm["darktide_fit_bones"] = json.dumps({new: old for old, new in new_name.items()})
    for obj, _world, _rigid in plan:
        obj.parent = None
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_basis = Matrix.Identity(4)
        obj.parent = new_arm
        modifier = obj.modifiers.new("Darktide fit", "ARMATURE")
        modifier.object = new_arm
        obj.modifiers.move(len(obj.modifiers) - 1, 0)
    collection.objects.unlink(arm)
    arm.hide_set(True)
    arm.hide_viewport = True
    identity.only_weighted_bones = True

    mapped_count = len(mapping)
    report.append("Mapped %d bones -> %d Darktide bones; scale %.4g" % (mapped_count, len(set(mapping.values())), scale0))
    unmapped = [b.name for b in bones if b.name not in mapping]
    report.append("Unmapped/grafted as extras: %d (%s%s)" % (len(unmapped), ", ".join(unmapped[:6]),
                                                            "..." if len(unmapped) > 6 else ""))
    report.append("Meshes fitted: %d (%d rigid parts baked into the skin)" % (len(plan), rigid_count))
    if unweighted_total:
        warnings.append("%d vertices carry no usable weights and were only similarity-fitted" % unweighted_total)
    if skipped:
        warnings.append("Meshes not tied to the source armature were left alone: " + ", ".join(skipped[:4]))
    if arm.animation_data and arm.animation_data.action:
        warnings.append("The source rig's animation stays on it; Carry Over Animation bakes it onto this one")
    report.extend("WARNING: " + w for w in warnings)
    return new_arm, report


# --------------------------------------------------------------------------------------
# Operators
# --------------------------------------------------------------------------------------

class DARKTIDE_OT_auto_map_bones(bpy.types.Operator):
    bl_idname = "darktide.auto_map_bones"
    bl_label = "Auto-map Bones"
    bl_description = "Guess source-bone to Darktide-bone rows from names and hierarchy; edit any row afterwards"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection first")
            return {"CANCELLED"}
        identity = collection.dt_asset_identity
        try:
            arm = find_source_armature(context, collection)
            nodes = refresh_reference_names(identity)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        ordered, listing = _armature_bone_list(arm)
        heads = [tuple(bone.head_local) for bone in ordered]
        assigned = auto_map_bones(listing, [n["name"] for n in nodes], heads)
        identity.bone_map.clear()
        for i, bone in enumerate(ordered):
            row = identity.bone_map.add()
            row.source_bone = bone.name
            row.target_bone = assigned.get(i, "")
        missing = missing_required(assigned.values())
        if missing:
            self.report({"WARNING"}, "Mapped %d bones; still unmapped: %s" % (len(assigned), ", ".join(missing)))
        else:
            self.report({"INFO"}, "Mapped %d of %d bones" % (len(assigned), len(ordered)))
        return {"FINISHED"}


class DARKTIDE_OT_fit_to_skeleton(bpy.types.Operator):
    bl_idname = "darktide.fit_to_skeleton"
    bl_label = "Fit to Darktide Skeleton"
    bl_description = "Warp the source rig's meshes onto the reference skeleton and rebuild the armature with native bones"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection first")
            return {"CANCELLED"}
        identity = collection.dt_asset_identity
        try:
            arm = find_source_armature(context, collection)
            rows = {r.source_bone: r.target_bone for r in identity.bone_map if r.target_bone}
            new_arm, lines = fit_to_skeleton(context, collection, arm, rows)
        except (ValueError, RuntimeError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        identity.fit_report = "\n".join(lines)
        for line in lines:
            self.report({"WARNING"} if line.startswith("WARNING") else {"INFO"}, line[:200])
        return {"FINISHED"}


class DARKTIDE_UL_bone_map(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        split = layout.split(factor=0.5)
        split.label(text=item.source_bone, icon="BONE_DATA")
        split.prop_search(item, "target_bone", data, "reference_bone_names", text="")

    def filter_items(self, context, data, propname):
        rows = getattr(data, propname)
        pattern = self.filter_name.lower()
        flags = [self.bitflag_filter_item if not pattern or pattern in (r.source_bone + " " + r.target_bone).lower()
                 else 0 for r in rows]
        return flags, []


def draw_fit_panel(layout, context, collection):
    identity = collection.dt_asset_identity
    box = layout.box()
    box.label(text="Fit to Darktide Skeleton")
    box.label(text="1. Pick a preset above  2. Auto-map  3. Fit  4. Compile")
    box.operator(DARKTIDE_OT_auto_map_bones.bl_idname, icon="BONE_DATA")
    if identity.bone_map:
        box.template_list("DARKTIDE_UL_bone_map", "", identity, "bone_map", identity, "bone_map_index", rows=6)
        mapped = [r.target_bone for r in identity.bone_map if r.target_bone]
        for name in missing_required(mapped):
            box.label(text="Unmapped required bone: " + name, icon="ERROR")
        box.label(text="%d of %d bones mapped; others are grafted as extras" % (len(mapped), len(identity.bone_map)))
        row = box.row()
        row.prop(identity, "fit_keep_torso")
        row.prop(identity, "fit_keep_proportions")
        box.operator(DARKTIDE_OT_fit_to_skeleton.bl_idname, icon="ARMATURE_DATA")
    for line in identity.fit_report.splitlines()[:8]:
        box.label(text=line[:90], icon="ERROR" if line.startswith("WARNING") else "INFO")
    starter = layout.box()
    starter.label(text="Starter Template")
    starter.operator(DARKTIDE_OT_new_starter_asset.bl_idname, icon="MESH_CAPSULE")


# --------------------------------------------------------------------------------------
# Starter template
# --------------------------------------------------------------------------------------

def _tube(bm, rings, segments=8):
    """rings: [(center, radius, {bone: weight})]; returns [(vertex, weights)]."""
    ring_verts = []
    for i, (center, radius, weights) in enumerate(rings):
        direction = (rings[i + 1][0] - center) if i + 1 < len(rings) else (center - rings[i - 1][0])
        direction = _unit(direction)
        u = direction.orthogonal().normalized()
        v = direction.cross(u)
        ring = [bm.verts.new(center + (u * math.cos(2 * math.pi * k / segments)
                                       + v * math.sin(2 * math.pi * k / segments)) * radius)
                for k in range(segments)]
        ring_verts.append((ring, weights))
    for (ring_a, _wa), (ring_b, _wb) in zip(ring_verts, ring_verts[1:]):
        for k in range(segments):
            n = (k + 1) % segments
            bm.faces.new((ring_a[k], ring_a[n], ring_b[n], ring_b[k]))
    bm.faces.new(ring_verts[0][0][::-1])
    bm.faces.new(ring_verts[-1][0])
    return [(vertex, weights) for ring, weights in ring_verts for vertex in ring]


def _chain_rings(pos, names, radii, extend, before=None, tip_direction=None):
    rings = []
    if before:
        rings.append((before[0], radii[0], before[1]))
    rings.extend((pos[n], r, {n: 1.0}) for n, r in zip(names, radii))
    if extend:
        direction = tip_direction or _unit(pos[names[-1]] - pos[names[-2]])
        rings.append((pos[names[-1]] + direction * extend, radii[len(names) - 1] * 0.8, {names[-1]: 1.0}))
    return rings


def _starter_parts(region, pos):
    """[(vertex, {bone: weight})] built in a bmesh, plus the bmesh."""
    bm = bmesh.new()
    weighted = []
    have = lambda names: [n for n in names if n in pos]
    if region == "hands":
        for side in ("left", "right"):
            names = have(["j_%shand" % side] + ["j_%shandmiddle%d" % (side, k) for k in (1, 2, 3)])
            if not names:
                continue
            radii = (0.048, 0.04, 0.034, 0.03)[:len(names)]
            forearm = "j_%sforearm" % side
            before = None
            if forearm in pos:
                back = _unit(pos[names[0]] - pos[forearm])
                before = (pos[names[0]] - back * 0.05, {forearm: 0.5, names[0]: 0.5})
            tip_direction = None
            if len(names) == 1:
                tip_direction = _unit(pos[names[0]] - pos[forearm]) if forearm in pos else Vector((0.0, 0.0, -1.0))
            rings = _chain_rings(pos, names, radii, 0.1 if len(names) == 1 else 0.05, before, tip_direction)
            weighted.extend(_tube(bm, rings))
    elif region == "head":
        head = pos["j_head"]
        center = head + Vector((0.0, 0.0, 0.09))
        geom = bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=1.0)
        for vertex in geom["verts"]:
            vertex.co = Vector((center.x + vertex.co.x * 0.10, center.y + vertex.co.y * 0.115,
                                center.z + vertex.co.z * 0.13))
            k = max(0.0, min(1.0, (head.z - vertex.co.z) / 0.07)) if "j_neck" in pos else 0.0
            weighted.append((vertex, {"j_head": 1.0 - k, "j_neck": k} if k > 0 else {"j_head": 1.0}))
    else:
        for side in ("left", "right"):
            legs = have(["j_%s%s" % (side, n) for n in ("upleg", "leg", "foot", "toebase")])
            arms = have(["j_%s%s" % (side, n) for n in ("arm", "forearm", "hand")])
            if len(legs) > 1:
                weighted.extend(_tube(bm, _chain_rings(pos, legs, (0.075, 0.055, 0.045, 0.03), 0.04)))
            if len(arms) > 1:
                weighted.extend(_tube(bm, _chain_rings(pos, arms, (0.05, 0.04, 0.035), 0.06)))
        torso = have(["j_hips", "j_spine", "j_spine1", "j_spine2", "j_neck", "j_head"])
        radius = {"j_hips": 0.14, "j_spine": 0.13, "j_spine1": 0.13, "j_spine2": 0.12, "j_neck": 0.05, "j_head": 0.09}
        weighted.extend(_tube(bm, _chain_rings(pos, torso, [radius[n] for n in torso], 0.12)))
    return bm, weighted


class DARKTIDE_OT_new_starter_asset(bpy.types.Operator):
    bl_idname = "darktide.new_starter_asset"
    bl_label = "New Starter Asset from Skeleton"
    bl_description = "Create an asset collection with the chosen Darktide skeleton and an example mesh weighted to it"
    bl_options = {"REGISTER", "UNDO"}

    preset: EnumProperty(name="Skeleton", default="human", items=STARTER_PRESETS)
    region: EnumProperty(name="Region", default="hands", items=[
        ("hands", "Hands (glove-like)", "Tube gloves weighted to the hand and finger bones"),
        ("head", "Head (helmet-like)", "Ellipsoid helmet weighted to the head and neck"),
        ("body", "Full body proxy", "One capsule per limb plus torso, weighted to the matching bones")])

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        from . import _new_asset_collection, _reference_armature
        scene = context.scene
        try:
            collection = _new_asset_collection(context, set())
            collection.name = "Starter " + self.region + " Asset"
            identity = collection.dt_asset_identity
            identity.skeleton_preset = self.preset
            if not identity.reference_unit:
                raise ValueError(identity.skeleton_preset_status or "Set the Game extract folder first")
            nodes = refresh_reference_names(identity)
            rig = _reference_armature(context, collection, bpy.path.abspath(identity.reference_unit),
                                      bpy.path.abspath(identity.reference_bones), identity.skeleton_resource)
        except (ValueError, RuntimeError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        pos = {n["name"]: Vector((n["world"][12], n["world"][13], n["world"][14])) for n in nodes}
        if self.region == "head" and "j_head" not in pos:
            self.report({"ERROR"}, "The skeleton has no j_head")
            return {"CANCELLED"}
        bm, weighted = _starter_parts(self.region, pos)
        bm.verts.index_update()
        name = "starter_" + self.region
        mesh = bpy.data.meshes.new(name)
        indexed = [(v.index, w) for v, w in weighted]
        bm.to_mesh(mesh)
        bm.free()
        mesh.polygons.foreach_set("use_smooth", [True] * len(mesh.polygons))
        obj = bpy.data.objects.new(name, mesh)
        collection.objects.link(obj)
        groups = {}
        for index, by_bone in indexed:
            for bone, weight in by_bone.items():
                if weight > 0:
                    if bone not in groups:
                        groups[bone] = obj.vertex_groups.new(name=bone)
                    groups[bone].add([index], weight, "REPLACE")
        material = bpy.data.materials.new(name + "_material")
        material.diffuse_color = (0.05, 0.05, 0.06, 1.0)
        mesh.materials.append(material)
        modifier = obj.modifiers.new("Armature", "ARMATURE")
        modifier.object = rig
        obj.parent = rig
        identity.asset_path = "content/mods/my_mod/" + name
        identity.asset_filename = name
        identity.only_weighted_bones = True
        scene.dt_asset.asset_collection = collection
        self.report({"INFO"}, "Created %s with %d weighted vertices on %s" % (collection.name, len(mesh.vertices), self.preset))
        return {"FINISHED"}


CLASSES = (DarktideBoneMapRow, DarktideReferenceBoneName)
UI_CLASSES = (DARKTIDE_OT_auto_map_bones, DARKTIDE_OT_fit_to_skeleton, DARKTIDE_UL_bone_map,
              DARKTIDE_OT_new_starter_asset)
