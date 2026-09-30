"""Small reader for native Darktide UNIT scene graphs and BONES tables."""

import math
import struct


class ReferenceSkeletonError(ValueError):
    pass


class _Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, size):
        if size < 0 or self.offset + size > len(self.data):
            raise ReferenceSkeletonError("UNIT scene graph is truncated")
        value = self.data[self.offset:self.offset + size]
        self.offset += size
        return value

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def floats(self, count):
        values = struct.unpack("<" + str(count) + "f", self.take(4 * count))
        if not all(math.isfinite(value) for value in values):
            raise ReferenceSkeletonError("UNIT contains non-finite transforms")
        return values

    def array(self):
        size = self.u32()
        self.take(size)

    def u32_array(self):
        count = self.u32()
        if count > 1_000_000:
            raise ReferenceSkeletonError("UNIT contains an implausible array")
        self.take(4 * count)


def _resource_hash(name):
    if (len(name) == 21 and name.startswith("#ID[") and name.endswith("]")
            and all(char in "0123456789abcdef" for char in name[4:20])):
        return int(name[4:20], 16)
    return _murmur64(name)


def _body(blob, label, resource_name=None):
    if len(blob) < 38:
        raise ReferenceSkeletonError(label + " is shorter than its cooked header")
    if struct.unpack_from("<Q", blob)[0] != _murmur64(label.lower()):
        raise ReferenceSkeletonError(label + " cooked type does not match")
    if resource_name is not None:
        found = struct.unpack_from("<Q", blob, 8)[0]
        expected = _resource_hash(resource_name)
        if found != expected:
            raise ReferenceSkeletonError(
                label + " cooked identity does not match the selected resource: expected %016x (%s), file is %016x"
                % (expected, resource_name, found))
    if blob[16] != 1 or blob[33] != 1 or any(blob[17:29]):
        raise ReferenceSkeletonError(label + " cooked header has unsupported fields")
    data_length = struct.unpack_from("<I", blob, 29)[0]
    stream_length = struct.unpack_from("<I", blob, 34)[0]
    expected = 38 + data_length + stream_length
    if len(blob) != expected:
        raise ReferenceSkeletonError(label + " cooked length does not match its header")
    stream = blob[38 + data_length:]
    try:
        stream_name = stream.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ReferenceSkeletonError(label + " stream name is not UTF-8") from exc
    return blob[38:38 + data_length], stream_name


def _skip_geometry(reader):
    if reader.u32() != 1:
        raise ReferenceSkeletonError("Unsupported UNIT mesh geometry version")
    count = reader.u32()
    if count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible vertex stream count")
    for _ in range(count):
        reader.array()
        reader.take(16)
    count = reader.u32()
    if count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible vertex channel count")
    reader.take(count * 17)
    reader.take(16)
    reader.array()
    count = reader.u32()
    if count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible mesh batch count")
    reader.take(count * 16)
    reader.take(40)
    reader.u32_array()
    reader.take(4)


def _parse_unit(blob):
    body, _stream_name = _body(blob, "UNIT")
    reader = _Reader(body)
    if reader.u32() != 0x73:
        raise ReferenceSkeletonError("Unsupported UNIT version; expected 0x73")
    count = reader.u32()
    if count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible mesh count")
    for _ in range(count):
        _skip_geometry(reader)
    count = reader.u32()
    if count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible skin count")
    for _ in range(count):
        matrix_count = reader.u32()
        if matrix_count > 1_000_000:
            raise ReferenceSkeletonError("UNIT contains an implausible skin matrix count")
        reader.take(matrix_count * 64)
        reader.u32_array()
        set_count = reader.u32()
        if set_count > 1_000_000:
            raise ReferenceSkeletonError("UNIT contains an implausible skin matrix set count")
        for _ in range(set_count):
            reader.u32_array()
    reader.array()
    groups = reader.u32()
    if groups > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible animation group count")
    for _ in range(groups):
        reader.take(4)
        reader.u32_array()
    count = reader.u32()
    if not 1 <= count <= 65535:
        raise ReferenceSkeletonError("UNIT scene node count is invalid")
    local_transforms = []
    for _ in range(count):
        rotation = reader.floats(9)
        position = reader.floats(3)
        scale = reader.floats(3)
        local_transforms.append((rotation, position, scale))
    worlds = [reader.floats(16) for _ in range(count)]
    parents = [struct.unpack("<HH", reader.take(4)) for _ in range(count)]
    hashes = [reader.u32() for _ in range(count)]
    unknown_count = reader.u32()
    if unknown_count > 1_000_000:
        raise ReferenceSkeletonError("UNIT contains an implausible trailing pair count")
    reader.take(unknown_count * 8)
    nodes = []
    for index, ((rotation, position, scale), world, (parent_type, parent_index), name_hash) in enumerate(
            zip(local_transforms, worlds, parents, hashes)):
        if parent_type not in {0, 1, 2, 3}:
            raise ReferenceSkeletonError("UNIT scene graph has an unknown parent type")
        if parent_type == 1 and parent_index >= count:
            raise ReferenceSkeletonError("UNIT scene graph has an invalid parent index")
        nodes.append({"index": index, "hash": name_hash, "parent_type": parent_type,
                      "parent_index": parent_index, "world": world})
    return nodes


