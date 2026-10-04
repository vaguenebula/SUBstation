"""Freezing: rendering a track's signal before its fader into its frozen audio,
and a frozen track in the engine (its frozen audio as its only clip, its
devices' processors gone until it is unfrozen)."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from ... import _engine as ge
from ...model.automation import MASTER
from ...model.project import Freeze, Project, iter_devices
from .recording import recordings_folder, take_path
from .sources import _key

FREEZE_TAIL_SECONDS = 10.0  # a frozen track renders on past the arrangement's end this long, while it sounds


def freeze_folder(project: Project) -> Path:
    """Where frozen tracks' audio goes: the project's "Freeze" folder once it is
    saved, else a "Freeze" folder in the recordings folder (see recordings_folder)."""
    if project.path is not None:
        return Path(project.path).parent / "Freeze"
    return recordings_folder(project) / "Freeze"


@dataclass
class FreezeRender:
    """A track's render for freezing it, in the background (start_freeze)."""

    track_id: str
    job: ge.RenderJob  # its progress, cancel()
    tempo: float  # the project's when it started


class FreezeSync:
    """Frozen tracks, and rendering their audio. Part of EngineBridge (engine_bridge/__init__.py)."""

    def _push_frozen(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        frozen = self.project.track(track_id).frozen is not None
        if frozen != (track_id in self._frozen):
            self.engine.set_track_frozen(engine_id, frozen)
            if frozen:
                self._frozen.add(track_id)
            else:
                self._frozen.discard(track_id)

    def _on_freeze_changed(self, track_id: str) -> None:
        """Frozen: it plays its frozen audio, and its devices go (their states kept);
        unfrozen, they come back."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is None:
            return
        track = self.project.track(track_id)
        if track.frozen is not None:  # its plug-ins go: their states into the model, to be saved while it is frozen
            self.store_plugin_states({d.id for d in iter_devices(track.devices)})
        self._push_frozen(track_id)
        if track.frozen is None and track.is_midi:
            self.engine.set_track_clips(engine_id, [])  # (its frozen audio: it plays its notes again)
        self._push_clips(track_id)
        self._sync_devices(track_id)
        self._push_sidechains()

    def render_freeze(self, track_id: str) -> Freeze:
        """Renders a track's signal before its fader (after its devices; a group's
        bus) from the timeline's start to the arrangement's end, and its tail,
        into a new WAV file in the freeze folder: its frozen audio. Raises
        ValueError (with a message for the user) if there is nothing to render,
        OSError or RuntimeError if the file can't be written. (The UI renders in
        the background instead: start_freeze.)"""
        self.wait_for_device_states()  # plug-ins and samples still loading
        return self.finish_freeze(self.start_freeze(track_id))

    def start_freeze(self, track_id: str) -> FreezeRender:
        """render_freeze in the background: starts the render (the devices
        should be ready: devices_ready) and returns at once; finish_freeze makes
        it the track's frozen audio. Raises as render_freeze does."""
        track = self.project.track(track_id)
        engine_id = self._track_ids.get(track_id)
        end = self.project.end_beat()
        if engine_id is None or track_id == MASTER:
            raise ValueError(f"{track.name} can't be frozen")
        if end <= 0:
            raise ValueError("There is nothing to freeze yet: the arrangement is empty")
        folder = freeze_folder(self.project)
        folder.mkdir(parents=True, exist_ok=True)
        path = take_path(folder, f"{track.name} Freeze", datetime.now().astimezone())
        if self.is_playing:
            self.stop()
        job = self.engine.start_track_render(engine_id, str(path), 0.0, end, FREEZE_TAIL_SECONDS)
        return FreezeRender(track_id, job, self.project.tempo)

    def finish_freeze(self, render: FreezeRender) -> Freeze | None:
        """A freeze's render, ended: its frozen audio (decoded), or None if it was
        cancelled (its file is gone). Raises OSError or RuntimeError if it failed."""
        frames = render.job.finish()
        if frames is None:
            return None
        path = render.job.path
        # Decoded now, so that it plays as soon as the track is frozen (no gap while it loads).
        self._sources[_key(path)] = self.engine.load_source(path)
        self.source_ready.emit(path)
        return Freeze(path=path, duration_sec=frames / self.engine.sample_rate, tempo=render.tempo)

    def discard_freeze(self, freeze: Freeze) -> None:
        """A render no track plays (no undo step holds it): its decoded audio is
        forgotten and its file deleted."""
        self._sources.pop(_key(freeze.path), None)
        self.engine.release_unused_sources()
        # (which also lets go of others no track plays: those are decoded again when asked for)
        self._sources = {k: src for k, src in self._sources.items() if self.engine.cached_source(src.path) is not None}
        Path(freeze.path).unlink(missing_ok=True)
