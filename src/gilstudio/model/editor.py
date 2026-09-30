"""High-level, undoable edit operations used by the UI."""

from __future__ import annotations

import copy
import math
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path

from PySide6.QtGui import QUndoStack

from . import edits, notes
from .commands import (
    InsertTrackCommand,
    RemoveTrackCommand,
    SetClipsCommand,
    SetDeviceEnabledCommand,
    SetDeviceParamCommand,
    SetDevicesCommand,
    SetDeviceStateCommand,
    SetTempoCommand,
    UpdateSettingsCommand,
    UpdateTrackCommand,
)
from .project import (
    PLUGIN_KIND,
    AnyClip,
    Clip,
    Device,
    MidiClip,
    PluginRef,
    Project,
    Track,
    new_id,
)
from .timebase import TimeSignature

ClipRef = tuple[str, str]  # (track id, clip id)

BUILTIN_DEVICES = {
    # kind: (display name, {param id: default})
    "synth": ("Synth", {"wave": 2.0, "attack": 3.0, "decay": 300.0, "sustain": 70.0, "release": 200.0,
                        "cutoff": 4000.0, "resonance": 10.0, "volume": 0.0}),
    "utility": ("Utility", {"gain": 0.0, "pan": 0.0, "width": 100.0}),
}
# How the browser's Built-in category groups the devices above.
BUILTIN_CATEGORIES = {"Instruments": ["synth"], "Audio Effects": ["utility"]}
DEFAULT_INSTRUMENT = "synth"  # new MIDI tracks come with it, ready to play


def is_instrument(kind: str, plugin: PluginRef | None = None) -> bool:
    """Whether a device of this kind (and plug-in) is an instrument."""
    if kind == PLUGIN_KIND:
        return plugin is not None and plugin.instrument
    return kind in BUILTIN_CATEGORIES["Instruments"]


def device_is_instrument(device: Device) -> bool:
    return is_instrument(device.kind, device.plugin)


def device_name(device: Device) -> str:
    if device.plugin is not None:
        return device.plugin.name
    return BUILTIN_DEVICES.get(device.kind, (device.kind,))[0]


def new_device(kind: str, plugin: PluginRef | None = None) -> Device:
    if kind == PLUGIN_KIND:
        if plugin is None:
            raise ValueError("a plug-in device needs a plug-in")
        return Device(id=new_id(), kind=kind, plugin=plugin)
    return Device(id=new_id(), kind=kind, params=dict(BUILTIN_DEVICES[kind][1]))


