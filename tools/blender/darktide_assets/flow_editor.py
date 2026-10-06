"""Darktide unit flow as a Blender node tree ("Darktide Flow" in the node editor), like Stingray's flow editor.
Node types, inputs, outputs and events come from flow_schema.json (compiler --flow-schema). Yellow sockets are
events (what happens when), the others carry values. A unit input left open means the unit itself.
At export the tree becomes the unit's flow description (the compiler builds the game's flow graph from it)."""
import json
import os

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, StringProperty

TREE = "DarktideFlowTree"

LABELS = {
    "unit_spawned": "Unit Spawned",
    "unit_unspawned": "Unit Unspawned",
    "external_event": "Flow Event",
    "particle_effect": "Particle Effect",
    "set_unit_visibility": "Set Visibility",
    "animation_event": "Animation Event",
    "delay": "Delay",
    "once": "Once",
    "gate": "Gate",
    "branch": "Branch",
    "compare": "Compare",
    "fork": "Fork",
    "counter": "Counter",
    "random_float": "Random Number",
    "vector3": "Vector3",
    "get_mesh": "Get Mesh",
    "get_material": "Get Material",
    "set_material_variable": "Set Material Variable",
    "get_light": "Get Light",
    "set_light_intensity": "Set Light Intensity",
    "set_light_color": "Set Light Color",
    "get_data_bool": "Get Unit Data (bool)",
    "get_data_number": "Get Unit Data (number)",
    "lua": "Call Lua",
}
DESCRIPTIONS = {
    "unit_spawned": "Fires when the unit spawns; its unit output is the unit itself",
    "unit_unspawned": "Fires when the unit is destroyed (despawned, removed with its level)",
    "external_event": "Fires when a script calls Unit.flow_event(unit, \"<event>\")",
    "particle_effect": "Plays a particle effect (create), stops spawning (stop) or removes it (kill)",
    "set_unit_visibility": "Shows or hides a visibility group, or the whole unit when the group is empty",
    "animation_event": "Sends an event to the unit's animation state machine",
    "delay": "Fires its output after the given time (seconds)",
    "once": "Lets the first event through, then nothing",
    "gate": "Lets events through (pass) while open; open, close and toggle switch it",
    "branch": "Fires true or false depending on the condition",
    "compare": "Compares a with b and fires every output that holds (less, less or equal, equal, ...)",
    "fork": "Fires its outputs one after the other, 1 to 8",
    "counter": "Holds a number: starts at start, add and subtract change it by step, reset puts it back",
    "random_float": "A random number between min and max, new each time something reads it",
    "vector3": "Builds a vector3 from x, y and z",
    "get_mesh": "One of the unit's meshes by name (the object's name in blender)",
    "get_material": "A material of a mesh by name (the material's name in blender)",
    "set_material_variable": "Sets a variable of a material, e.g. emissive_intensity; value for numbers, vector for colors",
    "get_light": "One of the unit's lights by name (the lamp's name in blender)",
    "set_light_intensity": "Sets a light's brightness (blender watts x 60, so 600 is a 10 W lamp)",
    "set_light_color": "Sets a light's color (red, green, blue from 0 to 1)",
    "get_data_bool": "Reads a true/false value from the unit's data (the asset collection's custom properties)",
    "get_data_number": "Reads a number from the unit's data (the asset collection's custom properties)",
    "lua": "Calls a Lua function (FlowCallbacks.<function> unless another class is set) with the inputs as a table; "
           "the table it returns sets the outputs and fires the output events set to true (out fires unless set false)",
}
INPUT_HINTS = {
    "effect": "Particle effect resource path, e.g. content/fx/particles/...",
    "object": "Node of the unit the effect sits on (empty = the unit's root)",
    "group": "Visibility group name (empty = the whole unit)",
    "event": "Animation state machine event name",
    "orphaned_policy": "When the unit goes away: 0 destroy the effect, 1 stop spawning, 2 unlink and let it finish",
}


