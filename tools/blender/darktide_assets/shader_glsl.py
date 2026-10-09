"""Turns a pair of the game's compiled shader stages (vertex + pixel, DXIL) into GLSL for Blender's GPU module.

The translation follows the DXIL instruction by instruction. Control flow without loops runs every block in order
with a "reached" flag per block (phi nodes pick their value by the edge taken, side effects only happen in reached
blocks); loops are not supported. Resources keep their names: constant buffers become uniform blocks of vec4
registers (filled by name from the stage's type annotations), textures become samplers, and the bindless texture
slots of a material resolve to the material's texture names. Writes to engine buffers (texture streaming feedback)
are dropped.
"""
import math
import struct

from . import dxil

# DXIL signature system values
SV_ARBITRARY, SV_VERTEX_ID, SV_INSTANCE_ID, SV_POSITION, SV_IS_FRONT_FACE, SV_TARGET, SV_DEPTH = 0, 1, 2, 3, 13, 16, 17
# resource kinds
TEXTURE_DIMS = {1: "1D", 2: "2D", 3: "2D", 4: "3D", 5: "CUBE", 6: "1D_ARRAY", 7: "2D_ARRAY", 9: "CUBE_ARRAY"}
BUFFER_KINDS = {10, 11, 12}


class TranslationError(ValueError):
    pass


def _f(value):
    if math.isnan(value) or math.isinf(value):
        return "uintBitsToFloat(%du)" % struct.unpack("<I", struct.pack("<f", value))[0]
    text = repr(float(struct.unpack("<f", struct.pack("<f", value))[0]))
    return text if ("." in text or "e" in text) else text + ".0"


def _glsl_type(t):
    if t.kind in ("float", "half", "double"):
        return "float"
    if t.kind == "int":
        return "bool" if t.width == 1 else "uint"
    if t.kind == "struct":
        members = t.members
        if members and all(m.kind in ("float", "half", "double") for m in members[:4]):
            return "vec4"
        return "uvec4"
    raise TranslationError("no GLSL type for " + repr(t))


def _const(v):
    if v.kind in ("null", "undef"):
        t = v.type
        if t.kind in ("float", "half", "double"):
            return "0.0"
        if t.kind == "int":
            return "false" if t.width == 1 else "0u"
        return None
    if v.kind == "const":
        t = v.type
        if t.kind in ("float", "half", "double"):
            return _f(v.value)
        if t.width == 1:
            return "true" if v.value else "false"
        return "%du" % (v.value & 0xffffffff)
    return None


class _Annotations:
    """Constant buffer layouts by name: member -> (offset, kind, count, matrix) from dx.typeAnnotations."""
    def __init__(self, stat):
        self.structs = {}
        for index in stat.named_metadata.get("dx.typeAnnotations", []):
            node = stat.md(index)
            if not node or node[0] is not None and getattr(node[0], "value", 0) != 0:
                continue
            for i in range(1, len(node) - 1, 2):
                value, fields = node[i], node[i + 1]
                if value is None or not fields:
                    continue
                self.structs[id(value.type)] = (value.type, fields)

    def layout(self, struct_type, base=0, prefix=""):
        """[(name, offset, element type, count, matrix rows/cols or None)] flattened."""
        entry = self.structs.get(id(struct_type))
        if entry is None:
            return []
        t, fields = entry
        out = []
        for member, field in zip(t.members, fields[1:]):
            info = {}
            for k in range(0, len(field) - 1, 2):
                key = field[k].value if hasattr(field[k], "value") else (0 if field[k] is not None and field[k].kind == "null" else field[k])
                info[key] = field[k + 1]
            name = info.get(6)
            offset = info.get(3)
            offset = 0 if offset is None or not isinstance(offset, int) and offset.kind == "null" else (offset if isinstance(offset, int) else offset.value)
            matrix = info.get(2)
            if matrix is not None:
                matrix = [_num(x) for x in matrix]
            count = 1
            element = member
            while element.kind == "array":
                count *= element.count
                element = element.element
            if element.kind == "struct" and not (element.name or "").startswith("class.matrix") and matrix is None:
                inner = self.layout(element, base + offset, prefix + name + ".")
                if inner:
                    out.extend(inner)
                    continue
            out.append((prefix + name, base + offset, element, count, matrix))
        return out


