"""High-level, undoable edit operations used by the UI."""

from __future__ import annotations

import copy
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path

from PySide6.QtGui import QUndoStack

from . import edits
from .commands import (
    InsertTrackCommand,
    RemoveTrackCommand,
    SetClipsCommand,
    SetDeviceEnabledCommand,
    SetDeviceParamCommand,
    SetDevicesCommand,
    SetTempoCommand,
    UpdateSettingsCommand,
    UpdateTrackCommand,
)
from .project import Clip, Device, Project, Track, new_id
from .timebase import TimeSignature

ClipRef = tuple[str, str]  # (track id, clip id)

BUILTIN_DEVICES = {
    # kind: (display name, {param id: default})
    "utility": ("Utility", {"gain": 0.0, "pan": 0.0, "width": 100.0}),
}
# How the browser's Built-in category groups the devices above.
BUILTIN_CATEGORIES = {"Audio Effects": ["utility"]}


class ProjectEditor:
    def __init__(self, project: Project, undo_stack: QUndoStack):
        self.project = project
        self.undo_stack = undo_stack

    def _push(self, command) -> None:
        self.undo_stack.push(command)

    # --- Tracks -----------------------------------------------------------------

    def add_audio_track(self, index: int | None = None, name: str | None = None) -> Track:
        p = self.project
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{len(p.tracks) + 1} Audio"),
                      color=p.next_color())
        self._push(InsertTrackCommand(p, track, len(p.tracks) if index is None else index, "Insert Audio Track"))
        return p.track(track.id)

    def delete_tracks(self, track_ids: list[str]) -> None:
        if not track_ids:
            return
        self.undo_stack.beginMacro("Delete Track" if len(track_ids) == 1 else "Delete Tracks")
        for track_id in track_ids:
            self._push(RemoveTrackCommand(self.project, track_id))
        self.undo_stack.endMacro()

    def rename_track(self, track_id: str, name: str) -> None:
        old = self.project.track(track_id).name
        if name and name != old:
            self._push(UpdateTrackCommand(self.project, track_id, "name", old, name, "Rename Track"))

    def set_track_color(self, track_id: str, color: str) -> None:
        old = self.project.track(track_id).color
        if color != old:
            self._push(UpdateTrackCommand(self.project, track_id, "color", old, color, "Change Track Color"))

    def set_track_param(self, track_id: str, attr: str, value, merge_key: object | None = None) -> None:
        """Mixer settings: volume_db, pan, mute, solo."""
        labels = {"volume_db": "Change Volume", "pan": "Change Pan", "mute": "Toggle Track Activator",
                  "solo": "Toggle Solo"}
        old = getattr(self.project.track(track_id), attr)
        if value != old:
            self._push(UpdateTrackCommand(self.project, track_id, attr, old, value, labels[attr], merge_key))

    def set_track_height(self, track_id: str, height: int) -> None:
        # View state: saved with the project but not worth an undo step.
        self.project.update_track(track_id, height=height)

    # --- Project settings --------------------------------------------------------

    def _set_settings(self, text: str, merge_key: object | None = None, **new) -> None:
        old = {name: getattr(self.project, name) for name in new}
        if old != new:
            self._push(UpdateSettingsCommand(self.project, old, new, text, merge_key))

    def set_tempo(self, bpm: float, merge_key: object | None = None) -> None:
        """Change the tempo, trimming clips that would otherwise overlap."""
        p = self.project
        tempo = max(20.0, min(999.0, round(bpm, 2)))
        if tempo == p.tempo:
            return
        current = {t.id: list(t.clips) for t in p.tracks}
        # Within one drag, fit from the clips as they were when the drag began, so
        # going up and back down doesn't leave clips trimmed.
        baseline = current
        index = self.undo_stack.index()
        last = self.undo_stack.command(index - 1) if index > 0 else None
        if (merge_key is not None and isinstance(last, SetTempoCommand) and last.merge_key == merge_key
                and last.old[1].keys() == current.keys()):
            baseline = last.old[1]
        fitted = {tid: edits.fit_to_tempo(clips, tempo) for tid, clips in baseline.items()}
        self._push(SetTempoCommand(p, (p.tempo, current), (tempo, fitted), merge_key))

    def set_time_signature(self, ts: TimeSignature) -> None:
        self._set_settings("Change Time Signature", time_signature=ts)

    def set_loop(self, enabled: bool, start: float, end: float, merge_key: object | None = None) -> None:
        start = max(0.0, start)
        end = max(start + 0.25, end)
        self._set_settings("Change Loop", merge_key, loop_enabled=enabled, loop_start=start, loop_end=end)

    def set_loop_enabled(self, enabled: bool) -> None:
        self._set_settings("Toggle Loop", loop_enabled=enabled)

    def set_master_volume(self, db: float, merge_key: object | None = None) -> None:
        self._set_settings("Change Master Volume", merge_key, master_volume_db=db)

    # --- Clips -------------------------------------------------------------------

    def _commit(self, text: str, after: dict[str, list[Clip]], merge_key: object | None = None) -> None:
        before = {tid: list(self.project.track(tid).clips) for tid in after}
        if before != after:
            self._push(SetClipsCommand(self.project, text, before, after, merge_key))

    def add_clips(self, track_id: str | None, start_beat: float, sources: list[tuple[str, float]],
                  track_index: int | None = None) -> list[ClipRef]:
        """Place audio files one after another; `sources` is [(path, duration_sec)].
        With no track id a new audio track is created (at `track_index`)."""
        if not sources:
            return []
        self.undo_stack.beginMacro("Add Clip" if len(sources) == 1 else "Add Clips")
        try:
            if track_id is None:
                track_id = self.add_audio_track(index=track_index, name=Path(sources[0][0]).stem).id
            tempo = self.project.tempo
            clips = list(self.project.track(track_id).clips)
            new_ids = set()
            position = max(0.0, start_beat)
            for path, duration in sources:
                clip = Clip(id=new_id(), path=path, name=Path(path).stem, start_beat=position,
                            duration_sec=duration, source_duration_sec=duration)
                clips.append(clip)
                new_ids.add(clip.id)
                position = clip.end_beat(tempo)
            self._commit("Add Clip", {track_id: edits.resolve_overlaps(clips, new_ids, tempo)})
        finally:
            self.undo_stack.endMacro()
        return [(track_id, cid) for cid in new_ids]

    def move_clips(self, refs: list[ClipRef], delta_beats: float, track_delta: int = 0,
                   copy_clips: bool = False) -> list[ClipRef]:
        """Move (or copy) clips in time and across tracks. Returns the resulting refs."""
        p = self.project
        tempo = p.tempo
        moving = [(tid, p.clip(tid, cid)) for tid, cid in refs]
        if not moving:
            return []
        # Keep the whole group inside the timeline and the track list.
        delta_beats = max(delta_beats, -min(c.start_beat for _, c in moving))
        indices = [p.track_index(tid) for tid, _ in moving]
        track_delta = max(-min(indices), min(track_delta, len(p.tracks) - 1 - max(indices)))

        lists = {t.id: list(t.clips) for t in p.tracks}
        affected: set[str] = set()
        winners: dict[str, set[str]] = {}
        result: list[ClipRef] = []
        moving_ids = {c.id for _, c in moving}
        if not copy_clips:
            for tid, _ in moving:
                lists[tid] = [c for c in lists[tid] if c.id not in moving_ids]
                affected.add(tid)
        for (_tid, clip), index in zip(moving, indices, strict=True):
            dest = p.tracks[index + track_delta].id
            moved = replace(clip, start_beat=clip.start_beat + delta_beats,
                            id=new_id() if copy_clips else clip.id)
            lists[dest].append(moved)
            winners.setdefault(dest, set()).add(moved.id)
            affected.add(dest)
            result.append((dest, moved.id))
        after = {tid: edits.resolve_overlaps(lists[tid], winners.get(tid, set()), tempo) for tid in affected}
        self._commit("Copy Clips" if copy_clips else "Move Clips", after)
        return result

    def replace_clip(self, track_id: str, clip: Clip, text: str) -> None:
        """Commit an edited version of one clip (e.g. after trimming)."""
        clips = [clip if c.id == clip.id else c for c in self.project.track(track_id).clips]
        self._commit(text, {track_id: edits.resolve_overlaps(clips, {clip.id}, self.project.tempo)})

    def update_clips(self, refs: list[ClipRef], change: Callable[[Clip], Clip], text: str,
                     merge_key: object | None = None) -> None:
        """Apply `change` to each clip in `refs` as one undo step (clip view settings).
        Positions and lengths must not change, so overlaps need no resolving."""
        ids_by_track: dict[str, set[str]] = {}
        for tid, cid in refs:
            ids_by_track.setdefault(tid, set()).add(cid)
        after = {tid: [change(c) if c.id in ids else c for c in self.project.track(tid).clips]
                 for tid, ids in ids_by_track.items()}
        self._commit(text, after, merge_key)

    def delete_clips(self, refs: list[ClipRef]) -> None:
        ids_by_track: dict[str, set[str]] = {}
        for tid, cid in refs:
            ids_by_track.setdefault(tid, set()).add(cid)
        after = {tid: [c for c in self.project.track(tid).clips if c.id not in ids]
                 for tid, ids in ids_by_track.items()}
        self._commit("Delete Clips" if len(refs) > 1 else "Delete Clip", after)

    def delete_range(self, start: float, end: float, track_ids: list[str]) -> None:
        """Delete the clip content between two beats on the given tracks."""
        tempo = self.project.tempo
        self._commit("Delete Time Selection", {
            tid: edits.remove_range(self.project.track(tid).clips, start, end, tempo) for tid in track_ids})

    def duplicate_range(self, start: float, end: float, track_ids: list[str]) -> list[ClipRef]:
        """Ableton's Ctrl+D on a time selection: copy just the clip content between
        two beats to right after `end`, replacing what was there. Returns the copies."""
        tempo = self.project.tempo
        length = end - start
        after: dict[str, list[Clip]] = {}
        result: list[ClipRef] = []
        for tid in track_ids:
            clips = self.project.track(tid).clips
            copies = [replace(c, start_beat=c.start_beat + length) for c in edits.slice_range(clips, start, end, tempo)]
            if copies:
                ids = {c.id for c in copies}
                after[tid] = edits.resolve_overlaps(list(clips) + copies, ids, tempo)
                result += [(tid, cid) for cid in ids]
        if after:
            self._commit("Duplicate Time Selection", after)
        return result

    def clips_in_range(self, start: float, end: float, track_ids) -> set[ClipRef]:
        """The clips on these tracks that overlap the beat range."""
        tempo = self.project.tempo
        return {(tid, c.id) for tid in track_ids for c in self.project.track(tid).clips
                if c.start_beat < end and c.end_beat(tempo) > start}

    def duplicate_clips(self, refs: list[ClipRef]) -> list[ClipRef]:
        """Ableton's Ctrl+D: copies land right after the selection."""
        p = self.project
        clips = [p.clip(tid, cid) for tid, cid in refs]
        if not clips:
            return []
        start, end = edits.selection_span(clips, p.tempo)
        return self.move_clips(refs, end - start, 0, copy_clips=True)

    def split_clips(self, refs: list[ClipRef], at_beat: float) -> None:
        tempo = self.project.tempo
        after: dict[str, list[Clip]] = {}
        for tid, cid in refs:
            clips = after.get(tid) or list(self.project.track(tid).clips)
            for i, clip in enumerate(clips):
                if clip.id == cid:
                    parts = edits.split_clip(clip, at_beat, tempo)
                    if parts:
                        clips[i:i + 1] = list(parts)
                        after[tid] = clips
                    break
        if after:
            self._commit("Split", after)

    def clips_at(self, track_ids: list[str], beat: float) -> list[ClipRef]:
        tempo = self.project.tempo
        return [(tid, c.id) for tid in track_ids for c in self.project.track(tid).clips
                if c.start_beat < beat < c.end_beat(tempo)]

    # --- Devices -----------------------------------------------------------------

    def add_device(self, track_id: str, kind: str, index: int | None = None) -> Device:
        name, defaults = BUILTIN_DEVICES[kind]
        before = copy.deepcopy(self.project.track(track_id).devices)
        device = Device(id=new_id(), kind=kind, params=dict(defaults))
        after = copy.deepcopy(before)
        after.insert(len(after) if index is None else index, device)
        self._push(SetDevicesCommand(self.project, track_id, before, after, f"Add {name}"))
        return device

    def remove_device(self, track_id: str, device_id: str) -> None:
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = [d for d in copy.deepcopy(before) if d.id != device_id]
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Delete Device"))

    def set_device_param(self, track_id: str, device_id: str, param_id: str, value: float,
                         merge_key: object | None = None) -> None:
        old = self.project.device(track_id, device_id).params.get(param_id)
        if old != value:
            self._push(SetDeviceParamCommand(self.project, track_id, device_id, param_id, old, value, merge_key))

    def set_device_enabled(self, track_id: str, device_id: str, enabled: bool) -> None:
        if self.project.device(track_id, device_id).enabled != enabled:
            self._push(SetDeviceEnabledCommand(self.project, track_id, device_id, enabled))