def _schema():
    if not hasattr(_schema, "cache"):
        with open(os.path.join(os.path.dirname(__file__), "flow_schema.json"), "r", encoding="utf-8") as source:
            _schema.cache = json.load(source)
    return _schema.cache


class DarktideFlowTree(bpy.types.NodeTree):
    """Darktide unit flow: events and what they do"""
    bl_idname = TREE
    bl_label = "Darktide Flow"
    bl_icon = "NODETREE"


class _FlowSocket:
    kind = ""

    def draw(self, context, layout, node, text):
        if self.is_output or self.is_linked or not hasattr(self, "default_value"):
            layout.label(text=text)
        else:
            layout.prop(self, "default_value", text=text)


def _socket_class(kind, color, prop=None):
    attrs = {"bl_idname": "DarktideFlowSocket_" + kind, "bl_label": kind.capitalize(), "kind": kind,
             "draw_color": lambda self, context, node: color}
    if prop is not None:
        attrs["__annotations__"] = {"default_value": prop}
    return type("DarktideFlowSocket_" + kind, (_FlowSocket, bpy.types.NodeSocket), attrs)


SOCKETS = {
    "event": _socket_class("event", (1.0, 0.8, 0.1, 1.0)),
    "unit": _socket_class("unit", (0.3, 0.6, 1.0, 1.0)),
    "bool": _socket_class("bool", (0.8, 0.65, 0.84, 1.0), BoolProperty(default=False)),
    "uint": _socket_class("uint", (0.35, 0.55, 0.36, 1.0), IntProperty(default=0, min=0)),
    "float": _socket_class("float", (0.63, 0.63, 0.63, 1.0), FloatProperty(default=0.0)),
    "vector3": _socket_class("vector3", (0.39, 0.39, 0.78, 1.0), FloatVectorProperty(size=3, default=(0.0, 0.0, 0.0))),
    "quaternion": _socket_class("quaternion", (0.39, 0.29, 0.78, 1.0),
                                FloatVectorProperty(size=4, default=(0.0, 0.0, 0.0, 1.0))),
    "string": _socket_class("string", (0.44, 0.7, 1.0, 1.0), StringProperty(default="")),
    "name": _socket_class("name", (0.44, 0.85, 0.85, 1.0), StringProperty(default="")),
    "mesh": _socket_class("mesh", (0.9, 0.45, 0.2, 1.0)),
    "material": _socket_class("material", (0.9, 0.3, 0.55, 1.0)),
    "light": _socket_class("light", (0.95, 0.85, 0.3, 1.0)),
}
# inputs whose starting value differs from the socket default
INPUT_DEFAULTS = {("set_unit_visibility", "visible"): True}


class _FlowNode:
    flow_type = ""

    @classmethod
    def poll(cls, tree):
        return tree.bl_idname == TREE

    def init(self, context):
        spec = _schema()[self.flow_type]
        for event in spec["in_events"]:
            self.inputs.new(SOCKETS["event"].bl_idname, event)
        for name, kind in spec["inputs"].items():
            socket = self.inputs.new(SOCKETS[kind].bl_idname, name)
            if (self.flow_type, name) in INPUT_DEFAULTS:
                socket.default_value = INPUT_DEFAULTS[(self.flow_type, name)]
        for event in spec["out_events"]:
            self.outputs.new(SOCKETS["event"].bl_idname, event)
        for name, kind in spec["outputs"].items():
            self.outputs.new(SOCKETS[kind].bl_idname, name)
        if self.flow_type == "set_material_variable":
            _show_material_value(self, context)

    def draw_buttons(self, context, layout):
        if self.flow_type == "external_event":
            layout.prop(self, "event")
        if self.flow_type == "set_material_variable":
            layout.prop(self, "value_kind", expand=True)


KINDS = ("unit", "bool", "uint", "float", "vector3", "quaternion", "string", "name")


