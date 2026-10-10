"""Read the events and variables of the game's own state machines (.state_machine in the limn extract).

A unit that takes over a game character's scripts must list every event those scripts send: the engine stops the game
on an event its state machine doesn't have. Names come from animation_names.json (the game's script strings); events
only known by hash stay "#1234abcd", which the compiler writes back as that hash.
"""
import json
import os
import struct

from .reference_skeleton import ReferenceSkeletonError, _body, _murmur64

_NAMES = None


def names():
    global _NAMES
    if _NAMES is None:
        with open(os.path.join(os.path.dirname(__file__), "animation_names.json"), encoding="utf-8") as source:
            _NAMES = {int(key, 16): value for key, value in json.load(source).items()}
    return _NAMES


class _Reader:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def take(self, size):
        if size < 0 or self.offset + size > len(self.data):
            raise ReferenceSkeletonError("state machine is truncated")
        value = self.data[self.offset:self.offset + size]
        self.offset += size
        return value

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]

    def count(self, limit=1_000_000):
        value = self.u32()
        if value > limit:
            raise ReferenceSkeletonError("state machine holds an implausible list")
        return value

    def u32s(self):
        return [self.u32() for _ in range(self.count())]


def _skip_state(r):
    # mirrors the engine's state loader
    r.take(24 + 4)
    r.take(8 * r.count())                        # animations
    r.take(4 * r.count())                        # thresholds
    r.take(4 + 1 + 4)
    r.take(9 * r.count())                        # timeline markers
    r.take(28 * r.count())                       # transitions
    for _ in range(r.count()):                   # selectors
        r.u32()
        r.take(14 * r.count())
    r.take(8)
    r.take(12 * r.count())
    r.take(8)
    r.u32s()
    r.u32s()
    r.take(16)
    r.u32s()
    r.take(12)


def read(blob):
    """(event names, [(variable name, default, minimum, maximum)]) of a cooked .state_machine. The engine's last
    variable (the clip length it fills in itself) is left out."""
    body, _ = _body(blob, "STATE_MACHINE")
    r = _Reader(body)
    for _ in range(r.count()):
        for _ in range(r.count()):
            _skip_state(r)
        r.u32()
    events = r.u32s()
    variables = r.u32s()
    defaults = [struct.unpack("<f", struct.pack("<I", value))[0] for value in r.u32s()]
    bounds = [(r.f32(), r.f32()) for _ in range(r.count())]
    table = names()
    event_names = [table.get(value, "#%08x" % value) for value in events]
    result = []
    for index, value in enumerate(variables[:-1]):
        low, high = bounds[index] if index < len(bounds) else (0.0, 1.0)
        default = defaults[index] if index < len(defaults) else 0.0
        result.append((table.get(value, "#%08x" % value), default, min(low, default), max(high, default)))
    return event_names, result


def unit_state_machine(folder, resource):
    """The resource path of the state machine a game unit carries (empty when it has none)."""
    from .game_unit import read_unit_tail
    stem = "%016x" % _murmur64(resource)
    path = os.path.join(folder, stem + ".unit")
    if not os.path.isfile(path):
        raise ValueError(resource + " is not in the extract folder (looked for " + stem + ".unit)")
    with open(path, "rb") as source:
        tail = read_unit_tail(source.read())
    if tail["state_machine"] is None:
        raise ValueError("Could not read the state machine name of " + resource)
    return tail["state_machine"]


def load(folder, resource):
    """Events and variables of a game unit's state machine, or of a state machine named directly."""
    machine = resource
    if not os.path.isfile(os.path.join(folder, "%016x.state_machine" % _murmur64(resource))):
        machine = unit_state_machine(folder, resource)
        if not machine:
            raise ValueError(resource + " has no state machine of its own (the game may set one from its scripts; "
                             "type that state machine's path instead)")
    path = os.path.join(folder, "%016x.state_machine" % _murmur64(machine))
    if not os.path.isfile(path):
        raise ValueError(machine + " is not in the extract folder; extract the state_machine type with limn too")
    with open(path, "rb") as source:
        events, variables = read(source.read())
    return machine, events, variables
