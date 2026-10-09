"""The state graph's transitions as the compiler gets them: rows marked "from any state" and event names with *
spelled out into one transition per state and event.

A row that names its state and event outright wins over generated ones for the same state and event; among generated
ones the first row wins.
"""
import fnmatch
import re
from types import SimpleNamespace

FIELDS = ("from_state", "to_state", "event_name", "blend_seconds", "condition", "variable_index", "lower", "upper",
          "lower_exclusive", "upper_exclusive")


def clip_names(options):
    """Clip index -> clip name from the last source inspection."""
    names = {}
    for line in options.inspection_summary.splitlines():
        match = re.match(r"\s*\[(\d+)\] (.*?) duration=", line)
        if match:
            names[int(match.group(1))] = match.group(2).strip().strip('"')
    return names


def clips_text(options, text):
    """A clip list (clip:value, clip:x:y or clip:weight) with clip names turned into their numbers."""
    index = {name: number for number, name in clip_names(options).items()}
    parts = []
    for part in text.split(","):
        part = part.strip().replace(" ", "")
        if not part:
            continue
        clip, _, rest = part.partition(":")
        if not clip.isdigit():
            if clip not in index:
                raise ValueError("No clip named " + clip + " (Inspect Asset lists them)")
            clip = str(index[clip])
        parts.append(clip + (":" + rest if rest else ""))
    return ",".join(parts)


def game_events(options):
    """The events listed by Import Game Events (names, or #1234abcd for ones only known by hash)."""
    return [name for name in (part.strip() for part in options.state_graph_game_events.split(",")) if name]


def known_events(options):
    """Every event name the graph knows: game events, timed and exit events, and transition events without a *."""
    names = set(game_events(options))
    for state in options.state_graph_states:
        for part in state.events_at.split(","):
            if ":" in part and part.split(":", 1)[1].strip():
                names.add(part.split(":", 1)[1].strip())
        if state.exit_event.strip():
            names.add(state.exit_event.strip())
    for transition in options.state_graph_transitions:
        name = transition.event_name.strip()
        if name and "*" not in name:
            names.add(name)
    return names


def expand(options):
    states = options.state_graph_states
    rows = list(options.state_graph_transitions)
    explicit = {(row.from_state, row.event_name.strip()) for row in rows
                if not row.from_any and "*" not in row.event_name}
    events = sorted(known_events(options))
    generated = set()
    result = []
    for row in rows:
        name = row.event_name.strip()
        pattern = "*" in name
        names = [event for event in events if fnmatch.fnmatchcase(event, name)] if pattern else [name]
        if row.from_any and row.to_state < len(states):
            layer = states[row.to_state].layer
            sources = [index for index, state in enumerate(states) if index != row.to_state and state.layer == layer]
        else:
            sources = [row.from_state]
        for source in sources:
            for event in names:
                if row.from_any or pattern:
                    key = (source, event)
                    if key in explicit:
                        continue
                    if row.condition != "range":
                        if key in generated:
                            continue
                        generated.add(key)
                values = {field: getattr(row, field) for field in FIELDS}
                values.update(from_state=source, event_name=event)
                result.append(SimpleNamespace(**values))
    return result
