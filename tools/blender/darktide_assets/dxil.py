"""Reader for the game's compiled shaders: DXBC containers holding DXIL (LLVM 3.7 bitcode, shader model 6.0).

parse_container() splits a container into its parts; Module() reads a DXIL program (the DXIL part for the code,
the STAT part for the names the shader was compiled with) into types, constants, globals, metadata and the
instructions of each function, numbered the way LLVM numbers values.
"""
import struct

# ---------------------------------------------------------------------------------------------------------
# Container


def parse_container(data):
    """{fourcc: bytes} of a DXBC container."""
    if data[:4] != b"DXBC":
        raise ValueError("not a DXBC container")
    count, = struct.unpack_from("<I", data, 28)
    parts = {}
    for offset in struct.unpack_from("<%dI" % count, data, 32):
        size, = struct.unpack_from("<I", data, offset + 4)
        parts[data[offset:offset + 4].decode("latin-1")] = data[offset + 8:offset + 8 + size]
    return parts


def program_bitcode(part):
    """The bitcode of a DXIL / STAT part: program header (version, size), then the DXIL header (magic, version,
    offset, size)."""
    version, = struct.unpack_from("<I", part, 0)
    magic, _, offset, size = struct.unpack_from("<4sIII", part, 8)
    if magic != b"DXIL":
        raise ValueError("not a DXIL program")
    return version >> 16, part[8 + offset:8 + offset + size]


def parse_psv(part):
    """Pipeline state validation part: resource bindings and the signature elements in DXIL order."""
    p = 0
    info_size, = struct.unpack_from("<I", part, p); p += 4
    info = part[p:p + info_size]; p += info_size
    count, = struct.unpack_from("<I", part, p); p += 4
    resources = []
    if count:
        stride, = struct.unpack_from("<I", part, p); p += 4
        for i in range(count):
            kind, space, lower, upper = struct.unpack_from("<4I", part, p + i * stride)
            resources.append({"type": kind, "space": space, "lower": lower, "upper": upper})
        p += count * stride
    out = {"resources": resources, "inputs": [], "outputs": []}
    if info_size < 36:
        return out
    inputs, outputs = info[28], info[29]
    strings_size, = struct.unpack_from("<I", part, p); p += 4
    strings = part[p:p + strings_size]; p += strings_size
    index_count, = struct.unpack_from("<I", part, p); p += 4
    indexes = struct.unpack_from("<%dI" % index_count, part, p); p += 4 * index_count
    if inputs + outputs:
        stride, = struct.unpack_from("<I", part, p); p += 4

        def element(q):
            name, index_offset, rows, start_row, cols, kind, ctype, interp, mask, _ = struct.unpack_from("<IIBBBBBBBB", part, q)
            text = strings[name:strings.index(b"\0", name)].decode("latin-1")
            return {"name": text, "indexes": list(indexes[index_offset:index_offset + rows]), "rows": rows,
                    "start_row": start_row, "cols": cols & 0xf, "start_col": (cols >> 4) & 3, "kind": kind,
                    "type": ctype, "interpolation": interp}
        out["inputs"] = [element(p + i * stride) for i in range(inputs)]
        p += inputs * stride
        out["outputs"] = [element(p + i * stride) for i in range(outputs)]
    return out


# ---------------------------------------------------------------------------------------------------------
# Bitstream


class _Bits:
    def __init__(self, data):
        self.data = data + b"\0" * 8
        self.pos = 0
        self.end = len(data) * 8

    def read(self, width):
        if width == 0:
            return 0
        byte, shift = self.pos >> 3, self.pos & 7
        value = (int.from_bytes(self.data[byte:byte + 8], "little") >> shift) & ((1 << width) - 1)
        if width + shift > 64:
            value |= (self.data[byte + 8] << (64 - shift)) & ((1 << width) - 1)
        self.pos += width
        return value

    def vbr(self, width):
        value, shift, top = 0, 0, 1 << (width - 1)
        while True:
            piece = self.read(width)
            value |= (piece & (top - 1)) << shift
            if not piece & top:
                return value
            shift += width - 1

    def align32(self):
        self.pos = (self.pos + 31) & ~31


_CHAR6 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"


class _Block:
    def __init__(self, block_id, width, abbrevs):
        self.id = block_id
        self.width = width
        self.abbrevs = list(abbrevs)


