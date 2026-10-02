"""Parameters, whatever they belong to: a track's or the master's mixer, a
built-in device, or a plug-in of any format.

A ParamSpec describes one: its name, range, units and how its values read. It
maps plain values (in the parameter's own units) to and from normalized ones
(0..1), which is how automation stores them. Device parameters come from the
engine's ParamInfo (ParamSpec.from_info) and map as it does; the mixer's
controls (volume, pan and sends) are described here (mixer_specs). The UI
shows every kind alike."""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass, field

from . import automation
from .timebase import format_db, format_pan


def format_value(value: float, unit: str) -> str:
    """A built-in device's value, in its units."""
    if unit == "dB":
        return f"{value:.1f} dB"
    if unit == "%":
        return f"{value:.0f} %"
    if unit == "":
        return f"{value:+.2f}" if value else "0.00"
    if unit == "Hz":
        return f"{value / 1000:.2f} kHz" if value >= 1000 else f"{value:.0f} Hz"
    if unit == "ms":
        if value >= 1000:
            return f"{value / 1000:.2f} s"
        return f"{value:.1f} ms" if value < 10 else f"{value:.0f} ms"
    return f"{value:.2f} {unit}"


@dataclass(frozen=True)
class ParamSpec:
    key: str  # its automation key (see automation.py)
    name: str
    group: str  # "Mixer", or its device's name
    minimum: float = 0.0
    maximum: float = 1.0
    default: float = 0.0
    unit: str = ""
    scale: str = "linear"  # "linear", "log" (moves evenly in log(value)) or "fader" (a mixer's volume)
    steps: int = 0  # > 0: discrete, this many steps from minimum to maximum
    labels: tuple[str, ...] = ()  # names of the steps of a list
    # Plain value -> text; by default from the units (or the labels).
    text: Callable[[float], str] | None = field(default=None, compare=False)

    @classmethod
    def from_info(cls, info, key: str, group: str, text: Callable[[float], str] | None = None) -> ParamSpec:
        """A device parameter, from the engine's ParamInfo."""
        log = info.log_scale and info.min_value > 0 and info.max_value > info.min_value
        return cls(key=key, name=info.name, group=group, minimum=info.min_value, maximum=info.max_value,
                   default=info.default_value, unit=info.unit, scale="log" if log else "linear",
                   steps=info.step_count, labels=tuple(info.value_labels), text=text)

    @property
    def discrete(self) -> bool:
        return self.steps > 0

    # The same mapping as the engine's ParamInfo (and the mixer's in automation.py).
    def to_normalized(self, plain: float) -> float:
        if self.scale == "fader":
            return automation.volume_to_normalized(plain)
        span = self.maximum - self.minimum
        if span <= 0:
            return 0.0
        if self.steps > 0:
            return min(float(self.steps), max(0.0, round(plain - self.minimum))) / self.steps
        plain = min(self.maximum, max(self.minimum, plain))
        if self.scale == "log":
            return math.log(plain / self.minimum) / math.log(self.maximum / self.minimum)
        return (plain - self.minimum) / span

    def from_normalized(self, value: float) -> float:
        value = min(1.0, max(0.0, value))
        if self.scale == "fader":
            return automation.normalized_to_volume(value)
        if self.steps > 0:
            return self.minimum + min(float(self.steps), math.floor(value * (self.steps + 1)))
        if self.scale == "log":
            return self.minimum * (self.maximum / self.minimum) ** value
        return self.minimum + value * (self.maximum - self.minimum)

    def quantize(self, value: float) -> float:
        """A normalized value as the parameter can take it."""
        return self.to_normalized(self.from_normalized(value)) if self.steps > 0 else value

    def format(self, plain: float) -> str:
        if self.text is not None:
            return self.text(plain)
        if self.labels:
            index = round(plain - self.minimum)
            return self.labels[max(0, min(len(self.labels) - 1, index))]
        return format_value(plain, self.unit)

    def format_normalized(self, value: float) -> str:
        return self.format(self.from_normalized(value))


def mixer_specs(master: bool = False, sends: tuple[tuple[str, str], ...] = ()) -> list[ParamSpec]:
    """The mixer controls of a track (or the master) that can be automated: its
    volume and pan, and its sends to `sends` ((return id, letter) each)."""
    who = "Master" if master else "Track"
    return [
        ParamSpec(automation.MIXER_VOLUME, f"{who} Volume", "Mixer", automation.MIN_VOLUME_DB,
                  automation.MAX_VOLUME_DB, 0.0, "dB", scale="fader", text=format_db),
        ParamSpec(automation.MIXER_PAN, f"{who} Pan", "Mixer", -1.0, 1.0, 0.0, text=format_pan),
        *(send_spec(return_id, letter) for return_id, letter in sends),
    ]


def send_spec(return_id: str, letter: str) -> ParamSpec:
    """A send's level to a return (lettered as it shows), automated as a volume is."""
    return ParamSpec(automation.send_key(return_id), f"Send {letter}", "Mixer", automation.MIN_VOLUME_DB,
                     automation.MAX_VOLUME_DB, automation.MIN_VOLUME_DB, "dB", scale="fader", text=format_db)