class ProjectEditor:
    def __init__(self, project: Project, undo_stack: QUndoStack):
        self.project = project
        self.undo_stack = undo_stack
        # Called with (track id, device id) when the user adds a plug-in (not on undo or redo).
        self.plugin_added: Callable[[str, str], None] = lambda track_id, device_id: None

    def _push(self, command) -> None:
        self.undo_stack.push(command)

    # --- Tracks -----------------------------------------------------------------

    def add_audio_track(self, index: int | None = None, name: str | None = None) -> Track:
        p = self.project
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{len(p.tracks) + 1} Audio"),
                      color=p.next_color())
        self._push(InsertTrackCommand(p, track, len(p.tracks) if index is None else index, "Insert Audio Track"))
        return p.track(track.id)

    def add_midi_track(self, index: int | None = None, name: str | None = None,
                       instrument: str | None = DEFAULT_INSTRUMENT, plugin: PluginRef | None = None) -> Track:
        """A MIDI track with a built-in `instrument`, or with an instrument `plugin`."""
        p = self.project
        devices = [new_device(PLUGIN_KIND, plugin)] if plugin else [new_device(instrument)] if instrument else []
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{len(p.tracks) + 1} MIDI"),
                      color=p.next_color(), kind="midi", devices=devices)
        self._push(InsertTrackCommand(p, track, len(p.tracks) if index is None else index, "Insert MIDI Track"))
        if plugin:
            self.plugin_added(track.id, devices[0].id)
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

    def _commit(self, text: str, after: dict[str, list[AnyClip]], merge_key: object | None = None) -> None:
        before = {tid: list(self.project.track(tid).clips) for tid in after}
        if before != after:
            self._push(SetClipsCommand(self.project, text, before, after, merge_key))

    def add_clips(self, track_id: str | None, start_beat: float, sources: list[tuple[str, float]],
                  track_index: int | None = None) -> list[ClipRef]:
        """Place audio files one after another; `sources` is [(path, duration_sec)].
        With no track id (or a MIDI track's) a new audio track is created (at `track_index`)."""
        if not sources:
            return []
        self.undo_stack.beginMacro("Add Clip" if len(sources) == 1 else "Add Clips")
        try:
            if track_id is None or self.project.track(track_id).is_midi:
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

    def add_midi_clip(self, track_id: str, start_beat: float, length_beats: float) -> ClipRef | None:
        """An empty MIDI clip on a MIDI track, named after the track. It wins
        against clips it overlaps, like a placed clip."""
        track = self.project.track(track_id)
        if not track.is_midi or length_beats < edits.MIN_MIDI_CLIP_BEATS:
            return None
        clip = MidiClip(id=new_id(), name=track.name, start_beat=max(0.0, start_beat), duration_beats=length_beats)
        self._commit("Insert MIDI Clip", {track_id: edits.resolve_overlaps(list(track.clips) + [clip], {clip.id},
                                                                           self.project.tempo)})
        return track_id, clip.id

    def midi_clip_span(self, track_id: str, beat: float, grid_step: float = 0.0) -> tuple[float, float]:
        """Where a new MIDI clip made at `beat` goes: from the grid line at or before
        it (not reaching back over the clip before), one bar long or up to the next clip."""
        tempo = self.project.tempo
        clips = self.project.track(track_id).clips
        beat = max(0.0, beat)
        start = math.floor(beat / grid_step + 1e-9) * grid_step if grid_step > 0 else beat
        start = max([start] + [c.end_beat(tempo) for c in clips if c.end_beat(tempo) <= beat + edits.EPS])
        end = min([start + self.project.time_signature.beats_per_bar]
                  + [c.start_beat for c in clips if c.start_beat > start + edits.EPS])
        return start, end - start

    def set_clip_notes(self, ref: ClipRef, clip_notes, text: str, merge_key: object | None = None) -> None:
        """Replace a MIDI clip's notes (piano roll edits). With a `merge_key`, one
        gesture's edits are one undo step."""
        normalized = notes.normalize(clip_notes)
        self.update_clips([ref], lambda c: replace(c, notes=normalized), text, merge_key)

    def clamp_track_delta(self, refs: list[ClipRef], track_delta: int) -> int:
        """How far these clips can move across tracks: within the track list, and
        only onto tracks of their own kind (audio or MIDI). A move that would put
        any clip on the other kind of track keeps them on their tracks."""
        p = self.project
        indices = [p.track_index(tid) for tid, _ in refs]
        if not indices:
            return 0
        track_delta = max(-min(indices), min(track_delta, len(p.tracks) - 1 - max(indices)))
        if any(p.tracks[i + track_delta].kind != p.tracks[i].kind for i in indices):
            return 0
        return track_delta

    def move_clips(self, refs: list[ClipRef], delta_beats: float, track_delta: int = 0,
                   copy_clips: bool = False) -> list[ClipRef]:
        """Move (or copy) clips in time and across tracks. Returns the resulting refs."""
        p = self.project
        tempo = p.tempo
        moving = [(tid, p.clip(tid, cid)) for tid, cid in refs]
        if not moving:
            return []
        # Keep the whole group inside the timeline, the track list, and tracks of its kind.
        delta_beats = max(delta_beats, -min(c.start_beat for _, c in moving))
        indices = [p.track_index(tid) for tid, _ in moving]
        track_delta = self.clamp_track_delta(refs, track_delta)

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

    def replace_clip(self, track_id: str, clip: AnyClip, text: str) -> None:
        """Commit an edited version of one clip (e.g. after trimming)."""
        clips = [clip if c.id == clip.id else c for c in self.project.track(track_id).clips]
        self._commit(text, {track_id: edits.resolve_overlaps(clips, {clip.id}, self.project.tempo)})

    def update_clips(self, refs: list[ClipRef], change: Callable[[AnyClip], AnyClip], text: str,
                     merge_key: object | None = None) -> None:
        """Apply `change` to each clip in `refs` as one undo step (clip view settings).
        Positions must not change. Lengths in beats may (warping, segment BPM): a
        clip that would run into the next one is trimmed, as on a tempo change."""
        ids_by_track: dict[str, set[str]] = {}
        for tid, cid in refs:
            ids_by_track.setdefault(tid, set()).add(cid)
        current = {tid: list(self.project.track(tid).clips) for tid in ids_by_track}
        # Within one drag, work from the clips as they were when the drag began, so
        # dragging the segment BPM down and back up doesn't leave clips trimmed.
        baseline = current
        index = self.undo_stack.index()
        last = self.undo_stack.command(index - 1) if index > 0 else None
        if (merge_key is not None and isinstance(last, SetClipsCommand) and last.merge_key == merge_key
                and last.before.keys() == current.keys()):
            baseline = last.before
        tempo = self.project.tempo
        after = {tid: edits.fit_to_tempo([change(c) if c.id in ids_by_track[tid] else c for c in clips], tempo)
                 for tid, clips in baseline.items()}
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
        after: dict[str, list[AnyClip]] = {}
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
        after: dict[str, list[AnyClip]] = {}
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

    def add_device(self, track_id: str, kind: str, index: int | None = None,
                   plugin: PluginRef | None = None) -> Device | None:
        """Add a device (a built-in `kind`, or kind 'plugin' and a `plugin`) to a
        track's chain. An instrument only goes on a MIDI track (None otherwise),
        where it comes first and replaces any other instrument."""
        track = self.project.track(track_id)
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        device = new_device(kind, plugin)
        if device_is_instrument(device):
            if not track.is_midi:
                return None
            after = [d for d in after if not device_is_instrument(d)]
            after.insert(0, device)
        else:
            first = 1 if after and device_is_instrument(after[0]) else 0  # effects go after the instrument
            after.insert(len(after) if index is None else max(first, index), device)
        self._push(SetDevicesCommand(self.project, track_id, before, after, f"Add {device_name(device)}"))
        if device.is_plugin:
            self.plugin_added(track_id, device.id)
        return device

    def move_device(self, track_id: str, device_id: str, index: int) -> None:
        """Move a device within its chain (an instrument stays first)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        [moving] = [d for d in after if d.id == device_id]
        if device_is_instrument(moving):
            return
        after.remove(moving)
        first = 1 if after and device_is_instrument(after[0]) else 0
        after.insert(max(first, min(index, len(after))), moving)
        if [d.id for d in after] != [d.id for d in before]:
            self._push(SetDevicesCommand(self.project, track_id, before, after, "Move Device"))

    def move_devices(self, track_id: str, device_ids, index: int) -> None:
        """Move effects together (in their chain order) to before the device at
        `index` in the chain as it is now (the end if past it). One undo step; an
        instrument doesn't move, and nothing goes before it."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        ids = {d.id for d in before if d.id in set(device_ids) and not device_is_instrument(d)}
        if not ids:
            return
        moving = [d for d in copy.deepcopy(before) if d.id in ids]
        staying = [d for d in copy.deepcopy(before) if d.id not in ids]
        at = sum(1 for d in before[:max(0, index)] if d.id not in ids)
        first = 1 if staying and device_is_instrument(staying[0]) else 0
        at = max(first, min(at, len(staying)))
        after = staying[:at] + moving + staying[at:]
        if [d.id for d in after] != [d.id for d in before]:
            self._push(SetDevicesCommand(self.project, track_id, before, after,
                                         "Move Device" if len(moving) == 1 else "Move Devices"))

    def remove_device(self, track_id: str, device_id: str) -> None:
        self.remove_devices(track_id, [device_id])

    def remove_devices(self, track_id: str, device_ids) -> None:
        """Delete devices from a track's chain, in one undo step."""
        ids = set(device_ids)
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = [d for d in copy.deepcopy(before) if d.id not in ids]
        if len(after) != len(before):
            text = "Delete Device" if len(before) - len(after) == 1 else "Delete Devices"
            self._push(SetDevicesCommand(self.project, track_id, before, after, text))

    def set_device_param(self, track_id: str, device_id: str, param_id: str, value: float,
                         merge_key: object | None = None, old: float | None = None) -> None:
        """Change a parameter. `old` is its value before, if the model doesn't know
        it (a plug-in's parameters live in the plug-in)."""
        if old is None:
            old = self.project.device(track_id, device_id).params.get(param_id)
        if old is None or old != value:
            self._push(SetDeviceParamCommand(self.project, track_id, device_id, param_id,
                                             value if old is None else old, value, merge_key))

    def set_device_state(self, track_id: str, device_id: str, old: str | None, new: str,
                         text: str = "Load Preset") -> None:
        """Replace a plug-in's whole state (base64), e.g. with a preset. `old` is
        its state before, to go back to on undo."""
        self._push(SetDeviceStateCommand(self.project, track_id, device_id, old, new, text))

    def set_device_enabled(self, track_id: str, device_id: str, enabled: bool) -> None:
        if self.project.device(track_id, device_id).enabled != enabled:
            self._push(SetDeviceEnabledCommand(self.project, track_id, device_id, enabled))
