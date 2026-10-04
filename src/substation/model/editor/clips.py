"""Editing clips: adding, moving, trimming, splitting and consolidating them, and
time selections (delete, duplicate, copy, cut, paste and move a range, with the
automation under it)."""

from __future__ import annotations

import math
from collections.abc import Callable
from dataclasses import dataclass, replace
from pathlib import Path

from .. import automation, edits, notes
from ..automation import Envelope
from ..commands import SetClipsCommand
from ..keys import clip_settings
from ..project import AnyClip, Clip, MidiClip, new_id
from .automation_edits import LaneRef

ClipRef = tuple[str, str]  # (track id, clip id)


@dataclass(frozen=True)
class CopiedTrack:
    """One track's part of copied clip content: its clips (starts from the copied
    range's start), and the automation under them (key: points from beat 0)."""

    track_id: str
    kind: str
    row: int  # tracks below the topmost copied one
    clips: tuple[AnyClip, ...]
    automation: tuple[tuple[str, Envelope], ...] = ()


@dataclass(frozen=True)
class ClipboardContent:
    """Clip content copied from a time selection (Ctrl+C / Ctrl+X), `length` beats long."""

    length: float
    tracks: tuple[CopiedTrack, ...]


class ClipEdits:
    """Clips and time selections. Part of ProjectEditor (editor/__init__.py)."""

    def _commit(self, text: str, after: dict[str, list[AnyClip]], merge_key: object | None = None) -> None:
        before = {tid: list(self.project.track(tid).clips) for tid in after}
        if before != after:
            self._push(SetClipsCommand(self.project, text, before, after, merge_key))

    def _commit_moved(self, text: str, after: dict[str, list[AnyClip]], envelopes: dict[LaneRef, Envelope]) -> None:
        """Clips that moved, and the automation that moved with them, as one undo step."""
        if not envelopes:
            self._commit(text, after)
            return
        self.undo_stack.beginMacro(text)
        try:
            self._commit(text, after)
            for (owner, key), points in envelopes.items():
                self.set_envelope(owner, key, points, text)
        finally:
            self.undo_stack.endMacro()

    def _carried_automation(self, spans, delta_beats: float, copy_clips: bool) -> dict[LaneRef, Envelope]:
        """The envelopes after the automation under moving (or copied) clips went
        with them, unless automation is locked. `spans` are (source track,
        destination track, start, end) of the clips. Only envelopes with
        breakpoints under the clips move. Across tracks, only the mixer's
        automation goes along (a device's belongs to its track); a device's
        stays where it is."""
        if self.project.automation_locked:
            return {}
        by_move: dict[tuple[str, str], list[tuple[float, float]]] = {}
        for source, dest, start, end in spans:
            if end > start:
                by_move.setdefault((source, dest), []).append((start, end))
        changed: dict[LaneRef, Envelope] = {}
        edges: dict[LaneRef, set[float]] = {}

        def current(owner: str, key: str) -> Envelope:
            return changed.get((owner, key), self.project.envelope(owner, key))

        pastes = []
        for (source, dest), ranges in by_move.items():
            if source == dest and delta_beats == 0 and not copy_clips:
                continue
            for start, end in automation.merge_spans(ranges):
                for key, points in self.project.automation(source).items():
                    if (dest != source and not automation.is_mixer_key(key)
                            or not automation.has_points_in(points, start, end)):
                        continue
                    pastes.append((dest, key, start + delta_beats, end - start,
                                   automation.copy_range(points, start, end)))
                    if not copy_clips:
                        changed[(source, key)] = automation.remove_range(current(source, key), start, end)
                        edges.setdefault((source, key), set()).update((start, end))
        for dest, key, at, length, content in pastes:
            changed[(dest, key)] = automation.paste_range(current(dest, key), content, at, length)
            edges.setdefault((dest, key), set()).update((at, at + length))
        changed = {lane: automation.drop_redundant(points, edges[lane]) for lane, points in changed.items()}
        return {lane: points for lane, points in changed.items() if points != self.project.envelope(*lane)}

    def add_clips(self, track_id: str | None, start_beat: float, sources: list[tuple[str, float]],
                  track_index: int | None = None) -> list[ClipRef]:
        """Place audio files one after another; `sources` is [(path, duration_sec)].
        With no track id (or one of a MIDI or group track) a new audio track is created (at `track_index`).
        A tempo or key in a file's name sets up its clip (see keys.clip_settings):
        loops and long files are warped, and audio is transposed to the project's key."""
        if not sources:
            return []
        if track_id is not None and self.project.is_frozen(track_id) and self.project.track(track_id).is_audio:
            self.refused.emit(f"{self.project.track(self.project.frozen_by(track_id)).name} is frozen: "
                              "unfreeze it to change its clips")
            return []
        self.undo_stack.beginMacro("Add Clip" if len(sources) == 1 else "Add Clips")
        try:
            if track_id is None or not self.project.track(track_id).is_audio:
                track_id = self.add_audio_track(index=track_index, name=Path(sources[0][0]).stem).id
            tempo = self.project.tempo
            clips = list(self.project.track(track_id).clips)
            new_ids = set()
            position = max(0.0, start_beat)
            for path, duration in sources:
                clip = Clip(id=new_id(), path=path, name=Path(path).stem, start_beat=position,
                            duration_sec=duration, source_duration_sec=duration,
                            **clip_settings(Path(path).name, duration, tempo, self.project.key))
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
        return (track_id, clip.id) if any(c.id == clip.id for c in track.clips) else None  # (not on a frozen track)

    def add_midi_clips_over(self, start_beat: float, end_beat: float, track_ids) -> list[ClipRef]:
        """An empty MIDI clip over a time range on each MIDI track of `track_ids`."""
        refs = [self.add_midi_clip(tid, start_beat, end_beat - start_beat) for tid in track_ids
                if self.project.has_track(tid) and self.project.track(tid).is_midi]
        return [ref for ref in refs if ref is not None]

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
        spans = [(tid, p.tracks[index + track_delta].id, clip.start_beat, clip.end_beat(tempo))
                 for (tid, clip), index in zip(moving, indices, strict=True)]
        self._commit_moved("Copy Clips" if copy_clips else "Move Clips", after,
                           self._carried_automation(spans, delta_beats, copy_clips))
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
            spans = [(tid, tid, start, end) for tid in after]
            self._commit_moved("Duplicate Time Selection", after, self._carried_automation(spans, length, True))
        return result

    def copy_range(self, start: float, end: float, track_ids) -> ClipboardContent | None:
        """Ctrl+C on a time selection: just the clip content between two beats on these
        tracks (clips across its edges are cut there), with the automation under it
        unless automation is locked. None if there is no clip content there."""
        p = self.project
        tempo = p.tempo
        ids = sorted((t for t in set(track_ids) if p.has_track(t)), key=p.track_index)
        if end <= start:
            return None
        copied = []
        for tid in ids:
            track = p.track(tid)
            clips = tuple(replace(c, start_beat=c.start_beat - start)
                          for c in edits.slice_range(track.clips, start, end, tempo))
            if not clips:
                continue
            lanes = () if p.automation_locked else tuple(
                (key, automation.copy_range(points, start, end)) for key, points in p.automation(tid).items()
                if automation.has_points_in(points, start, end))
            copied.append((p.track_index(tid), CopiedTrack(tid, track.kind, 0, clips, lanes)))
        if not copied:
            return None
        top = copied[0][0]  # rows count from the topmost track with content
        return ClipboardContent(end - start, tuple(replace(c, row=index - top) for index, c in copied))

    def cut_range(self, start: float, end: float, track_ids) -> ClipboardContent | None:
        """Ctrl+X on a time selection: copy it (see copy_range), then take the clip
        content, and the automation copied with it, out. One undo step."""
        content = self.copy_range(start, end, track_ids)
        if content is None:
            return None
        tempo = self.project.tempo
        after = {c.track_id: edits.remove_range(self.project.track(c.track_id).clips, start, end, tempo)
                 for c in content.tracks}
        envelopes = {}
        for c in content.tracks:
            for key, _points in c.automation:
                points = automation.remove_range(self.project.envelope(c.track_id, key), start, end)
                envelopes[(c.track_id, key)] = automation.drop_redundant(points, (start, end))
        self._commit_moved("Cut", after, {lane: points for lane, points in envelopes.items()
                                          if points != self.project.envelope(*lane)})
        return content

    def paste_targets(self, content: ClipboardContent, track_id: str | None) -> list[str] | None:
        """Where pasted content goes: its top track onto `track_id` and the rest onto
        the tracks below, as they were copied. If they aren't all tracks of the
        content's kind (audio, MIDI), the tracks it was copied from; None if those are gone."""
        p = self.project
        if track_id is not None and p.has_track(track_id):
            rows = [p.track_index(track_id) + c.row for c in content.tracks]
            if all(r < len(p.tracks) and p.tracks[r].kind == c.kind for r, c in zip(rows, content.tracks, strict=True)):
                return [p.tracks[r].id for r in rows]
        if all(p.has_track(c.track_id) and p.track(c.track_id).kind == c.kind for c in content.tracks):
            return [c.track_id for c in content.tracks]
        return None

    def paste(self, content: ClipboardContent, at_beat: float,
              track_id: str | None = None) -> tuple[float, float, list[str]] | None:
        """Ctrl+V: copied clip content at `at_beat` (see paste_targets for which
        tracks), replacing what is there, as new clips. Its automation comes along
        unless automation is locked; onto another track, only the mixer's (a
        device's belongs to its track). One undo step. Returns the area pasted
        over (start, end, the tracks from the top one to the lowest); None if the
        content has nowhere to go."""
        p = self.project
        dests = self.paste_targets(content, track_id)
        if dests is None:
            return None
        tempo = p.tempo
        at = max(0.0, at_beat)
        lists: dict[str, list[AnyClip]] = {}
        winners: dict[str, set[str]] = {}
        changed: dict[LaneRef, Envelope] = {}
        for copied, dest in zip(content.tracks, dests, strict=True):
            pasted = [replace(c, id=new_id(), start_beat=c.start_beat + at) for c in copied.clips]
            lists.setdefault(dest, list(p.track(dest).clips)).extend(pasted)
            winners.setdefault(dest, set()).update(c.id for c in pasted)
            if p.automation_locked:
                continue
            for key, points in copied.automation:
                if dest == copied.track_id or key in automation.MIXER_KEYS:
                    lane = (dest, key)
                    changed[lane] = automation.paste_range(changed.get(lane, p.envelope(*lane)), points, at,
                                                           content.length)
        after = {tid: edits.resolve_overlaps(clips, winners[tid], tempo) for tid, clips in lists.items()}
        edges = (at, at + content.length)
        envelopes = {lane: automation.drop_redundant(points, edges) for lane, points in changed.items()}
        self._commit_moved("Paste", after, {lane: points for lane, points in envelopes.items()
                                            if points != p.envelope(*lane)})
        rows = sorted(p.track_index(d) for d in dests)
        return at, at + content.length, [t.id for t in p.tracks[rows[0]:rows[-1] + 1]]

    def move_range(self, start: float, end: float, track_ids: list[str], delta_beats: float,
                   track_delta: int = 0, copy_clips: bool = False) -> tuple[float, list[str]]:
        """Ableton's drag of a time selection: move (or copy) just the clip content
        between two beats, in time and across tracks. Clips across the range's edges
        are split there; the moved content replaces what it lands on. Returns where
        the range ended up: its new start and tracks."""
        p = self.project
        tempo = p.tempo
        delta_beats = max(delta_beats, -start)
        track_delta = self.clamp_track_delta([(tid, "") for tid in track_ids], track_delta)
        lists = {t.id: list(t.clips) for t in p.tracks}
        affected: set[str] = set()
        winners: dict[str, set[str]] = {}
        dest_ids = []
        # Moved clips that were wholly inside the range stay the same clips.
        pieces = {tid: edits.slice_range(lists[tid], start, end, tempo, keep_ids=not copy_clips) for tid in track_ids}
        for tid in track_ids:
            dest = p.tracks[p.track_index(tid) + track_delta].id
            dest_ids.append(dest)
            if not copy_clips and pieces[tid]:
                lists[tid] = edits.remove_range(lists[tid], start, end, tempo)
                affected.add(tid)
        for tid, dest in zip(track_ids, dest_ids, strict=True):
            moved = [replace(c, start_beat=c.start_beat + delta_beats) for c in pieces[tid]]
            if moved:
                lists[dest] += moved
                winners.setdefault(dest, set()).update(c.id for c in moved)
                affected.add(dest)
        after = {tid: edits.resolve_overlaps(lists[tid], winners.get(tid, set()), tempo) for tid in affected}
        if after:
            spans = [(tid, dest, start, end) for tid, dest in zip(track_ids, dest_ids, strict=True) if pieces[tid]]
            self._commit_moved("Copy Time Selection" if copy_clips else "Move Time Selection", after,
                               self._carried_automation(spans, delta_beats, copy_clips))
        return start + delta_beats, dest_ids

    def clips_area(self, refs) -> tuple[float, float, list[str]] | None:
        """The grid area that fully contains these clips: the earliest start to the
        latest end, on every track from the topmost clip's to the lowest one's.
        None if none of them exist."""
        p = self.project
        by_ref = {(t.id, c.id): (i, c) for i, t in enumerate(p.tracks) for c in t.clips}
        found = [by_ref[ref] for ref in refs if ref in by_ref]
        if not found:
            return None
        rows = [i for i, _ in found]
        start = min(c.start_beat for _, c in found)
        end = max(c.end_beat(p.tempo) for _, c in found)
        return start, end, [t.id for t in p.tracks[min(rows):max(rows) + 1]]

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

    def consolidatable(self, refs) -> dict[str, list[MidiClip]]:
        """The MIDI clips among `refs` that Consolidate joins: on each track with two or more."""
        by_track: dict[str, list[MidiClip]] = {}
        for tid, cid in refs:
            clip = self.project.clip(tid, cid)
            if isinstance(clip, MidiClip):
                by_track.setdefault(tid, []).append(clip)
        return {tid: clips for tid, clips in by_track.items() if len(clips) > 1}

    def consolidate_clips(self, refs) -> list[ClipRef]:
        """Join the selected MIDI clips on each track into one clip spanning them
        (Ctrl+J), as one undo step. The new clips; [] if there was nothing to join."""
        tempo = self.project.tempo
        after: dict[str, list[AnyClip]] = {}
        joined: list[ClipRef] = []
        for tid, clips in self.consolidatable(refs).items():
            clip = edits.consolidate_midi(clips)
            ids = {c.id for c in clips}
            kept = [c for c in self.project.track(tid).clips if c.id not in ids]
            after[tid] = edits.resolve_overlaps(kept + [clip], {clip.id}, tempo)
            joined.append((tid, clip.id))
        if after:
            self._commit("Consolidate", after)
        return joined

    def clips_at(self, track_ids: list[str], beat: float) -> list[ClipRef]:
        tempo = self.project.tempo
        return [(tid, c.id) for tid in track_ids for c in self.project.track(tid).clips
                if c.start_beat < beat < c.end_beat(tempo)]