def _parse_slots(text):
    """'unit:unit, amount:float' -> [("unit", "unit"), ("amount", "float")]"""
    slots = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        name, _, kind = (piece.strip() for piece in part.partition(":"))
        if not name or "." in name or kind not in KINDS:
            raise ValueError("'%s': write name:kind, kind one of %s" % (part, ", ".join(KINDS)))
        slots.append((name, kind))
    return slots


def _parse_events(text):
    return [name.strip() for name in text.split(",") if name.strip()]


def _rebuild_lua(self, context):
    """Sockets follow the typed lists; a socket whose name and kind stay keeps its links and value."""
    try:
        params, returns, events = _parse_slots(self.params), _parse_slots(self.returns), _parse_events(self.events)
        names = [n for n, _ in params] + [n for n, _ in returns] + events
        if not events or len(set(names)) != len(names) or "node_id" in names:
            raise ValueError("names must be unique, node_id is taken and there is at least one output event")
    except ValueError as error:
        self.problem = str(error)
        return
    self.problem = ""

    def sync(sockets, wanted):
        for socket in list(sockets):
            if (socket.name, socket.kind) not in wanted:
                sockets.remove(socket)
        for index, (name, kind) in enumerate(wanted):
            existing = next((s for s in sockets if s.name == name and s.kind == kind), None)
            if existing is None:
                existing = sockets.new(SOCKETS[kind].bl_idname, name)
            sockets.move(list(sockets).index(existing), index)

    sync(self.inputs, [("in", "event")] + params)
    sync(self.outputs, [(name, "event") for name in events] + returns)


class DarktideFlowNode_lua(bpy.types.Node):
    """Calls a Lua function from flow"""
    bl_idname = "DarktideFlowNode_lua"
    bl_label = LABELS["lua"]
    bl_description = DESCRIPTIONS["lua"]
    flow_type = "lua"
    function: StringProperty(name="Function", description="Function to call: FlowCallbacks.<function>, which your mod "
                             "can add (FlowCallbacks.my_function = function(params) ... end)")
    lua_class: StringProperty(name="Class", default="FlowCallbacks",
                              description="Lua table holding the function (FlowCallbacks for the game's and mods' "
                              "flow functions)")
    params: StringProperty(name="Inputs", update=_rebuild_lua,
                           description="Values passed in params, as name:kind separated by commas, e.g. unit:unit, "
                           "amount:float (kinds: %s)" % ", ".join(KINDS))
    returns: StringProperty(name="Outputs", update=_rebuild_lua,
                            description="Values read back from the returned table, as name:kind")
    events: StringProperty(name="Events", default="out", update=_rebuild_lua,
                           description="Output events, comma separated; each fires when the returned table sets it "
                           "true (out fires unless set false)")
    problem: StringProperty()

    @classmethod
    def poll(cls, tree):
        return tree.bl_idname == TREE

    def init(self, context):
        _rebuild_lua(self, context)

    def draw_buttons(self, context, layout):
        layout.prop(self, "function")
        layout.prop(self, "lua_class")
        layout.prop(self, "params")
        layout.prop(self, "returns")
        layout.prop(self, "events")
        if self.problem:
            layout.label(text=self.problem, icon="ERROR")


def _show_material_value(self, context):
    """Set Material Variable writes either a number or a vector; the other input stays unset."""
    self.inputs["value"].hide = self.value_kind != "value"
    self.inputs["vector"].hide = self.value_kind != "vector"