def _read_abbrev(bits):
    ops = []
    for _ in range(bits.vbr(5)):
        if bits.read(1):
            ops.append(("literal", bits.vbr(8)))
        else:
            encoding = bits.read(3)
            if encoding in (1, 2):
                ops.append(("fixed" if encoding == 1 else "vbr", bits.vbr(5)))
            else:
                ops.append({3: ("array", 0), 4: ("char6", 0), 5: ("blob", 0)}[encoding])
    return ops


def _scalar(bits, op):
    kind, value = op
    if kind == "literal":
        return value
    if kind == "fixed":
        return bits.read(value)
    if kind == "vbr":
        return bits.vbr(value)
    if kind == "char6":
        return ord(_CHAR6[bits.read(6)])
    raise ValueError("bad abbreviation")


def _read_abbreviated(bits, ops):
    values = []
    i = 0
    while i < len(ops):
        kind = ops[i][0]
        if kind == "array":
            element = ops[i + 1]
            values.extend(_scalar(bits, element) for _ in range(bits.vbr(6)))
            i += 2
            continue
        if kind == "blob":
            size = bits.vbr(6)
            bits.align32()
            values.extend(bits.data[(bits.pos >> 3):(bits.pos >> 3) + size])
            bits.pos += size * 8
            bits.align32()
        else:
            values.append(_scalar(bits, ops[i]))
        i += 1
    return values[0], values[1:]


def _walk(bits, blockinfo, block):
    """Yield ('record', code, ops) / ('enter', id) / ('end',) items of one block, recursing into sub-blocks."""
    while True:
        if bits.pos >= bits.end:
            return
        abbrev = bits.read(block.width)
        if abbrev == 0:  # END_BLOCK
            bits.align32()
            yield ("end",)
            return
        if abbrev == 1:  # ENTER_SUBBLOCK
            sub_id = bits.vbr(8)
            width = bits.vbr(4)
            bits.align32()
            length = bits.read(32)
            yield ("enter", sub_id, length, _Block(sub_id, width, blockinfo.get(sub_id, [])))
            continue
        if abbrev == 2:  # DEFINE_ABBREV
            block.abbrevs.append(_read_abbrev(bits))
            continue
        if abbrev == 3:  # UNABBREV_RECORD
            code = bits.vbr(6)
            ops = [bits.vbr(6) for _ in range(bits.vbr(6))]
            yield ("record", code, ops)
            continue
        code, ops = _read_abbreviated(bits, block.abbrevs[abbrev - 4])
        yield ("record", code, ops)


def _sign_rotated(value):
    if not value & 1:
        return value >> 1
    if value != 1:
        return -(value >> 1)
    return -(1 << 63)


# ---------------------------------------------------------------------------------------------------------
# Module


class Type:
    def __init__(self, kind, **fields):
        self.kind = kind  # void float half double label metadata int pointer array vector struct function
        self.__dict__.update(fields)

    def __repr__(self):
        if self.kind == "int":
            return "i%d" % self.width
        if self.kind in ("array", "vector"):
            return "[%d x %r]" % (self.count, self.element)
        if self.kind == "pointer":
            return "%r*" % (self.pointee,)
        if self.kind == "struct":
            return "%" + (self.name or "anon")
        return self.kind


class Value:
    """A numbered value: a constant (kind 'const', 'undef', 'null', 'aggregate', 'data', 'expr'), a global,
    a function, an argument or an instruction result."""
    def __init__(self, kind, type, **fields):
        self.kind = kind
        self.type = type
        self.__dict__.update(fields)

    def __repr__(self):
        if self.kind == "const":
            return repr(self.value)
        return "<%s %s>" % (self.kind, getattr(self, "name", getattr(self, "index", "")))


class Instruction:
    def __init__(self, op, type, operands, **fields):
        self.op = op          # e.g. 'binop', 'cast', 'call', 'phi', 'br', ...
        self.type = type      # result type (None when it produces no value)
        self.operands = operands
        self.value = None     # the Value it defines
        self.__dict__.update(fields)


class Function:
    def __init__(self, name, type, prototype):
        self.name = name
        self.type = type
        self.prototype = prototype
        self.blocks = []  # lists of Instruction


_BINOPS = ["add", "sub", "mul", "udiv", "sdiv", "urem", "srem", "shl", "lshr", "ashr", "and", "or", "xor"]
_CASTS = ["trunc", "zext", "sext", "fptoui", "fptosi", "uitofp", "sitofp", "fptrunc", "fpext", "ptrtoint",
          "inttoptr", "bitcast", "addrspacecast"]


