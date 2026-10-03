"""Editing tracks: adding, deleting and duplicating them, returns and sends,
groups, a track's mixer, inputs and monitoring, and recorded takes."""

from __future__ import annotations

import copy
from dataclasses import dataclass, replace
from pathlib import Path

from .. import automation, edits, notes
from ..automation import MASTER, MAX_VOLUME_DB, MIN_VOLUME_DB, MIXER_PAN, MIXER_VOLUME
from ..commands import (
    ArrangeTracksCommand,
    InsertReturnCommand,
    InsertTrackCommand,
    RemoveReturnCommand,
    RemoveTrackCommand,
    SetDeviceSidechainCommand,
    SetEnvelopeCommand,
    UpdateTrackCommand,
    UpdateTrackFieldsCommand,
    UpdateTracksCommand,
)
from ..devices import DEFAULT_INSTRUMENT
from ..project import (
    GROUP_KIND,
    MONITOR_MODES,
    PLUGIN_KIND,
    RETURN_KIND,
    AnyClip,
    Clip,
    Device,
    MidiClip,
    MidiInput,
    Note,
    PluginRef,
    Send,
    Track,
    feeds,
    iter_chains,
    iter_devices,
    new_id,
    refresh_ids,
    return_letter,
    routing_graph,
    tree_problem,
)
from ..timebase import seconds_to_beats
from .clips import ClipRef

AT_INDEX = object()  # a new track's group: the one where it is inserted (Project.parent_at)


@dataclass(frozen=True)
class RecordedTake:
    """A recorded WAV file and where it starts: `start_sec` from the timeline's
    start (negative: it began before it). A MIDI take has no file but `notes`:
    (start, end, pitch, velocity), in seconds on the timeline, within the take."""

    track_id: str
    path: str
    start_sec: float
    duration_sec: float
    notes: tuple[tuple[float, float, int, int], ...] = ()
    midi: bool = False


def _renamed_key(key: str, ids: dict[str, str]) -> str:
    """An automation key with the devices (and rack chains) it names renamed by `ids`."""
    try:
        parts = automation.parse_key(key)
    except ValueError:
        return key
    if parts[0] != "device":
        return key
    _kind, device_id, param_id = parts
    chain = automation.key_chain_control(key)
    if chain is not None:
        return automation.chain_key(ids.get(device_id, device_id), ids.get(chain[0], chain[0]), chain[1])
    return automation.device_key(ids.get(device_id, device_id), param_id)


