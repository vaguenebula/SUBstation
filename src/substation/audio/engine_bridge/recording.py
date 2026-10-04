"""Recording: the armed tracks' takes, written to WAV files in the recordings
folder (MIDI takes: their notes), drawn live while they record, and handed on
as RecordedTakes when the recording ends."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

import numpy as np
from PySide6.QtCore import QStandardPaths

from ... import _engine as ge
from ...model.editor import RecordedTake
from ...model.project import Project, Track


def recordings_folder(project: Project) -> Path:
    """Where takes go: the project's "Recordings" folder once it is saved,
    else SUBSTATION_RECORDINGS or the user's Music folder."""
    if project.path is not None:
        return Path(project.path).parent / "Recordings"
    if os.environ.get("SUBSTATION_RECORDINGS"):
        return Path(os.environ["SUBSTATION_RECORDINGS"])
    music = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.MusicLocation) or str(Path.home())
    return Path(music) / "SUBstation" / "Recordings"


def take_path(folder: Path, track_name: str, when: datetime) -> Path:
    """A new file for a take: the track's name and the time, numbered if taken."""
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", track_name).strip(" .") or "Audio"
    stem = f"{name} {when:%Y-%m-%d %H%M%S}"
    path, n = folder / f"{stem}.wav", 2
    while path.exists():
        path, n = folder / f"{stem} {n}.wav", n + 1
    return path


@dataclass
class LiveTake:
    """A take while it records, as the arrangement draws it: an audio take's
    peaks, or a MIDI take's notes."""

    track_id: str
    start_sample: int = 0  # timeline sample of its first frame
    started: bool = False
    frames: int = 0
    peaks: np.ndarray = field(default_factory=lambda: np.zeros((0, 2), np.float32))  # (min, max) per PEAK_FRAMES
    midi: bool = False
    # A MIDI take's notes so far: rows of (start, end, key, velocity, channel) in
    # timeline samples; a held note's end is -1.
    notes: np.ndarray = field(default_factory=lambda: np.zeros((0, 5), np.int64))
    PEAK_FRAMES = ge.RECORD_PEAK_FRAMES
    _buffer: np.ndarray = field(default_factory=lambda: np.zeros((1024, 2), np.float32), repr=False)

    def add_peaks(self, peaks: np.ndarray) -> None:
        """More peaks; `peaks` stays a view of the ones so far (the buffer grows by doubling)."""
        filled = len(self.peaks)
        if filled + len(peaks) > len(self._buffer):
            grown = np.zeros((max(2 * len(self._buffer), filled + len(peaks)), 2), np.float32)
            grown[:filled] = self._buffer[:filled]
            self._buffer = grown
        self._buffer[filled:filled + len(peaks)] = peaks
        self.peaks = self._buffer[:filled + len(peaks)]


class Recorder:
    """Recording takes. Part of EngineBridge (engine_bridge/__init__.py)."""

    @property
    def is_recording(self) -> bool:
        return bool(self._recording)

    @property
    def is_counting_in(self) -> bool:
        return self.engine.is_counting_in

    def record_targets(self) -> list[Track]:
        """The tracks that record: armed tracks with an input (audio, or MIDI for MIDI tracks)."""
        return [t for t in self.project.tracks if t.armed and t.has_input and not self.project.is_frozen(t.id)]

    def start_recording(self, count_in_beats: float = 0.0) -> str | None:
        """Records the armed tracks (playing, after the count-in, if stopped).
        Returns why it couldn't, or None."""
        if self._recording:
            return None
        tracks = self.record_targets()
        if not tracks:
            return "Arm a MIDI track, or an audio track that has an input, to record."
        if not self.engine.device_status.open:
            return "No audio device is open. Choose one in Options > Preferences."
        audio = [track for track in tracks if not track.is_midi]
        for track in audio:
            self._push_input(track.id)  # (a source the engine couldn't take before)
            if track.input:
                self._open_inputs(track.input)
        folder = recordings_folder(self.project)
        if audio:
            try:
                folder.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                return f"Could not create the recordings folder {folder}: {exc}"
        now = datetime.now().astimezone()  # local time, in the file names
        targets, paths = [], set()
        for track in tracks:
            if track.is_midi:
                targets.append((self._track_ids[track.id], ""))  # its notes, no file
                continue
            path = take_path(folder, track.name, now)
            while path in paths:  # two tracks of the same name
                path = path.with_name(path.stem + "_.wav")
            paths.add(path)
            targets.append((self._track_ids[track.id], str(path)))
        try:
            self.engine.start_recording(targets, count_in_beats)
        except (RuntimeError, ValueError) as exc:
            return str(exc)
        self._recording = {engine_id: track.id for (engine_id, _), track in zip(targets, tracks, strict=True)}
        self.live_takes = {track.id: LiveTake(track.id, midi=track.is_midi) for track in tracks}
        self.recording_changed.emit(True)
        self._poll_position()
        return None

    def stop_recording(self) -> list[RecordedTake]:
        """Ends the recording (playing goes on); its takes go out as `takes_recorded`."""
        if not self._recording:
            return []
        recording, self._recording = self._recording, {}
        self.live_takes = {}
        takes = []
        for take in self.engine.stop_recording():
            if take.error:
                self.status_message.emit(take.error)
            if take.dropped_frames:
                self.status_message.emit(f"The disk fell behind while recording: {take.dropped_frames} samples "
                                         "were lost (silence in the take).")
            track_id = recording.get(take.track_id)
            if track_id is None or take.frames <= 0:
                continue
            rate = take.sample_rate
            notes = tuple((int(start) / rate, int(end) / rate, int(key), int(velocity))
                          for start, end, key, velocity, _channel in take.notes)
            takes.append(RecordedTake(track_id, take.path, take.start_sample / rate, take.frames / rate,
                                      notes=notes, midi=take.midi))
        self.recording_changed.emit(False)
        self.recording_updated.emit()
        if takes:
            self.takes_recorded.emit(takes)
        return takes

    def _poll_recording(self) -> None:
        if not self._recording:
            return
        for progress in self.engine.recording_progress():
            track_id = self._recording.get(progress.track_id)
            live = self.live_takes.get(track_id) if track_id else None
            if live is None:
                continue
            live.started = progress.started
            live.start_sample = progress.start_sample
            live.frames = progress.frames
            if progress.midi:
                live.notes = progress.notes
                continue
            peaks = progress.peaks
            if len(peaks):
                live.add_peaks(peaks)
        self.recording_updated.emit()
        if not self.engine.is_recording:  # a locate, or a device change, ended it
            if not self.engine.device_status.open or not self.engine.is_playing:
                self.status_message.emit("Recording stopped.")
            self.stop_recording()