def _node_class(flow_type):
    attrs = {"bl_idname": "DarktideFlowNode_" + flow_type, "bl_label": LABELS.get(flow_type, flow_type),
             "bl_description": DESCRIPTIONS.get(flow_type, ""), "flow_type": flow_type}
    if flow_type == "external_event":
        attrs["__annotations__"] = {"event": StringProperty(
            name="Event", description="Name scripts send with Unit.flow_event(unit, name)")}
    if flow_type == "set_material_variable":
        attrs["__annotations__"] = {"value_kind": EnumProperty(
            name="Value", update=_show_material_value, default="value",
            items=[("value", "Number", "Set a number variable (e.g. emissive_intensity) from value"),
                   ("vector", "Color / vector", "Set a color or vector variable (e.g. emissive_color) from vector")])}
    return type("DarktideFlowNode_" + flow_type, (_FlowNode, bpy.types.Node), attrs)


NODES = [DarktideFlowNode_lua if flow_type == "lua" else _node_class(flow_type) for flow_type in _schema()]


def _value(socket):
    value = socket.default_value
    if socket.kind in ("vector3", "quaternion"):
        return [float(v) for v in value]
    if socket.kind == "float":
        return float(value)
    if socket.kind == "uint":
        return int(value)
    return value


def _node_id(node):
    return node.name.replace(".", "_")


def description(tree):
    """The flow description the compiler takes (stingray/flow/flow_authoring.h)."""
    nodes, links, connections = [], [], []
    for node in tree.nodes:
        if not isinstance(node, (_FlowNode, DarktideFlowNode_lua)):
            continue
        record = {"id": _node_id(node), "type": node.flow_type}
        if node.flow_type == "lua":
            if node.problem:
                raise ValueError("Flow node '%s': %s" % (node.name, node.problem))
            if not node.function.strip():
                raise ValueError("Flow node '%s' needs a function name" % node.name)
            record["function"] = node.function.strip()
            if node.lua_class.strip() and node.lua_class.strip() != "FlowCallbacks":
                record["class"] = node.lua_class.strip()
            record["params"] = dict(_parse_slots(node.params))
            record["returns"] = dict(_parse_slots(node.returns))
            record["events"] = _parse_events(node.events)
        if node.flow_type == "external_event":
            if not node.event.strip():
                raise ValueError("Flow node '%s' needs an event name" % node.name)
            record["event"] = node.event.strip()
        inputs = {}
        for socket in node.inputs:
            if socket.kind in ("event", "unit") or socket.is_linked or not hasattr(socket, "default_value"):
                continue
            if node.flow_type == "set_material_variable" and socket.name in ("value", "vector") and \
                    socket.name != node.value_kind:
                continue
            value = _value(socket)
            if socket.kind in ("string", "name"):
                value = value.strip()
                if not value:
                    continue
            inputs[socket.name] = value
        if inputs:
            record["inputs"] = inputs
        nodes.append(record)
    for link in tree.links:
        flow_nodes = (_FlowNode, DarktideFlowNode_lua)
        if not (isinstance(link.from_node, flow_nodes) and isinstance(link.to_node, flow_nodes)) or not link.is_valid:
            continue
        pair = [_node_id(link.from_node) + "." + link.from_socket.name, _node_id(link.to_node) + "." + link.to_socket.name]
        if link.from_socket.kind == "event":
            links.append(pair)
        else:
            connections.append(pair)
    return {"nodes": nodes, "links": links, "connections": connections}


def _add_menu(self, context):
    if context.space_data.tree_type != TREE:
        return
    for node in NODES:
        operator = self.layout.operator("node.add_node", text=node.bl_label)
        operator.type = node.bl_idname
        operator.use_transform = True


def register():
    bpy.utils.register_class(DarktideFlowTree)
    for socket in SOCKETS.values():
        bpy.utils.register_class(socket)
    for node in NODES:
        bpy.utils.register_class(node)
    bpy.types.NODE_MT_add.append(_add_menu)


def unregister():
    bpy.types.NODE_MT_add.remove(_add_menu)
    for node in reversed(NODES):
        bpy.utils.unregister_class(node)
    for socket in reversed(list(SOCKETS.values())):
        bpy.utils.unregister_class(socket)
    bpy.utils.unregister_class(DarktideFlowTree)