class _Stage:
    def __init__(self, dxbc):
        parts = dxil.parse_container(dxbc)
        self.kind, bitcode = dxil.program_bitcode(parts["DXIL"])
        self.module = dxil.Module(bitcode)
        stat = dxil.Module(dxil.program_bitcode(parts["STAT"])[1]) if "STAT" in parts else self.module
        self.annotations = _Annotations(stat)
        self.stat = stat
        functions = [f for f in self.module.functions if f.blocks]
        if len(functions) != 1:
            raise TranslationError("expected one entry function")
        self.function = functions[0]
        entry = stat.md(stat.named_metadata["dx.entryPoints"][0])
        signatures = entry[2] or [None, None, None]
        self.inputs = [self._element(e) for e in (signatures[0] or [])]
        self.outputs = [self._element(e) for e in (signatures[1] or [])]
        resources = entry[3] or [None, None, None, None]
        self.resources = {}
        for klass, items in enumerate(resources):
            for item in items or []:
                rid = _num(item[0])
                record = {"class": klass, "id": rid, "name": item[2], "space": _num(item[3]),
                          "register": _num(item[4]), "count": _num(item[5]), "global": item[1]}
                if klass in (0, 1):
                    record["shape"] = _num(item[6])
                if klass == 2:
                    record["size"] = _num(item[6])
                self.resources[(klass, rid)] = record

    def _element(self, e):
        return {"id": _num(e[0]), "semantic": e[1], "type": _num(e[2]), "system": _num(e[3]),
                "indexes": [_num(x) for x in (e[4] or [])], "interpolation": _num(e[5]), "rows": _num(e[6]),
                "cols": _num(e[7])}


def _num(v):
    if v is None:
        return 0
    if isinstance(v, int):
        return v
    if v.kind == "const":
        return v.value
    return 0


# ---------------------------------------------------------------------------------------------------------


class _Emitter:
    def __init__(self, stage, program, prefix):
        self.stage = stage
        self.program = program
        self.prefix = prefix
        self.lines = []
        self.decls = []
        self.names = {}
        self.handles = {}
        self.pointers = {}

    def name(self, value):
        if value.kind == "instruction":
            return self.names[id(value)]
        text = _const(value)
        if text is None:
            raise TranslationError("unsupported operand " + repr(value))
        return text

    # -- resources
    def handle(self, value):
        if id(value) not in self.handles:
            raise TranslationError("resource handle not from createHandle")
        return self.handles[id(value)]

    def resolve_index(self, value):
        """A bindless index: (constant buffer name, byte offset) it was read from, plus a constant."""
        add = 0
        while value.kind == "instruction" and value.instruction.op == "binop" and value.instruction.opcode == "add":
            a, b = value.instruction.operands
            if b.kind in ("const", "null"):
                add += b.value
                value = a
            elif a.kind in ("const", "null"):
                add += a.value
                value = b
            else:
                break
        if value.kind in ("const", "null"):
            return None, value.value + add
        if value.kind == "instruction" and value.instruction.op == "extractvalue":
            load = value.instruction.operands[0]
            if load.kind == "instruction" and load.instruction.op == "call" and _opcode(load.instruction) == 59:
                buffer = self.cbuffer(self.handle(load.instruction.operands[1]))
                register = load.instruction.operands[2]
                if register.kind in ("const", "null"):
                    return (buffer, register.value * 16 + value.instruction.indices[0] * 4), add
        raise TranslationError("bindless index the preview can't follow")

    def cbuffer_member(self, buffer, offset):
        for name, at, element, count, matrix in buffer["layout"]:
            size = 16 * count if count > 1 or matrix else _size(element)
            if matrix:
                size = 16 * (matrix[0] if isinstance(matrix, list) else 4) * count
            if at <= offset < at + max(size, 4):
                return name, offset - at
        return None, 0

    def texture(self, handle, sampler_handle=None):
        resource = handle["resource"]
        if resource["count"] == -1 or resource["count"] == 0xffffffff or resource["count"] > 1 and handle["index"] is not None:
            buffer, add = handle["index"]
            if buffer is None:
                raise TranslationError("constant bindless index")
            member, _ = self.cbuffer_member(buffer[0], buffer[1])
            if member is None or "__BINDLESS_" not in member:
                raise TranslationError("bindless index from " + str(member))
            key = ("material", member.split("__BINDLESS_", 1)[1].split("_", 1)[1])
        else:
            name = resource["name"]
            key = ("engine", name[6:] if name.startswith("__tex_") else name)
        dim = TEXTURE_DIMS.get(resource.get("shape"), "2D")
        return self.program.texture(key, dim, self.sampler(sampler_handle) if sampler_handle else None)

    def sampler(self, handle):
        resource = handle["resource"]
        if resource["count"] in (-1, 0xffffffff):
            buffer, add = handle["index"]
            member, _ = self.cbuffer_member(buffer[0], buffer[1]) if buffer else (None, 0)
            if member and "__BINDLESS_SAMPLER_" in member:
                return ("material", member.split("__BINDLESS_SAMPLER_", 1)[1])
            return None
        name = resource["name"]
        return ("engine", name[7:] if name.startswith("__samp_") else name)

    def cbuffer(self, handle):
        resource = handle["resource"]
        return self.program.cbuffer(resource, self.stage)