def _murmur64(text):
    data = text.encode("utf-8")
    multiplier = 0xC6A4A7935BD1E995
    mask = 0xFFFFFFFFFFFFFFFF
    value = (len(data) * multiplier) & mask
    offset = 0
    while offset + 8 <= len(data):
        word = int.from_bytes(data[offset:offset + 8], "little")
        word = (word * multiplier) & mask
        word ^= word >> 47
        word = (word * multiplier) & mask
        value ^= word
        value = (value * multiplier) & mask
        offset += 8
    tail = data[offset:]
    if tail:
        value ^= int.from_bytes(tail, "little")
        value = (value * multiplier) & mask
    value ^= value >> 47
    value = (value * multiplier) & mask
    value ^= value >> 47
    return value


def _parse_bones(blob, resource_name=None):
    body, stream_name = _body(blob, "BONES", resource_name)
    if stream_name:
        raise ReferenceSkeletonError("BONES resource must be inline")
    if len(body) < 8:
        raise ReferenceSkeletonError("BONES body is truncated")
    bone_count, lod_count = struct.unpack_from("<II", body)
    if not 1 <= bone_count <= 4096 or not 1 <= lod_count <= 32:
        raise ReferenceSkeletonError("BONES header is invalid")
    cursor = 8
    fixed = cursor + 4 * (bone_count + lod_count)
    if fixed > len(body):
        raise ReferenceSkeletonError("BONES fixed tables are truncated")
    hashes = list(struct.unpack_from("<" + str(bone_count) + "I", body, cursor))
    cursor += 4 * bone_count
    lods = struct.unpack_from("<" + str(lod_count) + "I", body, cursor)
    if lods[0] != bone_count or any(lod < 1 or lod > bone_count or (index and lod > lods[index - 1])
                                       for index, lod in enumerate(lods)):
        raise ReferenceSkeletonError("BONES LOD table is invalid")
    cursor += 4 * lod_count
    names = []
    for _ in range(bone_count):
        try:
            end = body.index(b"\0", cursor)
            name = body[cursor:end].decode("utf-8")
        except (ValueError, UnicodeDecodeError) as exc:
            raise ReferenceSkeletonError("BONES name table is malformed") from exc
        names.append(name)
        cursor = end + 1
    if cursor != len(body):
        raise ReferenceSkeletonError("BONES parser did not consume the resource")
    if len(set(names)) != len(names) or len(set(hashes)) != len(hashes):
        raise ReferenceSkeletonError("BONES contains duplicate names or hashes")
    for name, name_hash in zip(names, hashes):
        if (_murmur64(name) >> 32) != name_hash:
            raise ReferenceSkeletonError("BONES name/hash mismatch for " + name)
    return names, hashes


def load_reference(unit_path, bones_path, resource_name=None):
    """Return BONES-ordered nodes with nearest named parent and native world matrix."""
    with open(unit_path, "rb") as source:
        unit_nodes = _parse_unit(source.read())
    with open(bones_path, "rb") as source:
        names, hashes = _parse_bones(source.read(), resource_name)
    by_hash = {}
    for node in unit_nodes:
        by_hash.setdefault(node["hash"], []).append(node)
    indices = {}
    for name, name_hash in zip(names, hashes):
        matches = by_hash.get(name_hash, ())
        if len(matches) != 1:
            reason = "missing" if not matches else "ambiguous"
            raise ReferenceSkeletonError("UNIT has " + reason + " BONES node " + name)
        indices[name] = matches[0]["index"]
    name_by_index = {index: name for name, index in indices.items()}
    result = []
    for name in names:
        node = unit_nodes[indices[name]]
        parent = None
        parent_type, parent_index = node["parent_type"], node["parent_index"]
        seen = {node["index"]}
        while parent_type == 1:
            if parent_index in seen:
                raise ReferenceSkeletonError("UNIT scene graph contains a parent cycle")
            seen.add(parent_index)
            if parent_index in name_by_index:
                parent = name_by_index[parent_index]
                break
            ancestor = unit_nodes[parent_index]
            parent_type, parent_index = ancestor["parent_type"], ancestor["parent_index"]
        result.append({"name": name, "parent": parent, "world": node["world"]})
    return result, len(unit_nodes)