class Module:
    def __init__(self, bitcode):
        if bitcode[:4] != b"BC\xc0\xde":
            raise ValueError("not LLVM bitcode")
        self.types = []
        self.values = []        # module-level values: globals, functions, constants
        self.globals = []
        self.functions = []
        self.metadata = {}      # index -> node (list), string or Value
        self.named_metadata = {}
        self.struct_names = []
        self.version = 0
        self._bodies = []       # functions with bodies, in order
        self._metadata_count = 0
        bits = _Bits(bitcode[4:])
        blockinfo = {}
        root = _Block(None, 2, [])
        for item in _walk(bits, blockinfo, root):
            if item[0] == "enter":
                self._block(bits, blockinfo, item)

    # -- block dispatch
    def _block(self, bits, blockinfo, item):
        _, block_id, length, block = item
        if block_id == 0:
            self._blockinfo(bits, blockinfo, block)
        elif block_id == 8:
            self._module(bits, blockinfo, block)
        else:
            bits.pos += length * 32

    def _blockinfo(self, bits, blockinfo, block):
        current = None
        while True:
            abbrev = bits.read(block.width)
            if abbrev == 0:
                bits.align32()
                return
            if abbrev == 2:
                blockinfo.setdefault(current, []).append(_read_abbrev(bits))
            elif abbrev == 3:
                code = bits.vbr(6)
                ops = [bits.vbr(6) for _ in range(bits.vbr(6))]
                if code == 1:
                    current = ops[0]
            else:
                raise ValueError("unexpected item in BLOCKINFO")

    def _skip(self, bits, length):
        bits.pos += length * 32

    def _module(self, bits, blockinfo, block):
        for item in _walk(bits, blockinfo, block):
            if item[0] == "enter":
                _, sub_id, length, sub = item
                if sub_id == 0:
                    self._blockinfo(bits, blockinfo, sub)
                elif sub_id == 17:
                    self._types(bits, blockinfo, sub)
                elif sub_id == 11:
                    self._constants(bits, blockinfo, sub, self.values)
                elif sub_id == 15:
                    self._metadata(bits, blockinfo, sub, self.values)
                elif sub_id == 12:
                    self._function(bits, blockinfo, sub)
                else:
                    self._skip(bits, length)
            elif item[0] == "record":
                _, code, ops = item
                if code == 1:
                    self.version = ops[0]
                elif code == 7:
                    self._global(ops)
                elif code == 8:
                    self._function_decl(ops)
            else:
                break
        for value in self.values:
            if value.kind == "global" and value.init_id:
                value.init = self.values[value.init_id - 1]

    def _types(self, bits, blockinfo, block):
        name = None
        pending = []  # pointers to types defined later (forward references)
        for item in _walk(bits, blockinfo, block):
            if item[0] != "record":
                if item[0] == "enter":
                    self._skip(bits, item[2])
                    continue
                break
            _, code, ops = item
            t = None
            if code == 1:
                continue
            elif code == 2:
                t = Type("void")
            elif code == 3:
                t = Type("float")
            elif code == 4:
                t = Type("double")
            elif code == 10:
                t = Type("half")
            elif code == 5:
                t = Type("label")
            elif code == 16:
                t = Type("metadata")
            elif code == 7:
                t = Type("int", width=ops[0])
            elif code == 8:
                t = Type("pointer", pointee=ops[0], space=ops[1] if len(ops) > 1 else 0)
                pending.append(t)
            elif code in (11, 12):
                t = Type("array" if code == 11 else "vector", count=ops[0], element=ops[1])
                pending.append(t)
            elif code == 19:
                name = "".join(chr(c) for c in ops)
                continue
            elif code in (18, 20):
                t = Type("struct", name=name if code == 20 else None, packed=ops[0], members=ops[1:])
                pending.append(t)
                name = None
            elif code == 6:
                t = Type("struct", name=name, packed=0, members=[], opaque=True)
                name = None
            elif code == 21:
                t = Type("function", vararg=ops[0], ret=ops[1], params=ops[2:])
                pending.append(t)
            else:
                t = Type("unknown%d" % code)
            self.types.append(t)
        for t in pending:
            if t.kind == "pointer":
                t.pointee = self.types[t.pointee]
            elif t.kind in ("array", "vector"):
                t.element = self.types[t.element]
            elif t.kind == "struct":
                t.members = [self.types[i] for i in t.members]
            elif t.kind == "function":
                t.ret = self.types[t.ret]
                t.params = [self.types[i] for i in t.params]

    def _global(self, ops):
        explicit = ops[1] & 2
        value_type = self.types[ops[0]]
        if not explicit:
            value_type = value_type.pointee
        pointer = Type("pointer", pointee=value_type, space=ops[1] >> 2 if explicit else 0)
        value = Value("global", pointer, value_type=value_type, constant=bool(ops[1] & 1), init_id=ops[2],
                      init=None, index=len(self.values))
        self.globals.append(value)
        self.values.append(value)

    def _function_decl(self, ops):
        ftype = self.types[ops[0]]
        if ftype.kind == "pointer":
            ftype = ftype.pointee
        function = Function(None, ftype, prototype=bool(ops[2]))
        value = Value("function", Type("pointer", pointee=ftype, space=0), function=function, index=len(self.values))
        function.value = value
        self.functions.append(function)
        self.values.append(value)
        if not function.prototype:
            self._bodies.append(function)

    # -- constants
    def _constants(self, bits, blockinfo, block, table):
        current = None
        for item in _walk(bits, blockinfo, block):
            if item[0] != "record":
                if item[0] == "enter":
                    self._skip(bits, item[2])
                    continue
                break
            _, code, ops = item
            if code == 1:
                current = self.types[ops[0]]
                continue
            t = current
            if code == 2:
                v = Value("null", t, value=0)
            elif code == 3:
                v = Value("undef", t)
            elif code == 4:
                v = Value("const", t, value=_sign_rotated(ops[0]))
            elif code == 6:
                if t.kind == "float":
                    v = Value("const", t, value=struct.unpack("<f", struct.pack("<I", ops[0] & 0xffffffff))[0], bits=ops[0])
                elif t.kind == "double":
                    v = Value("const", t, value=struct.unpack("<d", struct.pack("<Q", ops[0]))[0], bits=ops[0])
                elif t.kind == "half":
                    v = Value("const", t, value=struct.unpack("<e", struct.pack("<H", ops[0]))[0], bits=ops[0])
                else:
                    raise ValueError("float constant of type " + t.kind)
            elif code == 7:
                v = Value("aggregate", t, elements=list(ops))
            elif code in (8, 9):
                v = Value("data", t, items=list(ops))
            elif code == 22:
                element = t.element
                if element.kind == "float":
                    items = [struct.unpack("<f", struct.pack("<I", x))[0] for x in ops]
                elif element.kind == "double":
                    items = [struct.unpack("<d", struct.pack("<Q", x))[0] for x in ops]
                else:
                    items = list(ops)
                v = Value("data", t, items=items)
            elif code in (12, 20):
                # constant getelementptr: [pointee type?, (type, value)...]
                v = Value("expr", t, op="gep", ops=list(ops))
            elif code == 11:
                v = Value("expr", t, op="cast", ops=list(ops))
            elif code == 10:
                v = Value("expr", t, op="binop", ops=list(ops))
            else:
                v = Value("expr", t, op="code%d" % code, ops=list(ops))
            v.index = len(table)
            table.append(v)
        for v in table:
            if v.kind == "aggregate" and v.elements and isinstance(v.elements[0], int):
                v.elements = [table[i] for i in v.elements]

    # -- metadata
    def _metadata(self, bits, blockinfo, block, table):
        name = None
        for item in _walk(bits, blockinfo, block):
            if item[0] != "record":
                if item[0] == "enter":
                    self._skip(bits, item[2])
                    continue
                break
            _, code, ops = item
            index = self._metadata_count
            if code == 1:  # STRING
                self.metadata[index] = bytes(ops).decode("latin-1")
                self._metadata_count += 1
            elif code == 2:  # VALUE [type, value]
                self.metadata[index] = ("value", ops[1])
                self._metadata_count += 1
            elif code in (3, 5):  # NODE / DISTINCT_NODE [n x md num + 1]
                self.metadata[index] = [x - 1 if x else None for x in ops]
                self._metadata_count += 1
            elif code == 4:  # NAME
                name = bytes(ops).decode("latin-1")
            elif code == 10:  # NAMED_NODE
                self.named_metadata[name] = list(ops)
            elif code == 6:  # KIND
                pass
            else:
                self._metadata_count += 1

    def md(self, index):
        """Resolve a metadata node to Python values: strings, Values (constants / globals) and lists."""
        if index is None:
            return None
        node = self.metadata.get(index)
        if isinstance(node, str):
            return node
        if isinstance(node, tuple):
            return self.values[node[1]] if node[1] < len(self.values) else None
        if isinstance(node, list):
            return [self.md(i) for i in node]
        return None

    # -- functions
    def _function(self, bits, blockinfo, block):
        function = self._bodies.pop(0)
        values = list(self.values)
        for i, ptype in enumerate(function.type.params):
            values.append(Value("argument", ptype, index=i))
        blocks = []
        current = []
        declared = 0

        def next_id():
            return len(values)

        def relative(op):
            return next_id() - op if self.version >= 1 else op

        def value_type_pair(ops, slot):
            vid = relative(ops[slot])
            if vid < next_id():
                return values[vid], slot + 1
            # forward reference: the type follows
            placeholder = Value("forward", self.types[ops[slot + 1]], id=vid)
            return placeholder, slot + 2

        def value(ops, slot, vtype):
            vid = relative(ops[slot])
            if vid < next_id():
                return values[vid]
            return Value("forward", vtype, id=vid)

        def signed_value(raw, vtype):
            vid = next_id() - _sign_rotated(raw) if self.version >= 1 else raw
            if vid < next_id():
                return values[vid]
            return Value("forward", vtype, id=vid)

        def define(instruction):
            if instruction.type is not None and instruction.type.kind != "void":
                v = Value("instruction", instruction.type, instruction=instruction, index=len(values))
                instruction.value = v
                values.append(v)
            current.append(instruction)

        def finish_block():
            nonlocal current
            blocks.append(current)
            current = []

        for item in _walk(bits, blockinfo, block):
            if item[0] == "enter":
                _, sub_id, length, sub = item
                if sub_id == 11:
                    self._constants(bits, blockinfo, sub, values)
                elif sub_id == 15:
                    self._metadata(bits, blockinfo, sub, values)
                else:
                    self._skip(bits, length)
                continue
            if item[0] == "end":
                break
            _, code, ops = item
            if code == 1:  # DECLAREBLOCKS
                declared = ops[0]
                continue
            if code in (33, 35):  # debug locations
                continue
            if code == 2:  # BINOP
                a, slot = value_type_pair(ops, 0)
                b = value(ops, slot, a.type)
                define(Instruction("binop", a.type, [a, b], opcode=_BINOPS[ops[slot + 1]]))
            elif code == 3:  # CAST
                a, slot = value_type_pair(ops, 0)
                define(Instruction("cast", self.types[ops[slot]], [a], opcode=_CASTS[ops[slot + 1]]))
            elif code == 29:  # VSELECT
                t, slot = value_type_pair(ops, 0)
                f = value(ops, slot, t.type)
                cond, _ = value_type_pair(ops, slot + 1)
                define(Instruction("select", t.type, [cond, t, f]))
            elif code == 5:  # SELECT (old)
                t, slot = value_type_pair(ops, 0)
                f = value(ops, slot, t.type)
                cond = value(ops, slot + 1, Type("int", width=1))
                define(Instruction("select", t.type, [cond, t, f]))
            elif code == 26:  # EXTRACTVAL
                agg, slot = value_type_pair(ops, 0)
                indices = ops[slot:]
                t = agg.type
                for i in indices:
                    t = t.members[i] if t.kind == "struct" else t.element
                define(Instruction("extractvalue", t, [agg], indices=indices))
            elif code == 27:  # INSERTVAL
                agg, slot = value_type_pair(ops, 0)
                val, slot = value_type_pair(ops, slot)
                define(Instruction("insertvalue", agg.type, [agg, val], indices=ops[slot:]))
            elif code == 6:  # EXTRACTELT
                vec, slot = value_type_pair(ops, 0)
                idx, slot = value_type_pair(ops, slot)
                define(Instruction("extractelement", vec.type.element, [vec, idx]))
            elif code == 7:  # INSERTELT
                vec, slot = value_type_pair(ops, 0)
                elt = value(ops, slot, vec.type.element)
                idx, _ = value_type_pair(ops, slot + 1)
                define(Instruction("insertelement", vec.type, [vec, elt, idx]))
            elif code in (9, 28):  # CMP / CMP2
                a, slot = value_type_pair(ops, 0)
                b = value(ops, slot, a.type)
                result = Type("int", width=1)
                if a.type.kind == "vector":
                    result = Type("vector", count=a.type.count, element=result)
                define(Instruction("cmp", result, [a, b], predicate=ops[slot + 1]))
            elif code == 10:  # RET
                operands = []
                if ops:
                    v, _ = value_type_pair(ops, 0)
                    operands = [v]
                define(Instruction("ret", None, operands))
                finish_block()
            elif code == 11:  # BR
                if len(ops) == 1:
                    define(Instruction("br", None, [], targets=[ops[0]]))
                else:
                    cond = value(ops, 2, Type("int", width=1))
                    define(Instruction("br", None, [cond], targets=[ops[0], ops[1]]))
                finish_block()
            elif code == 12:  # SWITCH [opty, cond, default, (value id, bb)...]
                cond = value(ops, 1, self.types[ops[0]])
                cases = [(values[ops[i]] if ops[i] < len(values) else None, ops[i + 1]) for i in range(3, len(ops), 2)]
                define(Instruction("switch", None, [cond], default=ops[2], cases=cases))
                finish_block()
            elif code == 15:  # UNREACHABLE
                define(Instruction("unreachable", None, []))
                finish_block()
            elif code == 16:  # PHI [ty, (val, bb)...]
                t = self.types[ops[0]]
                incoming = []
                for i in range(1, len(ops) - 1, 2):
                    incoming.append((signed_value(ops[i], t), ops[i + 1]))
                define(Instruction("phi", t, [v for v, _ in incoming], blocks=[b for _, b in incoming]))
            elif code == 19:  # ALLOCA [instty, opty, op, align]
                t = self.types[ops[0]]
                size = values[ops[2]]
                explicit = ops[3] & (1 << 6)
                pointer = Type("pointer", pointee=t, space=0) if explicit else t
                define(Instruction("alloca", pointer, [size], allocated=t if explicit else t.pointee))
            elif code == 20:  # LOAD [op, ty?, align, vol]
                ptr, slot = value_type_pair(ops, 0)
                if slot + 3 == len(ops):
                    t = self.types[ops[slot]]
                else:
                    t = ptr.type.pointee
                define(Instruction("load", t, [ptr]))
            elif code == 44:  # STORE [ptr, val, align, vol]
                ptr, slot = value_type_pair(ops, 0)
                val, slot = value_type_pair(ops, slot)
                define(Instruction("store", None, [ptr, val]))
            elif code == 43:  # GEP [inbounds, ty, (ptr, idx...) with types]
                source = self.types[ops[1]]
                slot = 2
                operands = []
                while slot < len(ops):
                    v, slot = value_type_pair(ops, slot)
                    operands.append(v)
                t = source
                for idx in operands[2:]:
                    if t.kind == "struct":
                        t = t.members[idx.value]
                    else:
                        t = t.element
                define(Instruction("gep", Type("pointer", pointee=t, space=0), operands, source=source))
            elif code == 34:  # CALL [attrs, cc, fnty?, callee, args...]
                cc = ops[1]
                slot = 2
                ftype = None
                if cc & (1 << 15):
                    ftype = self.types[ops[slot]]
                    slot += 1
                callee, slot = value_type_pair(ops, slot)
                if ftype is None:
                    ftype = callee.type.pointee
                args = []
                for ptype in ftype.params:
                    args.append(value(ops, slot, ptype))
                    slot += 1
                define(Instruction("call", ftype.ret, args, callee=callee))
            elif code == 36:  # FENCE
                define(Instruction("fence", None, []))
            elif code == 38:  # ATOMICRMW [ptr, val, op, vol, ordering, scope]
                ptr, slot = value_type_pair(ops, 0)
                val = value(ops, slot, ptr.type.pointee)
                define(Instruction("atomicrmw", val.type, [ptr, val], opcode=ops[slot + 1]))
            elif code == 46:  # CMPXCHG
                ptr, slot = value_type_pair(ops, 0)
                cmp, slot = value_type_pair(ops, slot)
                new = value(ops, slot, cmp.type)
                define(Instruction("cmpxchg", Type("struct", name=None, packed=0, members=[cmp.type, Type("int", width=1)]), [ptr, cmp, new]))
            else:
                raise ValueError("unsupported instruction record %d" % code)
        if current:
            blocks.append(current)
        # resolve forward references
        for block_instructions in blocks:
            for instruction in block_instructions:
                instruction.operands = [values[v.id] if isinstance(v, Value) and v.kind == "forward" else v
                                        for v in instruction.operands]
        if declared and len(blocks) != declared:
            raise ValueError("function has %d blocks, declared %d" % (len(blocks), declared))
        function.blocks = blocks
        function.values = values

    # -- names: the module-level value symbol table is skipped; DXIL calls are identified by their opcode
