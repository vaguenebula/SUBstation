"""A built-in device's state besides its parameters (a sampler's sample), as
the engine stores it (BuiltinProcessor::encodeState): named text values, one
per line, "name=value", where a backslash escapes a backslash ("\\\\") or a
newline ("\\n"). The model keeps it, base64-encoded, in Device.state, as it
keeps a plug-in's state."""

from __future__ import annotations

import base64
import binascii


def encode(values: dict[str, str]) -> bytes:
    lines = []
    for name, value in sorted(values.items()):
        escaped = value.replace("\\", "\\\\").replace("\n", "\\n")
        lines.append(f"{name}={escaped}\n")
    return "".join(lines).encode()


def decode(state: bytes) -> dict[str, str]:
    values = {}
    for line in state.decode(errors="replace").split("\n"):
        name, equals, raw = line.partition("=")
        if not equals:
            continue
        value, i = [], 0
        while i < len(raw):
            if raw[i] == "\\" and i + 1 < len(raw):
                i += 1
                value.append("\n" if raw[i] == "n" else raw[i])
            else:
                value.append(raw[i])
            i += 1
        values[name] = "".join(value)
    return values


def to_model(values: dict[str, str]) -> str | None:
    """Values as Device.state keeps them (None for none)."""
    return base64.b64encode(encode(values)).decode("ascii") if values else None


def from_model(state: str | None) -> dict[str, str]:
    """A built-in device's values from its Device.state (none if it has none, or it's unreadable)."""
    if not state:
        return {}
    try:
        return decode(base64.b64decode(state))
    except (ValueError, binascii.Error):
        return {}