class TrackEdits:
    """Tracks, returns and sends, groups, inputs and recordings. Part of ProjectEditor (editor/__init__.py)."""

    def _insert_track(self, track: Track, index: int | None, parent, text: str) -> Track:
        """Insert a new track at `index` (None: last) in group `parent` (AT_INDEX: the
        group at that place). A group it can't be in there gives way to that one."""
        p = self.project
        index = len(p.tracks) if index is None else max(0, min(index, len(p.tracks)))
        track.parent = p.parent_at(index) if parent is AT_INDEX else parent
        if tree_problem(p.tracks[:index] + [track] + p.tracks[index:]) is not None:
            track.parent = p.parent_at(index)
        self._push(InsertTrackCommand(p, track, index, text))
        return p.track(track.id)

    def insertion_point(self, track_id: str | None) -> tuple[int | None, str | None]:
        """Where a track inserted "after" this one goes: after it and what is in it,
        in its group. (None, None) without a track: last, in no group."""
        if track_id is None or not self.project.has_track(track_id):
            return None, None
        index = self.project.track_index(track_id)
        return self.project.subtree_end(index), self.project.track(track_id).parent

    def add_audio_track(self, index: int | None = None, name: str | None = None, parent=AT_INDEX) -> Track:
        p = self.project
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{len(p.tracks) + 1} Audio"),
                      color=p.next_color())
        return self._insert_track(track, index, parent, "Insert Audio Track")

    def add_midi_track(self, index: int | None = None, name: str | None = None,
                       instrument: str | None = DEFAULT_INSTRUMENT, plugin: PluginRef | None = None,
                       parent=AT_INDEX) -> Track:
        """A MIDI track with a built-in `instrument`, or with an instrument `plugin`
        (as its default preset has it, if there is one)."""
        p = self.project
        devices = [self._new_device(PLUGIN_KIND, plugin)] if plugin else [self._new_device(instrument)] if instrument \
            else []
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{len(p.tracks) + 1} MIDI"),
                      color=p.next_color(), kind="midi", devices=devices)
        track = self._insert_track(track, index, parent, "Insert MIDI Track")
        if plugin:
            self.plugin_added.emit(track.id, devices[0].id)
        return track

    def add_midi_track_with(self, device: Device, index: int | None = None, parent=AT_INDEX,
                            text: str = "Insert MIDI Track") -> Track:
        """A MIDI track with an instrument device on it (an instrument preset: a
        plug-in, a built-in one or an instrument rack). One undo step."""
        self.undo_stack.beginMacro(text)
        try:
            track = self.add_midi_track(index, instrument=None, parent=parent)
            self.insert_device(track.id, device, text=text, show_editors=not device.is_rack)
        finally:
            self.undo_stack.endMacro()
        return track

    def delete_tracks(self, track_ids: list[str]) -> None:
        """Delete tracks (and return tracks); a group goes with what is in it, a
        return with the sends into it (and their automation). The inputs and
        sidechains they were the source of go too. One undo step."""
        p = self.project
        doomed = {t for t in track_ids if p.has_track(t)}
        doomed |= {d.id for t in list(doomed) for d in p.descendants(t)}
        returns = {t for t in track_ids if p.has_return(t)}
        if not doomed and not returns:
            return
        count = len(doomed) + len(returns)
        text = "Delete Return Track" if not doomed and count == 1 else "Delete Track" if count == 1 else "Delete Tracks"
        self.undo_stack.beginMacro(text)
        self._drop_inputs(doomed | returns, text)  # (first: undo brings them back after their sources)
        self._drop_sidechains(doomed | returns, text)
        # The last first: undo brings back each group before what is in it.
        for track in reversed(p.tracks):
            if track.id in doomed:
                self._push(RemoveTrackCommand(p, track.id, text))
        if returns:
            for track in p.senders():
                kept = {r: send for r, send in track.sends.items() if r not in returns}
                if track.id not in returns and kept != track.sends:
                    self._push(UpdateTrackCommand(p, track.id, "sends", track.sends, kept, text))
                for key in [k for k in track.automation if automation.key_send(k) in returns]:
                    self._push(SetEnvelopeCommand(p, track.id, key, p.envelope(track.id, key), (), text))
            for track in reversed(p.returns):
                if track.id in returns:
                    self._push(RemoveReturnCommand(p, track.id, text))
        self.undo_stack.endMacro()

    # --- Returns and sends --------------------------------------------------------

    def add_return_track(self, index: int | None = None, name: str | None = None) -> Track:
        """Ctrl+Alt+T: a return track, last (or at `index` among the returns)."""
        p = self.project
        index = len(p.returns) if index is None else max(0, min(index, len(p.returns)))
        track = Track(id=new_id(), name=name or p.unique_track_name(f"{return_letter(index)} Return"),
                      color=p.next_color(), kind=RETURN_KIND)
        self._push(InsertReturnCommand(p, track, index))
        return p.track(track.id)

    def set_send(self, track_id: str, return_id: str, level_db: float | None = None, pre_fader: bool | None = None,
                 merge_key: object | None = None) -> None:
        """A track's (or a group's, or a return's) send to a return: its level and
        where it taps. A send not made yet starts silent and after the fader.
        Raises ValueError for a send the routing can't have (into itself, or a
        cycle among returns)."""
        p = self.project
        track = p.track(track_id)
        if track.is_master or not p.has_return(return_id) or p.would_cycle(track_id, return_id):
            raise ValueError(f"{track.name} can't send to that track")
        old = track.sends.get(return_id, Send())
        new = Send(old.level_db if level_db is None else max(MIN_VOLUME_DB, min(MAX_VOLUME_DB, level_db)),
                   old.pre_fader if pre_fader is None else pre_fader)
        if return_id not in track.sends or new != old:
            text = "Change Send" if new.pre_fader == old.pre_fader else "Toggle Pre-Fader Send"
            self._push(UpdateTrackCommand(p, track_id, "sends", track.sends, {**track.sends, return_id: new}, text,
                                          merge_key))
        if level_db is not None:
            self.parameter_touched.emit(track_id, automation.send_key(return_id))

    def remove_send(self, track_id: str, return_id: str) -> None:
        track = self.project.track(track_id)
        if return_id in track.sends:
            kept = {r: send for r, send in track.sends.items() if r != return_id}
            self._push(UpdateTrackCommand(self.project, track_id, "sends", track.sends, kept, "Remove Send"))

    # --- Groups -------------------------------------------------------------------

    def _roots(self, track_ids) -> list[str]:
        """The tracks among these that aren't in a group among them, in track order."""
        p = self.project
        wanted = {t for t in track_ids if p.has_track(t)}
        return [t.id for t in p.tracks if t.id in wanted and not any(a in wanted for a in p.ancestors(t.id))]

    def _arranged(self, roots: list[str], at: int, parent: str | None) -> list[tuple[str, str | None]]:
        """The tree with these tracks (and what is in them) taken out and put, in
        their order, before the track at `at` (in the tree as it is), in `parent`."""
        p = self.project
        moving = set(roots) | {d.id for r in roots for d in p.descendants(r)}
        staying = [(t.id, t.parent) for t in p.tracks if t.id not in moving]
        block = [(t.id, parent if t.id in roots else t.parent) for t in p.tracks if t.id in moving]
        position = sum(1 for t in p.tracks[:max(0, at)] if t.id not in moving)
        return staying[:position] + block + staying[position:]

    def _arrange(self, tree, text: str) -> None:
        """Arranges the tracks so. A track taking its input from a group it comes
        into (or from what that group feeds) loses that input first: it would
        close a cycle; and so does a device taking its sidechain from one."""
        tree = tuple(tree)
        if tree == self.project.tree():
            return
        p = self.project
        parents = dict(tree)
        # Copies, without their inputs and sidechains, which come back one by one
        # unless they close a cycle with those before them.
        # (Their devices only as far as sidechains go: those kept, one by one, flat.)
        arranged = [replace(t, parent=parents.get(t.id), input_track=None, devices=[]) for t in p.tracks]
        returns = [replace(r, devices=[]) for r in p.returns]
        cycling, cycling_sidechains = [], []
        for track, original in zip(arranged, p.tracks, strict=True):
            source = original.input_track
            if source is not None and feeds(routing_graph(arranged, returns), track.id, source):
                cycling.append(track.id)
            else:
                track.input_track = source
        for track, original in zip([*arranged, *returns], [*p.tracks, *p.returns], strict=True):
            for device in iter_devices(original.devices):
                if device.sidechain is None:
                    continue
                if feeds(routing_graph(arranged, returns), track.id, device.sidechain.track_id):
                    cycling_sidechains.append((track.id, device))
                else:
                    track.devices.append(replace(device, chains=[]))
        if not cycling and not cycling_sidechains:
            self._push(ArrangeTracksCommand(p, p.tree(), tree, text))
            return
        self.undo_stack.beginMacro(text)
        try:
            for track_id in cycling:
                self._push(UpdateTrackCommand(p, track_id, "input_track", p.track(track_id).input_track, None, text))
            for track_id, device in cycling_sidechains:
                self._push(SetDeviceSidechainCommand(p, track_id, device.id, device.sidechain, None, text))
            self._push(ArrangeTracksCommand(p, p.tree(), tree, text))
        finally:
            self.undo_stack.endMacro()

    def _valid(self, tree) -> bool:
        tracks = {t.id: t for t in self.project.tracks}
        arranged = [Track(id=i, name=tracks[i].name, color="", kind=tracks[i].kind, parent=parent) for i, parent in tree]
        return tree_problem(arranged) is None

    def group_tracks(self, track_ids) -> Track | None:
        """Ctrl+G: a new group holding these tracks (and what is in them), where
        the first of them was, in its group. One undo step; the new group."""
        p = self.project
        roots = self._roots(track_ids)
        if not roots:
            return None
        first = p.track(roots[0])
        index = p.track_index(first.id)
        group = Track(id=new_id(), name=p.unique_track_name(f"{len(p.tracks) + 1} Group"), color=p.next_color(),
                      kind=GROUP_KIND, parent=first.parent)
        text = "Group Tracks"
        self.undo_stack.beginMacro(text)
        try:
            self._push(InsertTrackCommand(p, group, index, text))
            self._arrange(self._arranged(roots, p.track_index(group.id) + 1, group.id), text)
        finally:
            self.undo_stack.endMacro()
        return p.track(group.id)

    def ungroup(self, group_ids) -> None:
        """Ctrl+Shift+G: the groups go, and what was in them takes their place, in
        their groups. One undo step."""
        p = self.project
        groups = [t for t in self._roots(group_ids) if p.track(t).is_group]
        groups += [d.id for g in list(groups) for d in p.descendants(g) if d.is_group and d.id in set(group_ids)]
        if not groups:
            return
        text = "Ungroup Tracks"
        self.undo_stack.beginMacro(text)
        try:
            self._drop_inputs(set(groups), text)
            self._drop_sidechains(set(groups), text)
            tree = list(p.tree())
            for group_id in groups:
                parent = dict(tree)[group_id]
                tree = [(t, parent if t_parent == group_id else t_parent) for t, t_parent in tree]
            self._arrange(tree, text)
            for group_id in reversed(groups):
                self._push(RemoveTrackCommand(p, group_id, text))
        finally:
            self.undo_stack.endMacro()

    def can_move_tracks(self, track_ids, index: int, parent: str | None) -> bool:
        """Whether move_tracks would do something with these."""
        p = self.project
        roots = self._roots(track_ids)
        if not roots or (parent is not None and (not p.has_track(parent) or not p.track(parent).is_group)):
            return False
        if parent in roots or any(parent is not None and p.is_descendant(parent, r) for r in roots):
            return False  # a group can't go into itself
        tree = self._arranged(roots, index, parent)
        return tuple(tree) != p.tree() and self._valid(tree)

    def move_tracks(self, track_ids, index: int, parent: str | None) -> bool:
        """Move tracks (and what is in them) to before the track at `index` (as the
        tracks are now; past the end: last), into group `parent` (None: none). One
        undo step; False if they can't go there (a group into itself, or amid
        another group's tracks)."""
        if not self.can_move_tracks(track_ids, index, parent):
            return False
        roots = self._roots(track_ids)
        self._arrange(self._arranged(roots, index, parent), "Move Track" if len(roots) == 1 else "Move Tracks")
        return True

    def set_folded(self, track_id: str, folded: bool) -> None:
        """Fold or unfold a track: folded, it is a thin row without its automation;
        a folded group hides what is in it. View state: saved, not undone."""
        if self.project.track(track_id).folded != folded:
            self.project.update_track(track_id, folded=folded)

    def duplicate_tracks(self, track_ids) -> list[Track]:
        """Ctrl+D on tracks: a copy of each (a group with what is in it), together
        after the last of them (and what is in it), in its group. The copies have
        new clips and devices (plug-ins in the state last stored in the model:
        store their states first) and the same automation, sends, inputs and
        sidechains (from the copies, where they came from tracks copied with
        them); they aren't armed. One undo step; the copies of these tracks."""
        p = self.project
        roots = self._roots(track_ids)
        if not roots:
            return []
        originals = [t for r in roots for t in [p.track(r), *p.descendants(r)]]
        renamed: dict[str, str] = {t.id: new_id() for t in originals}
        last = roots[-1]
        index, parent = p.subtree_end(p.track_index(last)), p.track(last).parent
        copies = []
        for original in originals:
            track = copy.deepcopy(original)
            track.id = renamed[original.id]
            track.name = p.unique_track_name(original.name)
            track.armed = False
            track.parent = parent if original.id in roots else renamed[original.parent]
            track.clips = [replace(c, id=new_id()) for c in track.clips]
            ids: dict[str, str] = {}  # its devices' and rack chains' ids: the copies'
            for device in track.devices:
                old = [d.id for d in iter_devices([device])] + [c.id for _r, c in iter_chains([device])]
                refresh_ids(device)
                new = [d.id for d in iter_devices([device])] + [c.id for _r, c in iter_chains([device])]
                ids.update(zip(old, new, strict=True))
            for device in iter_devices(track.devices):
                if device.sidechain is not None and device.sidechain.track_id in renamed:
                    device.sidechain = replace(device.sidechain, track_id=renamed[device.sidechain.track_id])
            p.folded_devices |= {new for old, new in ids.items() if old in p.folded_devices}
            if track.input_track in renamed:
                track.input_track = renamed[track.input_track]
            track.automation = {_renamed_key(k, ids): points for k, points in track.automation.items()}
            view = track.automation_view
            track.automation_view = replace(view, key=view.key and _renamed_key(view.key, ids),
                                            lanes=tuple(_renamed_key(k, ids) for k in view.lanes))
            copies.append(track)
        text = "Duplicate Track" if len(roots) == 1 else "Duplicate Tracks"
        self.undo_stack.beginMacro(text)
        try:
            for offset, track in enumerate(copies):
                if track.parent == parent:  # (a group the tree doesn't let it be in: the one there)
                    self._insert_track(track, index + offset, parent, text)
                else:
                    self._push(InsertTrackCommand(p, track, index + offset, text))
        finally:
            self.undo_stack.endMacro()
        return [p.track(renamed[r]) for r in roots]

    def rename_track(self, track_id: str, name: str) -> None:
        old = self.project.track(track_id).name
        if name and name != old:
            self._push(UpdateTrackCommand(self.project, track_id, "name", old, name, "Rename Track"))

    def set_track_color(self, track_id: str, color: str) -> None:
        old = self.project.track(track_id).color
        if color != old:
            self._push(UpdateTrackCommand(self.project, track_id, "color", old, color, "Change Track Color"))

    def set_track_param(self, track_id: str, attr: str, value, merge_key: object | None = None) -> None:
        """Mixer settings: volume_db, pan, mute, solo (the master: volume_db and pan).
        Solo is a listening aid: saved, but not undone (see solo_tracks)."""
        labels = {"volume_db": "Change Volume", "pan": "Change Pan", "mute": "Toggle Track Activator"}
        if track_id == MASTER:
            if attr not in ("volume_db", "pan"):
                raise AttributeError(attr)  # the master is always heard
            labels = {"volume_db": "Change Master Volume", "pan": "Change Master Pan"}
        if attr == "solo":
            self.solo_tracks([track_id], value)
            return
        if attr == "pan":
            value = max(-1.0, min(1.0, value))
        old = getattr(self.project.track(track_id), attr)
        if value != old:
            self._push(UpdateTrackCommand(self.project, track_id, attr, old, value, labels[attr], merge_key))
        touched = {"volume_db": MIXER_VOLUME, "pan": MIXER_PAN}.get(attr)
        if touched:
            self.parameter_touched.emit(track_id, touched)

    def set_tracks_param(self, values: dict[str, float], attr: str, merge_key: object | None = None) -> None:
        """volume_db or pan on several tracks (track id -> value) as one undo step."""
        limits = (-70.0, 6.0) if attr == "volume_db" else (-1.0, 1.0)
        new = {t: max(limits[0], min(limits[1], v)) for t, v in values.items()}
        old = {t: getattr(self.project.track(t), attr) for t in new}
        if len(new) == 1:
            (track_id, value), = new.items()
            self.set_track_param(track_id, attr, value, merge_key)
            return
        if new != old:
            text = "Change Volume" if attr == "volume_db" else "Change Pan"
            self._push(UpdateTracksCommand(self.project, attr, old, new, text, merge_key))
        touched = MIXER_VOLUME if attr == "volume_db" else MIXER_PAN
        for track_id in new:
            self.parameter_touched.emit(track_id, touched)

    def solo_tracks(self, track_ids, solo: bool, exclusive: bool = False) -> None:
        """Solo (or unsolo) these tracks. `exclusive` (soloing): every other track
        is unsoloed. A listening aid, as in Ableton: saved, but not an undo step."""
        track_ids = set(track_ids)
        for track in self.project.senders():
            if track.id in track_ids or (exclusive and solo):
                new = track.id in track_ids and solo
                if track.solo != new:
                    self.project.update_track(track.id, solo=new)

    def set_track_input(self, track_id: str, channels) -> None:
        """A track's audio input: device channels, () for none."""
        channels = tuple(int(c) for c in channels)
        if len(channels) > 2:
            raise ValueError("an input is one channel or a pair")
        self._set_input(track_id, channels, None)

    def set_track_input_track(self, track_id: str, source_id: str | None) -> None:
        """An audio track's input from another track's output, after its fader (a
        track, a group or a return), or the master's (MASTER: resampling), instead
        of device channels; None: no input. Raises ValueError for a source it
        can't take (itself, or a track it feeds: a cycle)."""
        p = self.project
        track = p.track(track_id)
        if source_id is not None and (
                not track.is_audio or not (source_id == MASTER or p.has_track(source_id) or p.has_return(source_id))
                or p.input_would_cycle(track_id, source_id)):
            raise ValueError(f"{track.name} can't take its input from that track")
        self._set_input(track_id, (), source_id)

    def _set_input(self, track_id: str, channels: tuple[int, ...], source_id: str | None) -> None:
        track = self.project.track(track_id)
        old = {"input": track.input, "input_track": track.input_track}
        new = {"input": channels, "input_track": source_id}
        if new != old:
            self._push(UpdateTrackFieldsCommand(self.project, track_id, old, new, "Change Track Input"))

    def _drop_inputs(self, source_ids: set[str], text: str) -> None:
        """The tracks taking their input from these (going away) have none."""
        for track in self.project.tracks:
            if track.input_track in source_ids:
                self._push(UpdateTrackCommand(self.project, track.id, "input_track", track.input_track, None, text))

    def _drop_sidechains(self, source_ids: set[str], text: str) -> None:
        """The devices taking their sidechain from these (going away, with their
        own devices) have none. (Those on them keep theirs: they come back together.)"""
        for track in self.project.all_tracks():
            if track.id in source_ids:
                continue
            for device in iter_devices(track.devices):
                if device.sidechain is not None and device.sidechain.track_id in source_ids:
                    self._push(SetDeviceSidechainCommand(self.project, track.id, device.id, device.sidechain, None,
                                                         text))

    def set_track_monitor(self, track_id: str, mode: str) -> None:
        if mode not in MONITOR_MODES:
            raise ValueError(mode)
        old = self.project.track(track_id).monitor
        if mode != old:
            self._push(UpdateTrackCommand(self.project, track_id, "monitor", old, mode, "Change Monitoring"))

    def set_track_midi_input(self, track_id: str, midi_input: MidiInput | None) -> None:
        """A MIDI track's MIDI input (None: none)."""
        if midi_input is not None and not 0 <= midi_input.channel <= 16:
            raise ValueError("a MIDI channel is 1-16, or 0 for all")
        old = self.project.track(track_id).midi_input
        if midi_input != old:
            self._push(UpdateTrackCommand(self.project, track_id, "midi_input", old, midi_input,
                                          "Change MIDI Input"))

    def arm_tracks(self, track_ids, armed: bool, exclusive: bool = False) -> None:
        """Arm (or disarm) tracks for recording; `exclusive` (arming): every other
        track is disarmed. Like heights, arming is saved but not undone."""
        track_ids = set(track_ids)
        for track in self.project.tracks:
            if track.is_group:
                continue  # nothing to record
            wanted = armed if track.id in track_ids else (track.armed and not (exclusive and armed))
            if wanted != track.armed:
                self.project.update_track(track.id, armed=wanted)

    def add_recordings(self, takes: list[RecordedTake], quantize: float = 0.0) -> list[ClipRef]:
        """Finished takes become clips, one undo step: audio clips, and MIDI clips
        of the notes played (their starts on the grid of `quantize` beats, if
        given: record quantization). As in Ableton's Arrangement recording, a
        take replaces what was under it."""
        tempo = self.project.tempo
        by_track: dict[str, list[AnyClip]] = {}
        for take in takes:
            if not self.project.has_track(take.track_id) or take.duration_sec <= 0:
                continue
            track = self.project.track(take.track_id)
            if track.is_midi != take.midi:
                continue
            if take.midi:
                by_track.setdefault(take.track_id, []).append(self._recorded_midi_clip(track, take, quantize))
                continue
            start_beat = seconds_to_beats(take.start_sec, tempo)
            offset = 0.0
            if start_beat < 0:  # it began before the timeline (the count-in's latency)
                offset = -take.start_sec
                start_beat = 0.0
            duration = take.duration_sec - offset
            if duration <= 0:
                continue
            by_track.setdefault(take.track_id, []).append(Clip(
                id=new_id(), path=take.path, name=Path(take.path).stem, start_beat=start_beat,
                duration_sec=duration, offset_sec=offset, source_duration_sec=take.duration_sec))
        after = {}
        refs = []
        for track_id, clips in by_track.items():
            new_ids = {c.id for c in clips}
            after[track_id] = edits.resolve_overlaps(list(self.project.track(track_id).clips) + clips, new_ids, tempo)
            refs += [(track_id, c.id) for c in clips]
        if after:
            self._commit("Record", after)
        return refs

    def _recorded_midi_clip(self, track: Track, take: RecordedTake, quantize: float) -> MidiClip:
        tempo = self.project.tempo
        start = max(0.0, seconds_to_beats(take.start_sec, tempo))
        end = seconds_to_beats(take.start_sec + take.duration_sec, tempo)
        clip_notes = []
        for note_start, note_end, pitch, velocity in take.notes:
            begin = seconds_to_beats(note_start, tempo) - start
            length = seconds_to_beats(note_end, tempo) - start - begin
            if quantize > 0:  # on the arrangement's grid, unless that is outside the clip
                snapped = round((start + begin) / quantize) * quantize - start
                if 0 <= snapped < end - start:
                    begin = snapped
            if begin < 0 or begin >= end - start or length <= 0:
                continue
            clip_notes.append(Note(pitch=max(0, min(127, int(pitch))), start=begin, length=length,
                                   velocity=max(1, min(127, int(velocity)))))
        return MidiClip(id=new_id(), name=track.name, start_beat=start, duration_beats=end - start,
                        notes=notes.normalize(clip_notes))

    def set_track_height(self, track_id: str, height: int) -> None:
        # View state: saved with the project but not worth an undo step.
        self.project.update_track(track_id, height=height)
