"""What the layer needs to know from the engine and the UI, as narrow protocols,
so it never imports either: EngineFacts (implemented by the bridge's adapter,
qt/engine_facts.py) and UiFacts (implemented by the main window,
ui/intel/facts.py). When the engine's API changes names, only the adapter
changes; tests pass fakes, or None where a fact isn't needed."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Protocol

from ..model.params import ParamSpec
from ..model.project import Freeze


@dataclass(frozen=True)
class TransportState:
    playing: bool
    recording: bool
    position: float  # beats


@dataclass(frozen=True)
class MeterStats:
    """A strip's meter readings over the last `seconds` (linear peaks, 1.0 = 0 dBFS)."""

    seconds: float
    readings: int
    peak: float  # the loudest reading, either channel
    clipped: int  # readings at or over 0 dBFS


@dataclass(frozen=True)
class SampleView:
    """Decoded audio: (channels, frames) float32, at `sample_rate`."""

    samples: Any
    sample_rate: float


@dataclass(frozen=True)
class AvailablePlugin:
    """A plug-in the scan found (what the browser lists)."""

    uid: str
    name: str
    vendor: str
    category: str  # VST3 sub-categories, e.g. "Instrument|Synth", "Fx|Reverb"
    instrument: bool
    path: str
    format: str = "VST3"


@dataclass(frozen=True)
class SelectionFacts:
    """The arrangement's selection and what the device view shows, as plain values."""

    time_range: tuple[float, float] | None = None  # beats
    range_tracks: tuple[str, ...] = ()  # the tracks a time range spans
    clips: tuple[tuple[str, str], ...] = ()  # (track id, clip id)
    lanes: tuple[tuple[str, str], ...] = ()  # automation lanes in a time range: (owner, key)
    points: tuple[str, str, tuple[int, ...]] | None = None  # selected breakpoints: owner, key, indices
    tracks: tuple[str, ...] = ()  # selected tracks (headers)
    track: str | None = None  # the focused track
    device: tuple[str, str] | None = None  # (track id, device id) shown in the device view
    insert_beat: float = 0.0
    extra: dict = field(default_factory=dict)


class EngineFacts(Protocol):
    """Read-only questions to the engine (and the few transport and freeze
    actions the UI also takes through the bridge)."""

    def param_specs(self, track_id: str, device_id: str) -> list[ParamSpec] | None:
        """A device's automatable parameters; None if it isn't loaded (a missing plug-in, a frozen track)."""

    def param_text(self, track_id: str, device_id: str, param_id: str, plain: float) -> str | None:
        """The device's own text for a value of its parameter (a plug-in's: "-3.0 dB", "Bell")."""

    def own_value(self, owner: str, key: str) -> float | None:
        """A target's value as set by hand (plain), where the engine knows it better than the model (plug-ins)."""

    def is_automated(self, owner: str, key: str) -> bool: ...

    def is_overridden(self, owner: str, key: str) -> bool: ...

    def override_automation(self, owner: str, key: str) -> None:
        """A target set by hand: its envelope stops playing until re-enabled (as turning its knob does)."""

    def re_enable_automation(self, owner: str | None = None) -> None: ...

    def meter_history(self, strip_id: str, seconds: float) -> MeterStats: ...

    def decoded(self, path: str) -> SampleView | None: ...

    def file_duration(self, path: str) -> float | None:
        """An audio file's length in seconds from its header; None if it can't be read."""

    def transport(self) -> TransportState: ...

    def play(self) -> None: ...

    def stop(self) -> None: ...

    def locate(self, beat: float) -> None: ...

    def render_track(self, track_id: str, start_beat: float, end_beat: float) -> Any:
        """A track's signal before its fader, (frames, 2) float32 (engine request E1)."""

    def render_freeze(self, track_id: str) -> Freeze: ...

    def discard_freeze(self, freeze: Freeze) -> None: ...

    def program_name(self, track_id: str, device_id: str) -> str | None:
        """The plug-in's current program (engine request E5): None until it exists."""

    def plugin_state(self, track_id: str, device_id: str) -> bytes | None: ...

    def store_plugin_states(self, device_ids=None) -> None:
        """Plug-ins' states as they are now into the model (before copying devices or tracks)."""


class UiFacts(Protocol):
    def selection(self) -> SelectionFacts: ...

    def available_plugins(self) -> list[AvailablePlugin]: ...

    def places(self) -> list[str]:
        """The browser's places: folders files may be taken from."""