def _size(t):
    if t.kind in ("float", "int"):
        return 4
    if t.kind == "vector":
        return 4 * t.count
    return 16


def _opcode(instruction):
    return instruction.operands[0].value


class Program:
    """A vertex + pixel shader pair as GLSL sources for gpu.types.GPUShaderCreateInfo."""
    def __init__(self, vertex_dxbc, pixel_dxbc):
        self.textures = {}     # key -> {"glsl", "dim", "sampler"}
        self.cbuffers = {}     # name -> {"glsl", "registers", "layout"}
        self.warnings = []
        self.vertex = _Stage(vertex_dxbc)
        self.pixel = _Stage(pixel_dxbc)
        if self.vertex.kind != 1 or self.pixel.kind != 0:
            raise TranslationError("expected a vertex and a pixel shader")
        self.attributes = []   # (glsl name, semantic, index)
        self.varyings = []     # (glsl name, semantic + index, interpolation, kind)
        self._varying_names = {}
        for element in self.vertex.outputs:
            if element["system"] == SV_POSITION:
                continue
            for row in range(element["rows"]):
                semantic = "%s%d" % (element["semantic"], element["indexes"][row])
                name = "dt_io_" + semantic.lower()
                self._varying_names[semantic] = name
                flat = element["type"] != 9 or element["interpolation"] == 1
                self.varyings.append((name, semantic, element["interpolation"], "flat" if flat else "smooth",
                                      "uvec4" if element["type"] != 9 else "vec4"))
        self.vertex_source = self._translate(self.vertex, "v")
        self.fragment_source = self._translate(self.pixel, "p")

    def texture(self, key, dim, sampler):
        if key not in self.textures:
            self.textures[key] = {"glsl": "dt_tex%d" % len(self.textures), "dim": dim, "sampler": sampler}
        elif sampler and not self.textures[key]["sampler"]:
            self.textures[key]["sampler"] = sampler
        return self.textures[key]

    def cbuffer(self, resource, stage):
        name = resource["name"]
        if name not in self.cbuffers:
            layout = stage.annotations.layout(resource["global"].type.pointee)
            self.cbuffers[name] = {"glsl": "dt_cb%d" % len(self.cbuffers), "registers": (resource["size"] + 15) // 16,
                                   "layout": layout}
        return self.cbuffers[name]

    # ---------------------------------------------------------------------------------------------------------
    def _translate(self, stage, prefix):
        e = _Emitter(stage, self, prefix)
        function = stage.function
        blocks = function.blocks
        n = len(blocks)
        successors = []
        for block in blocks:
            last = block[-1]
            if last.op == "br":
                successors.append(list(last.targets))
            elif last.op == "switch":
                successors.append([last.default] + [b for _, b in last.cases])
            else:
                successors.append([])
        order, state = [], [0] * n

        def visit(b):
            state[b] = 1
            for s in successors[b]:
                if state[s] == 1:
                    raise TranslationError("the shader has a loop")
                if state[s] == 0:
                    visit(s)
            state[b] = 2
            order.append(b)
        visit(0)
        order.reverse()
        predecessors = {b: [] for b in range(n)}
        for b in order:
            for s in successors[b]:
                if b not in predecessors[s]:
                    predecessors[s].append(b)

        # liveness: outputs, discards, stores and branch conditions are roots
        live = set()
        work = []

        def mark(v):
            if v.kind == "instruction" and id(v) not in live:
                live.add(id(v))
                work.append(v.instruction)
        for b in order:
            for ins in blocks[b]:
                if ins.op in ("br", "switch") and ins.operands:
                    mark(ins.operands[0])
                elif ins.op == "store":
                    for v in ins.operands:
                        mark(v)
                elif ins.op == "call" and _opcode(ins) in (5, 82):
                    for v in ins.operands:
                        mark(v)
        while work:
            ins = work.pop()
            for v in ins.operands:
                mark(v)
        self._live = live

        out = e.lines
        if prefix == "v":
            out.append("vec4 dt_position = vec4(0.0);")
            for name, _, _, _, kind in self.varyings:
                out.append("%s = %s(0);" % (name, kind))
        else:
            out.append("dt_frag = vec4(0.0);")
        reached = {}
        edge = {}
        for b in order:
            if b == 0:
                reached[b] = "true"
            else:
                terms = [edge[(p, b)] for p in predecessors[b] if (p, b) in edge]
                name = "dt_%sb%d" % (prefix, b)
                out.append("bool %s = %s;" % (name, " || ".join(terms) if terms else "false"))
                reached[b] = name
            for ins in blocks[b]:
                self._instruction(e, ins, b, reached, edge, predecessors)
            last = blocks[b][-1]
            if last.op == "br":
                if len(last.targets) == 1:
                    edge[(b, last.targets[0])] = reached[b]
                else:
                    cond = e.name(last.operands[0])
                    t, f = last.targets
                    if t == f:
                        edge[(b, t)] = reached[b]
                    else:
                        edge[(b, t)] = "(%s && %s)" % (reached[b], cond)
                        edge[(b, f)] = "(%s && !%s)" % (reached[b], cond)
            elif last.op == "switch":
                cond = e.name(last.operands[0])
                cases = []
                for value, target in last.cases:
                    test = "%s == %s" % (cond, _const(value))
                    edge[(b, target)] = "(%s && %s)" % (reached[b], test) if (b, target) not in edge else edge[(b, target)][:-1] + " || " + test + ")"
                    cases.append(test)
                default = "(%s && !(%s))" % (reached[b], " || ".join(cases)) if cases else reached[b]
                edge[(b, last.default)] = default if (b, last.default) not in edge else "(%s || %s)" % (edge[(b, last.default)], default)
        if prefix == "v":
            out.append("gl_Position = vec4(dt_position.x, dt_position.y, 2.0 * dt_position.z - dt_position.w, dt_position.w);")
        return "\n".join(e.decls + ["void main() {"] + ["  " + line for line in out] + ["}"])

    def _guard(self, reached, b, statement):
        if reached[b] == "true":
            return statement
        return "if (%s) { %s }" % (reached[b], statement)

    def _instruction(self, e, ins, b, reached, edge, predecessors):
        out = e.lines
        op = ins.op
        if op in ("br", "switch", "ret", "unreachable", "fence"):
            return
        value = ins.value
        if value is not None and id(value) not in self._live and not (op == "call" and _opcode(ins) == 57) \
                and op not in ("alloca", "gep"):
            return

        def define(expr, gtype=None):
            name = "x%s%d" % (e.prefix, value.index)
            e.names[id(value)] = name
            out.append("%s %s = %s;" % (gtype or _glsl_type(ins.type), name, expr))

        a = [None if v.kind in ("function",) else v for v in ins.operands]
        if op == "binop":
            x, y = e.name(a[0]), e.name(a[1])
            t = ins.type
            o = ins.opcode
            if t.kind in ("float", "half", "double"):
                expr = {"add": "%s + %s", "sub": "%s - %s", "mul": "%s * %s", "sdiv": "%s / %s",
                        "udiv": "%s / %s", "srem": "mod_trunc(%s, %s)", "urem": "mod_trunc(%s, %s)"}[o]
                if "mod_trunc" in expr:
                    expr = "(%s - %s * trunc(%s / %s))" % (x, y, x, y)
                    define(expr)
                    return
                define("(" + expr % (x, y) + ")")
            elif t.width == 1:
                define("(%s %s %s)" % (x, {"and": "&&", "or": "||", "xor": "!="}[o], y))
            else:
                expr = {"add": "(%s + %s)", "sub": "(%s - %s)", "mul": "(%s * %s)", "udiv": "(%s / %s)",
                        "urem": "(%s %% %s)", "shl": "(%s << (%s & 31u))", "lshr": "(%s >> (%s & 31u))",
                        "ashr": "uint(int(%s) >> int(%s & 31u))", "and": "(%s & %s)", "or": "(%s | %s)",
                        "xor": "(%s ^ %s)", "sdiv": "uint(int(%s) / int(%s))", "srem": "uint(int(%s) %% int(%s))"}[o]
                expr = expr % (x, y)
                if t.width < 32:
                    expr = "(%s & %du)" % (expr, (1 << t.width) - 1)
                define(expr)
        elif op == "cast":
            x = e.name(a[0])
            src, dst, o = a[0].type, ins.type, ins.opcode
            if o == "trunc":
                define("((%s & 1u) != 0u)" % x if dst.width == 1 else "(%s & %du)" % (x, (1 << dst.width) - 1) if dst.width < 32 else x)
            elif o == "zext":
                define("(%s ? 1u : 0u)" % x if src.width == 1 else x)
            elif o == "sext":
                if src.width == 1:
                    define("(%s ? 0xffffffffu : 0u)" % x)
                else:
                    define("uint(bitfieldExtract(int(%s), 0, %d))" % (x, src.width))
            elif o == "fptoui":
                define("uint(max(%s, 0.0))" % x)
            elif o == "fptosi":
                define("uint(int(%s))" % x)
            elif o == "uitofp":
                define("float(%s)" % x if src.width != 1 else "(%s ? 1.0 : 0.0)" % x)
            elif o == "sitofp":
                define("float(int(%s))" % x if src.width != 1 else "(%s ? -1.0 : 0.0)" % x)
            elif o in ("fptrunc", "fpext"):
                define(x)
            elif o == "bitcast":
                if src.kind == "int" and dst.kind == "float":
                    define("uintBitsToFloat(%s)" % x)
                elif src.kind == "float" and dst.kind == "int":
                    define("floatBitsToUint(%s)" % x)
                else:
                    define(x)
            else:
                raise TranslationError("cast " + o)
        elif op == "cmp":
            x, y = e.name(a[0]), e.name(a[1])
            p = ins.predicate
            if p < 32:
                table = {0: "false", 15: "true", 1: "(%s == %s)", 9: "(%s == %s)", 2: "(%s > %s)", 10: "(%s > %s)",
                         3: "(%s >= %s)", 11: "(%s >= %s)", 4: "(%s < %s)", 12: "(%s < %s)", 5: "(%s <= %s)",
                         13: "(%s <= %s)", 6: "(%s != %s)", 14: "(%s != %s)",
                         7: "(!isnan(%s) && !isnan(%s))", 8: "(isnan(%s) || isnan(%s))"}
                expr = table[p]
                define(expr % (x, y) if "%" in expr else expr)
            else:
                signed = p >= 38
                if signed:
                    x, y = "int(%s)" % x, "int(%s)" % y
                expr = {32: "==", 33: "!=", 34: ">", 35: ">=", 36: "<", 37: "<=", 38: ">", 39: ">=", 40: "<", 41: "<="}[p]
                define("(%s %s %s)" % (x, expr, y))
        elif op == "select":
            define("(%s ? %s : %s)" % (e.name(a[0]), e.name(a[1]), e.name(a[2])))
        elif op == "extractvalue":
            src = a[0]
            index = ins.indices[0]
            base = e.name(src)
            if index >= 4:
                define("0u" if ins.type.kind == "int" else "0.0")
            else:
                define("%s.%s" % (base, "xyzw"[index]))
        elif op == "phi":
            incoming = list(zip(a, ins.blocks))
            expr = e.name(incoming[-1][0])
            for v, p in reversed(incoming[:-1]):
                expr = "(%s ? %s : %s)" % (edge.get((p, b), "false"), e.name(v), expr)
            define(expr)
        elif op == "alloca":
            t = ins.allocated
            count = 1
            element = t
            while element.kind == "array":
                count *= element.count
                element = element.element
            name = "a%s%d" % (e.prefix, value.index)
            gtype = _glsl_type(element)
            e.lines.append("%s %s[%d];" % (gtype, name, count))
            e.pointers[id(value)] = (name, None, count)
        elif op == "gep":
            base = a[0]
            if base.kind == "global":
                name = self._global_array(e, base)
                count = _array_count(base.value_type)
            elif id(base) in e.pointers:
                name, _, count = e.pointers[id(base)]
            else:
                raise TranslationError("pointer the preview can't follow")
            indexes = a[2:] if len(a) > 2 else a[1:]
            index = e.name(indexes[-1]) if indexes else "0u"
            e.pointers[id(value)] = (name, "min(%s, %du)" % (index, max(count - 1, 0)), count)
        elif op == "load":
            ref = e.pointers.get(id(a[0]))
            if ref is None:
                if a[0].kind == "global":
                    define(self._global_array(e, a[0]) + "[0]")
                    return
                raise TranslationError("load the preview can't follow")
            name, index, _ = ref
            define("%s[%s]" % (name, index or "0"))
        elif op == "store":
            ref = e.pointers.get(id(a[0]))
            if ref is None:
                raise TranslationError("store the preview can't follow")
            name, index, _ = ref
            out.append(self._guard(reached, b, "%s[%s] = %s;" % (name, index or "0", e.name(a[1]))))
        elif op == "call":
            self._dxop(e, ins, b, reached, define)
        elif op in ("atomicrmw", "cmpxchg"):
            define("0u")
        else:
            raise TranslationError("instruction " + op)

    def _global_array(self, e, g):
        name = "g%s%d" % (e.prefix, g.index)
        if name not in e.names.values() and not any(d.startswith("const") and (" %s[" % name) in d for d in e.decls):
            init = g.init
            count = _array_count(g.value_type)
            element = g.value_type
            while element.kind == "array":
                element = element.element
            gtype = _glsl_type(element)
            if init is None or init.kind in ("null", "undef"):
                items = ["0.0" if gtype == "float" else "0u"] * count
            elif init.kind == "data":
                items = [_f(x) if gtype == "float" else "%du" % x for x in init.items]
            else:
                items = [_const(x) for x in init.elements]
            e.decls.append("const %s %s[%d] = %s[%d](%s);" % (gtype, name, count, gtype, count, ", ".join(items)))
        return name

    # ---------------------------------------------------------------------------------------------------------
    def _dxop(self, e, ins, b, reached, define):
        op = _opcode(ins)
        a = ins.operands
        n = e.name
        stage = e.stage
        if op == 57:  # CreateHandle(class, rangeId, index, nonUniform)
            klass, rid = a[1].value, a[2].value
            resource = stage.resources.get((klass, rid))
            if resource is None:
                raise TranslationError("unknown resource")
            index = None
            if klass in (0, 1, 3) and resource["count"] in (-1, 0xffffffff):
                index = e.resolve_index(a[3])
            e.handles[id(ins.value)] = {"resource": resource, "index": index}
            return
        if op == 4:  # LoadInput(sigId, row, col, axis)
            element = stage.inputs[a[1].value]
            row = a[2].value if a[2].kind in ("const", "null") else 0
            col = a[3].value
            define(self._input(e, element, row, col, ins.type))
            return
        if op == 5:  # StoreOutput(sigId, row, col, value)
            element = stage.outputs[a[1].value]
            row = a[2].value if a[2].kind in ("const", "null") else 0
            col = a[3].value
            target = self._output(e, element, row)
            if target is not None:
                value = n(a[4])
                if target.startswith("dt_io_") and element["type"] != 9:
                    value = "uint(%s)" % value if a[4].type.kind == "int" and a[4].type.width == 1 else value
                e.lines.append(self._guard(reached, b, "%s.%s = %s;" % (target, "xyzw"[col], value)))
            return
        unary = {6: "abs(%s)", 7: "clamp(%s, 0.0, 1.0)", 8: "isnan(%s)", 9: "isinf(%s)",
                 10: "(!isinf(%s) && !isnan(%s))", 12: "cos(%s)", 13: "sin(%s)", 14: "tan(%s)", 15: "acos(%s)",
                 16: "asin(%s)", 17: "atan(%s)", 18: "cosh(%s)", 19: "sinh(%s)", 20: "tanh(%s)", 21: "exp2(%s)",
                 22: "fract(%s)", 23: "log2(%s)", 24: "sqrt(%s)", 25: "inversesqrt(%s)", 26: "roundEven(%s)",
                 27: "floor(%s)", 28: "ceil(%s)", 29: "trunc(%s)", 30: "bitfieldReverse(%s)",
                 31: "uint(bitCount(%s))", 32: "uint(findLSB(%s))"}
        if op in unary:
            x = n(a[1])
            define(unary[op].replace("%s", x))
            return
        if op == 11:
            x = n(a[1])
            define("(abs(%s) >= 1.17549435e-38 && !isinf(%s) && !isnan(%s))" % (x, x, x))
            return
        if op in (33, 34):  # FirstbitHi / FirstbitSHi: bit index counted from the top
            x = n(a[1])
            src = x if op == 33 else "int(%s)" % x
            define("(%s == 0u ? 0xffffffffu : 31u - uint(findMSB(%s)))" % (x, src))
            return
        binary = {35: "max(%s, %s)", 36: "min(%s, %s)", 37: "uint(max(int(%s), int(%s)))",
                  38: "uint(min(int(%s), int(%s)))", 39: "max(%s, %s)", 40: "min(%s, %s)"}
        if op in binary:
            define(binary[op] % (n(a[1]), n(a[2])))
            return
        if op in (46, 47, 48, 49):
            define("(%s * %s + %s)" % (n(a[1]), n(a[2]), n(a[3])))
            return
        if op in (51, 52):  # Ibfe / Ubfe (width, offset, value)
            value = "int(%s)" % n(a[3]) if op == 51 else n(a[3])
            define("uint(bitfieldExtract(%s, int(%s & 31u), int(%s & 31u)))" % (value, n(a[2]), n(a[1])))
            return
        if op == 53:  # Bfi(width, offset, value, replaced)
            define("bitfieldInsert(%s, %s, int(%s & 31u), int(%s & 31u))" % (n(a[4]), n(a[3]), n(a[2]), n(a[1])))
            return
        if op in (54, 55, 56):
            k = op - 52
            x = ", ".join(n(v) for v in a[1:1 + k])
            y = ", ".join(n(v) for v in a[1 + k:1 + 2 * k])
            define("dot(vec%d(%s), vec%d(%s))" % (k, x, k, y))
            return
        if op == 131:  # LegacyF16ToF32
            define("unpackHalf2x16(%s).x" % n(a[1]))
            return
        if op == 130:  # LegacyF32ToF16
            define("packHalf2x16(vec2(%s, 0.0))" % n(a[1]))
            return
        if op in (83, 85):
            define("dFdx(%s)" % n(a[1]))
            return
        if op in (84, 86):
            define("(-dFdy(%s))" % n(a[1]))
            return
        if op == 82:  # Discard(condition)
            e.lines.append("if (%s && %s) discard;" % (reached[b], n(a[1])))
            return
        if op == 59:  # CBufferLoadLegacy(handle, register)
            buffer = e.cbuffer(e.handle(a[1]))
            register = n(a[2])
            expr = "%s.r[int(%s)]" % (buffer["glsl"], register)
            if ins.type.kind == "struct" and ins.type.members[0].kind == "int":
                expr = "floatBitsToUint(%s)" % expr
            define(expr)
            return
        if op in (60, 61, 62, 63, 64, 65, 66, 73, 81, 72):
            self._texture_op(e, ins, op, define)
            return
        if op == 68:  # BufferLoad: engine buffers are not available in the preview
            define("uvec4(0u)" if _glsl_type(ins.type) == "uvec4" else "vec4(0.0)")
            return
        if op == 71:  # CheckAccessFullyMapped
            define("true")
            return
        if op in (78, 79):  # atomics on engine buffers
            define("0u")
            return
        if op == 123:  # QuadOp: the pixel's own value
            define(n(a[1]))
            return
        if op in (110, 113, 114, 115):
            define("true" if op in (110, 115) else n(a[1]))
            return
        if op in (111,):
            define("0u")
            return
        if op in (112,):
            define("1u")
            return
        if op in (117, 118, 119, 120, 121, 122):
            define(n(a[1]))
            return
        if op == 91:
            define("0xffffffffu")
            return
        raise TranslationError("DXIL operation %d" % op)

    def _texture_op(self, e, ins, op, define):
        a = ins.operands
        n = e.name
        handle = e.handle(a[1])
        resource = handle["resource"]
        if op == 72:  # GetDimensions(handle, mip)
            if resource.get("shape") in BUFFER_KINDS:
                define("uvec4(1u, 0u, 0u, 0u)")
                return
            texture = e.texture(handle)
            if texture["dim"] in ("2D", "CUBE"):
                define("uvec4(uvec2(textureSize(%s, int(%s))), 0u, uint(textureQueryLevels(%s)))" % (texture["glsl"], n(a[2]), texture["glsl"]))
            else:
                define("uvec4(uvec3(textureSize(%s, int(%s))), uint(textureQueryLevels(%s)))" % (texture["glsl"], n(a[2]), texture["glsl"]))
            return
        if resource.get("shape") in BUFFER_KINDS:
            define("vec4(0.0)")
            return
        sampler = a[2] if op not in (66,) else None
        texture = e.texture(handle, e.handle(sampler) if sampler is not None and op != 66 else None)
        dim = texture["dim"]
        size = {"1D": 1, "2D": 2, "3D": 3, "CUBE": 3, "2D_ARRAY": 3, "1D_ARRAY": 2, "CUBE_ARRAY": 4}[dim]
        glsl_vec = {1: "float", 2: "vec2", 3: "vec3", 4: "vec4"}[size]
        if op == 66:  # TextureLoad(srv, mip, c0, c1, c2, o0, o1, o2)
            coords = [n(v) for v in a[3:3 + size]]
            ivec = {1: "int", 2: "ivec2", 3: "ivec3"}[min(size, 3)]
            define("texelFetch(%s, %s(%s), int(%s))" % (texture["glsl"], ivec, ", ".join("int(%s)" % c for c in coords[:min(size, 3)]), n(a[2])))
            return
        coords = "%s(%s)" % (glsl_vec, ", ".join(n(v) for v in a[3:3 + size])) if size > 1 else n(a[3])
        offsets = [v for v in a[7:10][:min(size, 3)] if v.kind == "const" and v.value]
        if offsets:
            # constant texel offsets: shift the coordinates by texels of mip 0
            offset_vec = "%s(%s)" % (glsl_vec, ", ".join("float(int(%s))" % n(v) for v in a[7:7 + size]))
            coords = "(%s + %s / vec%d(textureSize(%s, 0)))" % (coords, offset_vec, size, texture["glsl"]) if size > 1 else coords
        t = texture["glsl"]
        if op == 60:
            define("texture(%s, %s)" % (t, coords))
        elif op == 61:
            define("texture(%s, %s, %s)" % (t, coords, n(a[10])))
        elif op == 62:
            define("textureLod(%s, %s, %s)" % (t, coords, n(a[10])))
        elif op == 63:
            dx = "%s(%s)" % (glsl_vec, ", ".join(n(v) for v in a[10:10 + size])) if size > 1 else n(a[10])
            dy = "%s(%s)" % (glsl_vec, ", ".join("-" + n(v) for v in a[13:13 + size])) if size > 1 else "-" + n(a[13])
            define("textureGrad(%s, %s, %s, %s)" % (t, coords, dx, dy))
        elif op in (64, 65):  # comparison sampling = shadow maps: fully lit in the preview
            define("vec4(1.0)")
        elif op == 73:  # TextureGather(srv, sampler, c0..c3, o0, o1, channel)
            define("textureGather(%s, %s, int(%s))" % (t, coords, n(a[9])))
        elif op == 81:  # CalculateLOD(handle, sampler, c0, c1, c2, clamped)
            clamped = a[6].value if a[6].kind in ("const", "null") else 1
            define("textureQueryLod(%s, %s).%s" % (t, coords, "x" if clamped else "y"))

    # ---------------------------------------------------------------------------------------------------------
    def _input(self, e, element, row, col, vtype):
        system = element["system"]
        stage_kind = e.stage.kind
        if stage_kind == 0:  # pixel
            if system == SV_POSITION:
                return ["gl_FragCoord.x", "(dt_viewport.w - gl_FragCoord.y)", "gl_FragCoord.z", "(1.0 / gl_FragCoord.w)"][col]
            if system == SV_IS_FRONT_FACE:
                return "gl_FrontFacing" if vtype.kind == "int" and vtype.width == 1 else "(gl_FrontFacing ? 0xffffffffu : 0u)"
            semantic = "%s%d" % (element["semantic"], element["indexes"][row])
            name = self._varying_names.get(semantic)
            if name is None:
                return "0.0" if vtype.kind == "float" else "0u"
            return "%s.%s" % (name, "xyzw"[col])
        # vertex
        if system in (SV_INSTANCE_ID, SV_VERTEX_ID):
            name = "dt_instance" if system == SV_INSTANCE_ID else "dt_vertex"
            self._attribute(name, system)
            return "uint(%s)" % name
        semantic = "%s%d" % (element["semantic"], element["indexes"][row])
        name = "dt_in_" + semantic.lower()
        self._attribute(name, semantic)
        expr = "%s.%s" % (name, "xyzw"[col])
        if vtype.kind == "int":
            return "uint(%s)" % expr
        return expr

    def _attribute(self, name, semantic):
        if all(name != n for n, _ in self.attributes):
            self.attributes.append((name, semantic))

    def _output(self, e, element, row):
        system = element["system"]
        if e.stage.kind == 1:
            if system == SV_POSITION:
                return "dt_position"
            semantic = "%s%d" % (element["semantic"], element["indexes"][row])
            return self._varying_names.get(semantic)
        if system == SV_TARGET and element["indexes"][row] == 0:
            return "dt_frag"
        return None


def _array_count(t):
    count = 1
    while t.kind == "array":
        count *= t.count
        t = t.element
    return count
