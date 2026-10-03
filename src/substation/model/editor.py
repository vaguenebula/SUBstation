"""High-level, undoable edit operations used by the UI."""

from __future__ import annotations

import copy
import math
from collections.abc import Callable
from dataclasses import dataclass, replace
from pathlib import Path

from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QUndoStack

from .. import _engine as ge
from . import automation, edits, notes
from .automation import (
    MASTER,
    MAX_VOLUME_DB,
    MIN_VOLUME_DB,
    MIXER_PAN,
    MIXER_VOLUME,
    AutomationView,
    Envelope,
)
from .commands import (
    ArrangeTracksCommand,
    InsertReturnCommand,
    InsertTrackCommand,
    RemoveReturnCommand,
    RemoveTrackCommand,
    SetChainsCommand,
    SetClipsCommand,
    SetDeviceEnabledCommand,
    SetDeviceNameCommand,
    SetDeviceParamCommand,
    SetDeviceParamsCommand,
    SetDevicesCommand,
    SetDeviceSidechainCommand,
    SetDeviceStateCommand,
    SetEnvelopeCommand,
    SetEnvelopesCommand,
    SetMacrosCommand,
    SetTempoCommand,
    UpdateChainCommand,
    UpdateSettingsCommand,
    UpdateTrackCommand,
    UpdateTrackFieldsCommand,
    UpdateTracksCommand,
)
from .keys import Key, clip_settings
from .project import (
    GROUP_KIND,
    MACRO_COUNT,
    MAX_RACK_DEPTH,
    MONITOR_MODES,
    PLUGIN_KIND,
    RACK_KIND,
    RETURN_KIND,
    AnyClip,
    Chain,
    Clip,
    Device,
    MacroMapping,
    MidiClip,
    MidiInput,
    Note,
    PluginRef,
    Project,
    Send,
    Sidechain,
    Track,
    chain_devices,
    container_of,
    feeds,
    find_device,
    iter_chains,
    iter_devices,
    macro_param,
    new_id,
    rack_depth,
    rack_height,
    refresh_ids,
    return_letter,
    routing_graph,
    tree_problem,
)
from .timebase import TimeSignature, seconds_to_beats

ClipRef = tuple[str, str]  # (track id, clip id)
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
LaneRef = tuple[str, str]  # (automation owner, target key)


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


@dataclass(frozen=True)
class CopiedAutomation:
    """Automation copied from a lane range (Ctrl+C / Ctrl+X), `length` beats long:
    each lane's envelope over it (points from beat 0), in the lanes' order."""

    length: float
    lanes: tuple[tuple[LaneRef, Envelope], ...]


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


# The built-in devices come from the engine (engine/src/builtin/devices/), so a new one
# needs nothing here. kind: (display name, {param id: default}).
BUILTIN_DEVICES = {
    d.id: (d.name, {p.id: p.default_value for p in d.params}) for d in ge.builtin_devices()
}
# How the browser's Built-in category groups the devices above.
BUILTIN_CATEGORIES: dict[str, list[str]] = {}
for _device in ge.builtin_devices():
    BUILTIN_CATEGORIES.setdefault(_device.category, []).append(_device.id)
BUILTIN_INSTRUMENTS = frozenset(d.id for d in ge.builtin_devices() if d.is_instrument)
DEFAULT_INSTRUMENT = "synth"  # new MIDI tracks come with it, ready to play


def is_instrument(kind: str, plugin: PluginRef | None = None) -> bool:
    """Whether a device of this kind (and plug-in) is an instrument."""
    if kind == PLUGIN_KIND:
        return plugin is not None and plugin.instrument
    return kind in BUILTIN_INSTRUMENTS


def device_is_instrument(device: Device) -> bool:
    """An instrument, or a rack with one in it (an instrument rack: it plays the track's notes)."""
    if device.is_rack:
        return any(device_is_instrument(d) for chain in device.chains for d in chain.devices)
    return is_instrument(device.kind, device.plugin)


def loads_into(preset: Device, device: Device) -> bool:
    """Whether a preset can be loaded into a device in place: a device of the
    same kind (the same plug-in; a rack into a rack, both instrument racks or
    neither)."""
    if preset.kind != device.kind or device_is_instrument(preset) != device_is_instrument(device):
        return False
    if preset.is_plugin:
        return preset.plugin is not None and device.plugin is not None and preset.plugin.uid == device.plugin.uid
    return True


def device_name(device: Device) -> str:
    """What a device is called: a rack by its own name if it has one (its
    preset's), else every device by its kind (kind_name)."""
    if device.is_rack and device.name:
        return device.name
    return kind_name(device)


def kind_name(device: Device) -> str:
    """The name of a device's kind: a plug-in's, a built-in device's, Audio Effect Rack or Instrument Rack."""
    if device.is_rack:
        return "Instrument Rack" if device_is_instrument(device) else "Audio Effect Rack"
    if device.plugin is not None:
        return device.plugin.name
    return BUILTIN_DEVICES.get(device.kind, (device.kind,))[0]


def new_device(kind: str, plugin: PluginRef | None = None) -> Device:
    if kind == PLUGIN_KIND:
        if plugin is None:
            raise ValueError("a plug-in device needs a plug-in")
        return Device(id=new_id(), kind=kind, plugin=plugin)
    if kind == RACK_KIND:
        return new_rack([])
    return Device(id=new_id(), kind=kind, params=dict(BUILTIN_DEVICES[kind][1]))


def new_rack(chains: list[Chain]) -> Device:
    """A rack with these chains, its macros at 0."""
    return Device(id=new_id(), kind=RACK_KIND, params={macro_param(i): 0.0 for i in range(MACRO_COUNT)},
                  chains=chains)


def new_chain(name: str, devices: list[Device] | None = None) -> Chain:
    return Chain(id=new_id(), name=name, devices=devices or [])


def builtin_param_info(kind: str, param_id: str):
    """A built-in device's parameter as the engine describes it (None: no such one)."""
    return next((p for d in ge.builtin_devices() if d.id == kind for p in d.params if p.id == param_id), None)


def device_ids_of_list(devices: list[Device]) -> set[str]:
    """The ids of these devices and of everything in them."""
    return {d.id for d in iter_devices(devices)}


def device_ids_of(device: Device) -> set[str]:
    """A device's id, and those of everything in it (a rack)."""
    return device_ids_of_list([device])


class ProjectEditor(QObject):
    # (track id, device id) when the user adds a plug-in (not on undo or redo).
    plugin_added = Signal(str, str)
    # (automation owner, target key) when the user changes a parameter that can be
    # automated (not on undo or redo): its automation lane shows it.
    parameter_touched = Signal(str, str)

    def __init__(self, project: Project, undo_stack: QUndoStack):
        super().__init__()
        self.project = project
        self.undo_stack = undo_stack

    def _push(self, command) -> None:
        self.undo_stack.push(command)

    # --- Tracks -----------------------------------------------------------------

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

    def set_key(self, key: Key | None) -> None:
        """The project's key (None: none). Audio added afterwards is transposed to it."""
        self._set_settings("Change Key", key=key)

    def set_loop(self, enabled: bool, start: float, end: float, merge_key: object | None = None) -> None:
        start = max(0.0, start)
        end = max(start + 0.25, end)
        self._set_settings("Change Loop", merge_key, loop_enabled=enabled, loop_start=start, loop_end=end)

    def set_loop_enabled(self, enabled: bool) -> None:
        self._set_settings("Toggle Loop", loop_enabled=enabled)

    # --- Clips -------------------------------------------------------------------

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
        return track_id, clip.id

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

    # --- Devices -----------------------------------------------------------------

    def set_device_defaults(self, default: Callable[[str, PluginRef | None], Device | None]) -> None:
        """Where new devices come from: default(kind, plugin) is a new device as
        the user's default preset for that kind has it, or None (as it comes;
        presets.default_device). Without it, devices start as they come."""
        self._default_device = default

    def _new_device(self, kind: str, plugin: PluginRef | None = None) -> Device:
        """A new device of a kind: as its default preset has it, if there is one."""
        default = getattr(self, "_default_device", None)
        device = default(kind, plugin) if default is not None else None
        return device if device is not None else new_device(kind, plugin)

    def add_device(self, track_id: str, kind: str, index: int | None = None,
                   plugin: PluginRef | None = None, chain: str | None = None) -> Device | None:
        """Add a device (a built-in `kind`, kind 'plugin' and a `plugin`, or an
        empty rack) to a track's (or the master's) chain, or to a rack's chain on
        it (`chain`: its id), as its default preset has it if there is one. An
        instrument only goes on a MIDI track (None otherwise), where it comes
        first in its chain and replaces any other instrument there."""
        device = self._new_device(kind, plugin)
        return device if self.insert_device(track_id, device, index, chain, f"Add {device_name(device)}") else None

    def insert_device(self, track_id: str, device: Device, index: int | None = None, chain: str | None = None,
                      text: str | None = None, show_editors: bool = True) -> bool:
        """Put a new device (a whole rack too: a preset) into a chain, as add_device
        does. One undo step; False if it can't go there."""
        return bool(self.insert_devices(track_id, [device], index, chain, text or f"Add {device_name(device)}",
                                        show_editors))

    def insert_devices(self, track_id: str, devices: list[Device], index: int | None = None,
                       chain: str | None = None, text: str = "Add Devices", show_editors: bool = True) -> list[Device]:
        """Put new devices (racks too) into a chain, in their order, before the
        device at `index` (None: last), as add_device does: an instrument only on
        a MIDI track, first, replacing the one there. One undo step; the devices
        that went in (none: nothing changed). Their plug-ins' editors show if
        `show_editors`."""
        track = self.project.track(track_id)
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        target = chain_devices(after, chain)
        if target is None:
            return []
        depth = rack_depth(after, chain)
        first = 1 if target and device_is_instrument(target[0]) else 0  # effects go after the instrument
        at = len(target) if index is None else max(first, min(index, len(target)))
        added = []
        for device in devices:
            if depth + rack_height(device) > MAX_RACK_DEPTH:
                continue
            if device_is_instrument(device):
                if not track.is_midi:
                    continue
                kept = [d for d in target if not device_is_instrument(d)]
                at += len(kept) - len(target) + 1  # (one may have gone from before it; this one goes first)
                target[:] = [device, *kept]
            else:
                first = 1 if target and device_is_instrument(target[0]) else 0
                at = max(first, at)
                target.insert(at, device)
                at += 1
            added.append(device)
        if not added:
            return []
        self._set_devices(track_id, before, after, text)
        if show_editors:
            for added_device in iter_devices(added):
                if added_device.is_plugin:
                    self.plugin_added.emit(track_id, added_device.id)
        return added

    def copy_devices(self, track_id: str, device_ids) -> list[Device]:
        """Copies of devices (racks with everything in them; those in a selected
        rack go with it), in their order on the track, for pasting: plug-ins in the
        state last stored in the model (store their states first)."""
        wanted = set(device_ids)
        found = [d for d in iter_devices(self.project.track(track_id).devices) if d.id in wanted]
        inside = {i for d in found for i in device_ids_of(d) if i != d.id}
        return [copy.deepcopy(d) for d in found if d.id not in inside]

    def paste_devices(self, track_id: str, copied: list[Device], index: int | None = None,
                      chain: str | None = None, folded=frozenset(), text: str | None = None) -> list[Device]:
        """New devices like the copied ones (copy_devices) into a chain of a track,
        before the device at `index` (None: last), as insert_devices puts them.
        Their sidechains stay, unless the source is gone or would close a cycle
        here. Those whose originals are `folded` are folded too. One undo step;
        the devices pasted."""
        devices = []
        for original in copied:
            device = copy.deepcopy(original)
            old_ids = [d.id for d in iter_devices([device])]
            refresh_ids(device)
            for old_id, inner in zip(old_ids, iter_devices([device]), strict=True):
                if old_id in folded:
                    self.project.folded_devices.add(inner.id)
                if inner.sidechain is not None and (not self.project.has_owner(inner.sidechain.track_id)
                                                    or self.project.sidechain_would_cycle(
                                                        track_id, inner.sidechain.track_id)):
                    inner.sidechain = None
            devices.append(device)
        if text is None:
            text = f"Paste {device_name(devices[0])}" if len(devices) == 1 else "Paste Devices"
        return self.insert_devices(track_id, devices, index, chain, text, show_editors=False)

    def set_devices_folded(self, track_id: str, device_ids, folded: bool) -> None:
        """Fold or unfold devices in the device view. View state: saved, not undone."""
        self.project.set_devices_folded(track_id, device_ids, folded)

    def move_device(self, track_id: str, device_id: str, index: int) -> None:
        """Move a device to position `index` in its chain (an instrument stays first)."""
        devices = self.project.track(track_id).devices
        chain = container_of(devices, device_id)
        ids = [d.id for d in chain_devices(devices, chain)]
        # move_devices counts positions in the chain before the move: moving right skips the device itself.
        self.move_devices(track_id, [device_id], index + 1 if index > ids.index(device_id) else index, chain)

    @staticmethod
    def _prune_macros(devices: list[Device]) -> None:
        """Drop the macro mappings whose device isn't (any longer) inside its rack:
        a macro moves parameters of devices in its rack."""
        for rack in iter_devices(devices):
            if not rack.macros:
                continue
            inside = device_ids_of(rack) - {rack.id}
            kept = tuple(m for m in rack.macros if m.device_id in inside)
            if kept != rack.macros:
                rack.macros = kept

    @staticmethod
    def _outermost(devices: list[Device], device_ids) -> list[Device]:
        """The devices of these ids (not instruments), in their order on the track,
        but those in racks among them (they go along with their rack)."""
        wanted = set(device_ids)
        found = [d for d in iter_devices(devices) if d.id in wanted and not device_is_instrument(d)]
        inside = {i for d in found for i in device_ids_of(d) if i != d.id}
        return [d for d in found if d.id not in inside]

    def move_devices(self, track_id: str, device_ids, index: int, chain: str | None = None) -> bool:
        """Move devices together (in their order on the track) to before the
        device at `index` in a chain of the track as it is now (the end if past
        it): its own (`chain` None) or a rack's. One undo step; an instrument
        doesn't move, and nothing goes before one; a rack doesn't go into itself,
        nor nest too deep. False if nothing moved."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        target = chain_devices(after, chain)
        moving = self._outermost(after, device_ids)
        if not moving or target is None:
            return False
        inside = set().union(*(device_ids_of(m) for m in moving))
        if chain is not None and self.project.chain_rack(track_id, chain).id in inside:
            return False  # into itself
        depth = rack_depth(after, chain)
        if any(depth + rack_height(d) > MAX_RACK_DEPTH for d in moving):
            return False
        at = sum(1 for d in target[:max(0, index)] if d.id not in inside)
        for device in moving:  # out of wherever they are
            chain_devices(after, container_of(after, device.id)).remove(device)
        first = 1 if target and device_is_instrument(target[0]) else 0
        at = max(first, min(at, len(target)))
        target[at:at] = moving
        self._prune_macros(after)  # (a device out of its rack leaves its macros)
        if after == before:
            return False
        self._push(SetDevicesCommand(self.project, track_id, before, after,
                                     "Move Device" if len(moving) == 1 else "Move Devices"))
        return True

    def move_devices_to_track(self, track_id: str, device_ids, to_track_id: str, index: int | None = None,
                              chain: str | None = None) -> bool:
        """Move effects (racks too, with everything in them; in their order on the
        track) to another track's (or the master's) chain, or a rack's chain there
        (`chain`), before the device at `index` (None: last; never before its
        instrument). They stay the same devices, so plug-ins keep their state, and
        their automation goes with them, and their sidechains (unless one would
        close a cycle there). One undo step; False if nothing moved."""
        if to_track_id == track_id:
            if index is None and chain is None:
                return False
            target = chain_devices(self.project.track(track_id).devices, chain) or []
            return self.move_devices(track_id, device_ids, len(target) if index is None else index, chain)
        source = copy.deepcopy(self.project.track(track_id).devices)
        target_devices = copy.deepcopy(self.project.track(to_track_id).devices)
        moving = self._outermost(source, device_ids)
        target = chain_devices(target_devices, chain)
        if not moving or target is None:
            return False
        if any(rack_depth(target_devices, chain) + rack_height(d) > MAX_RACK_DEPTH for d in moving):
            return False
        for device in moving:
            chain_devices(source, container_of(source, device.id)).remove(device)
            for inner in iter_devices([device]):  # a sidechain from where they go (or what that feeds) would close a cycle
                if inner.sidechain is not None and self.project.sidechain_would_cycle(to_track_id,
                                                                                     inner.sidechain.track_id):
                    inner.sidechain = None
        first = 1 if target and device_is_instrument(target[0]) else 0
        at = len(target) if index is None else max(first, min(index, len(target)))
        target[at:at] = moving
        self._prune_macros(source)
        self._prune_macros(target_devices)
        before = {track_id: copy.deepcopy(self.project.track(track_id).devices),
                  to_track_id: copy.deepcopy(self.project.track(to_track_id).devices)}
        text = "Move Device" if len(moving) == 1 else "Move Devices"
        moved = set().union(*(device_ids_of(d) for d in moving))
        envelopes = {key: points for key, points in self.project.automation(track_id).items()
                     if automation.key_device(key) in moved}
        if not envelopes:
            self._push(SetChainsCommand(self.project, before, {track_id: source, to_track_id: target_devices}, text))
            return True
        self.undo_stack.beginMacro(text)
        self._push(SetChainsCommand(self.project, before, {track_id: source, to_track_id: target_devices}, text))
        old = {(owner, key): self.project.envelope(owner, key) for key in envelopes for owner in (track_id, to_track_id)}
        new = {(track_id, key): () for key in envelopes} | {(to_track_id, key): points for key, points in envelopes.items()}
        self._push(SetEnvelopesCommand(self.project, old, new, text))
        self.undo_stack.endMacro()
        return True

    def remove_device(self, track_id: str, device_id: str) -> None:
        self.remove_devices(track_id, [device_id])

    def remove_devices(self, track_id: str, device_ids) -> None:
        """Delete devices from a track (in racks too; a rack with what is in it), in one undo step."""
        ids = set(device_ids)
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)

        def prune(devices: list[Device]) -> None:
            devices[:] = [d for d in devices if d.id not in ids]
            for device in devices:
                for chain in device.chains:
                    prune(chain.devices)

        prune(after)
        if after != before:
            removed = len(device_ids_of_list(before)) - len(device_ids_of_list(after))
            self._set_devices(track_id, before, after, "Delete Device" if removed == 1 else "Delete Devices")

    def _set_devices(self, track_id: str, before: list[Device], after: list[Device], text: str) -> None:
        """Change a track's devices; the automation of devices that leave it goes
        with them (their parameters', and a rack's chains' faders'), and so do the
        mappings of macros to them (in the same undo step)."""
        gone = device_ids_of_list(before) - device_ids_of_list(after)
        chains_gone = {c.id for _, c in iter_chains(before)} - {c.id for _, c in iter_chains(after)}
        self._prune_macros(after)
        orphans = [key for key in self.project.track(track_id).automation
                   if automation.key_device(key) in gone or automation.key_chain(key) in chains_gone]
        if not orphans:
            self._push(SetDevicesCommand(self.project, track_id, before, after, text))
            return
        self.undo_stack.beginMacro(text)
        self._push(SetDevicesCommand(self.project, track_id, before, after, text))
        for key in orphans:
            self._push(SetEnvelopeCommand(self.project, track_id, key, self.project.envelope(track_id, key), (), text))
        self.undo_stack.endMacro()

    # --- Racks ---------------------------------------------------------------------

    def group_devices(self, track_id: str, device_ids) -> Device | None:
        """Ctrl+G in the device view: these devices (in one chain, in its order)
        go into a new rack, in one chain, where the first of them was. One undo
        step; the rack (None if they aren't all in one chain, or it would nest
        too deep)."""
        track = self.project.track(track_id)
        wanted = {i for i in device_ids if find_device(track.devices, i) is not None}
        containers = {container_of(track.devices, i) for i in wanted}
        if len(containers) != 1:
            return None
        chain = containers.pop()
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        devices = chain_devices(after, chain)
        grouped = [d for d in devices if d.id in wanted]
        if rack_depth(after, chain) + 1 + max(rack_height(d) for d in grouped) > MAX_RACK_DEPTH:
            return None
        rack = new_rack([new_chain(device_name(grouped[0]), grouped)])
        at = devices.index(grouped[0])
        devices[:] = [d for d in devices if d.id not in wanted]
        devices.insert(at, rack)
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Group Devices"))
        return self.project.device(track_id, rack.id)

    def ungroup_rack(self, track_id: str, rack_id: str) -> bool:
        """Ctrl+Shift+G: a rack goes, and its chains' devices take its place, one
        chain after another (an instrument coming out goes first). The automation
        of its chains' faders and its macros go too. One undo step; False if it
        isn't a rack, or several instruments would come out of it (layered
        instruments: a chain has just one)."""
        track = self.project.track(track_id)
        rack = find_device(track.devices, rack_id)
        if rack is None or not rack.is_rack:
            return False
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        devices = chain_devices(after, container_of(after, rack_id))
        at = next(i for i, d in enumerate(devices) if d.id == rack_id)
        devices[at:at + 1] = [d for chain in devices[at].chains for d in chain.devices]
        if sum(1 for d in devices if device_is_instrument(d)) > 1:
            return False
        instrument = next((d for d in devices if device_is_instrument(d)), None)
        if instrument is not None and devices[0] is not instrument:
            devices.remove(instrument)
            devices.insert(0, instrument)
        self._set_devices(track_id, before, after, "Ungroup Rack")
        return True

    def add_rack_chain(self, track_id: str, rack_id: str, index: int | None = None,
                       name: str | None = None) -> Chain:
        """A new, empty chain of a rack (last, or at `index`)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = find_device(after, rack_id)
        if rack is None or not rack.is_rack:
            raise ValueError("chains belong to racks")
        chain = new_chain(name or f"Chain {len(rack.chains) + 1}")
        rack.chains.insert(len(rack.chains) if index is None else max(0, min(index, len(rack.chains))), chain)
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Add Chain"))
        return self.project.chain(track_id, chain.id)

    def remove_rack_chains(self, track_id: str, chain_ids) -> None:
        """Delete chains of racks, with their devices (and their automation, and
        their faders'). One undo step."""
        ids = set(chain_ids)
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        for rack, _chain in list(iter_chains(after)):
            rack.chains = [c for c in rack.chains if c.id not in ids]
        if after != before:
            self._set_devices(track_id, before, after, "Delete Chain" if len(ids) == 1 else "Delete Chains")

    def duplicate_rack_chain(self, track_id: str, chain_id: str) -> Chain:
        """A copy of a chain right after it: new devices with the same settings
        (plug-ins in the state they were last saved in, as presets are)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = next(r for r, c in iter_chains(after) if c.id == chain_id)
        index = next(i for i, c in enumerate(rack.chains) if c.id == chain_id)
        holder = new_rack([copy.deepcopy(rack.chains[index])])
        refresh_ids(holder)
        rack.chains.insert(index + 1, holder.chains[0])
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Duplicate Chain"))
        return self.project.chain(track_id, holder.chains[0].id)

    def move_rack_chain(self, track_id: str, chain_id: str, index: int) -> None:
        """Reorder a rack's chains: this one to `index` (among the others)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = next(r for r, c in iter_chains(after) if c.id == chain_id)
        chain = next(c for c in rack.chains if c.id == chain_id)
        rack.chains.remove(chain)
        rack.chains.insert(max(0, min(index, len(rack.chains))), chain)
        if after != before:
            self._push(SetDevicesCommand(self.project, track_id, before, after, "Move Chain"))

    def rename_chain(self, track_id: str, chain_id: str, name: str) -> None:
        old = self.project.chain(track_id, chain_id).name
        if name and name != old:
            self._push(UpdateChainCommand(self.project, track_id, chain_id, "name", old, name, "Rename Chain"))

    def set_chain_param(self, track_id: str, chain_id: str, attr: str, value, merge_key: object | None = None) -> None:
        """A rack chain's mixer: volume_db, pan, mute, solo (saved, but not undone, as a track's)."""
        if attr == "solo":
            if self.project.chain(track_id, chain_id).solo != value:
                self.project.update_chain(track_id, chain_id, solo=value)
            return
        labels = {"volume_db": "Change Chain Volume", "pan": "Change Chain Pan", "mute": "Toggle Chain Activator"}
        if attr == "volume_db":
            value = max(MIN_VOLUME_DB, min(MAX_VOLUME_DB, value))
        elif attr == "pan":
            value = max(-1.0, min(1.0, value))
        old = getattr(self.project.chain(track_id, chain_id), attr)
        if value != old:
            self._push(UpdateChainCommand(self.project, track_id, chain_id, attr, old, value, labels[attr], merge_key))
        touched = {"volume_db": automation.CHAIN_VOLUME, "pan": automation.CHAIN_PAN}.get(attr)
        if touched:
            rack = self.project.chain_rack(track_id, chain_id)
            self.parameter_touched.emit(track_id, automation.chain_key(rack.id, chain_id, touched))

    # Macros: a rack's parameters, each moving the parameters mapped to it. Their
    # values are normalized. param_info() describes a device's parameter (its
    # normalized mapping: a ParamInfo or ParamSpec); the UI hands it plug-ins'
    # (set_param_info); built-in devices' are known without it.

    def set_param_info(self, describe: Callable[[str, str, str], object | None]) -> None:
        """Where param_info() learns about parameters it doesn't know (plug-ins'):
        describe(track id, device id, param id)."""
        self._describe = describe

    def set_own_value(self, read: Callable[[str, str], float | None]) -> None:
        """Where set_macro() learns what a plug-in's parameter is now, as set in
        the plug-in's own editor (the model doesn't have it): read(owner, key)."""
        self._read_own = read

    def param_info(self, track_id: str, device_id: str, param_id: str):
        device = self.project.device(track_id, device_id)
        if not device.is_plugin and not device.is_rack:
            return builtin_param_info(device.kind, param_id)
        describe = getattr(self, "_describe", None)
        return describe(track_id, device_id, param_id) if describe is not None else None

    def macro_targets(self, track_id: str, rack_id: str, index: int, value: float) -> dict[tuple[str, str], float]:
        """What a rack's macro at `value` sets: itself, and each parameter mapped to it (plain values)."""
        rack = self.project.device(track_id, rack_id)
        value = max(0.0, min(1.0, value))
        values = {(rack_id, macro_param(index)): value}
        for mapping in rack.macros:
            if mapping.macro != index or not self.project.has_device(track_id, mapping.device_id):
                continue
            info = self.param_info(track_id, mapping.device_id, mapping.param_id)
            if info is not None:
                values[(mapping.device_id, mapping.param_id)] = info.from_normalized(mapping.target(value))
        return values

    def set_macro(self, track_id: str, rack_id: str, index: int, value: float, merge_key: object | None = None) -> None:
        """Turn a rack's macro: it and every parameter mapped to it, one undo step
        (one per gesture, with a `merge_key`)."""
        new = self.macro_targets(track_id, rack_id, index, value)
        old = {}
        for device_id, param_id in new:
            own = self.project.device(track_id, device_id).params.get(param_id)
            read = getattr(self, "_read_own", None)
            if own is None and read is not None:  # set in a plug-in's own editor
                own = read(track_id, automation.device_key(device_id, param_id))
            if own is None:  # a default value: as it was
                info = self.param_info(track_id, device_id, param_id)
                own = new[(device_id, param_id)] if info is None else getattr(info, "default_value",
                                                                              getattr(info, "default", 0.0))
            old[(device_id, param_id)] = own
        if old != new:
            self._push(SetDeviceParamsCommand(self.project, track_id, old, new, "Change Macro", merge_key))

    def map_macro(self, track_id: str, rack_id: str, index: int, device_id: str, param_id: str,
                  low: float = 0.0, high: float = 1.0) -> None:
        """Map a rack's macro to a parameter of a device in it (a parameter is
        mapped to one macro of the rack at a time)."""
        rack = self.project.device(track_id, rack_id)
        if not rack.is_rack or not 0 <= index < MACRO_COUNT or device_id == rack_id \
                or device_id not in device_ids_of(rack):
            raise ValueError("a macro moves parameters of devices in its rack")
        kept = tuple(m for m in rack.macros if (m.device_id, m.param_id) != (device_id, param_id))
        new = (*kept, MacroMapping(index, device_id, param_id, low, high))
        self._push(SetMacrosCommand(self.project, track_id, rack_id, rack.macros, new, "Map Macro"))

    def unmap_macro(self, track_id: str, rack_id: str, device_id: str, param_id: str) -> None:
        rack = self.project.device(track_id, rack_id)
        kept = tuple(m for m in rack.macros if (m.device_id, m.param_id) != (device_id, param_id))
        if kept != rack.macros:
            self._push(SetMacrosCommand(self.project, track_id, rack_id, rack.macros, kept, "Remove Macro Mapping"))

    def macro_of(self, track_id: str, device_id: str, param_id: str) -> tuple[str, int] | None:
        """The rack and macro a parameter is mapped to (the nearest rack's), if any."""
        devices = self.project.track(track_id).devices
        chain = container_of(devices, device_id)
        while chain is not None:
            rack = self.project.chain_rack(track_id, chain)
            for mapping in rack.macros:
                if (mapping.device_id, mapping.param_id) == (device_id, param_id):
                    return rack.id, mapping.macro
            chain = container_of(devices, rack.id)
        return None

    def set_device_param(self, track_id: str, device_id: str, param_id: str, value: float,
                         merge_key: object | None = None, old: float | None = None) -> None:
        """Change a parameter. `old` is its value before, if the model doesn't know
        it (a plug-in's parameters live in the plug-in)."""
        if old is None:
            old = self.project.device(track_id, device_id).params.get(param_id)
        if old is None or old != value:
            self._push(SetDeviceParamCommand(self.project, track_id, device_id, param_id,
                                             value if old is None else old, value, merge_key))
        self.parameter_touched.emit(track_id, automation.device_key(device_id, param_id))

    def touch_parameter(self, owner: str, key: str) -> None:
        """A parameter taken hold of (clicked) without changing it: as Ableton
        does, the arrangement shows its automation."""
        self.parameter_touched.emit(owner, key)

    def set_device_state(self, track_id: str, device_id: str, old: str | None, new: str,
                         text: str = "Load Preset") -> None:
        """Replace a device's state (base64): a plug-in's, e.g. with a preset, or
        a built-in device's besides its parameters. `old` is its state before,
        to go back to on undo."""
        self._push(SetDeviceStateCommand(self.project, track_id, device_id, old, new, text))

    def load_preset_into(self, track_id: str, device_id: str, preset: Device, text: str = "Load Preset") -> bool:
        """Load a preset (a device: serialization.load_preset) into a device of the
        same kind (loads_into), which stays where it is, with its id, on/off
        switch and sidechain: a plug-in takes the preset's state, a built-in
        device its parameters and state, a rack its chains (new devices) and
        macros. One undo step; False if it can't (another kind of device, or a
        rack that would nest too deep there). A plug-in's state before is the
        model's: store it first (EngineBridge.store_plugin_states)."""
        p = self.project
        device = p.device(track_id, device_id)
        if not loads_into(preset, device):
            return False
        if device.is_rack:
            before = copy.deepcopy(p.track(track_id).devices)
            if rack_depth(before, container_of(before, device_id)) + rack_height(preset) > MAX_RACK_DEPTH:
                return False
            after = copy.deepcopy(before)
            rack = find_device(after, device_id)
            rack.chains, rack.macros, rack.params = copy.deepcopy(preset.chains), preset.macros, dict(preset.params)
            rack.name = preset.name
            self._set_devices(track_id, before, after, text)
            return True
        commands = []
        if not device.is_plugin:  # (a plug-in's parameters are in its state)
            new = {(device_id, k): v for k, v in preset.params.items()}
            old = {}
            for key, value in new.items():
                own = device.params.get(key[1])
                if own is None:  # (a parameter the device has no value for: at its default)
                    info = builtin_param_info(device.kind, key[1])
                    own = value if info is None else info.default_value
                old[key] = own
            if old != new:
                commands.append(SetDeviceParamsCommand(p, track_id, old, new, text))
        if preset.state != device.state and (preset.state is not None or not device.is_plugin):
            commands.append(SetDeviceStateCommand(p, track_id, device_id, device.state, preset.state, text))
        if len(commands) == 1:
            self._push(commands[0])
        elif commands:
            self.undo_stack.beginMacro(text)
            for command in commands:
                self._push(command)
            self.undo_stack.endMacro()
        return True

    def rename_rack(self, track_id: str, device_id: str, name: str | None, text: str = "Rename Rack") -> None:
        """A rack's name (None or "": named by its kind again)."""
        device = self.project.device(track_id, device_id)
        name = name or None
        if device.is_rack and device.name != name:
            self._push(SetDeviceNameCommand(self.project, track_id, device_id, device.name, name, text))

    def set_device_enabled(self, track_id: str, device_id: str, enabled: bool) -> None:
        if self.project.device(track_id, device_id).enabled != enabled:
            self._push(SetDeviceEnabledCommand(self.project, track_id, device_id, enabled))

    def set_device_sidechain(self, track_id: str, device_id: str, sidechain: Sidechain | None) -> None:
        """What a device's sidechain (aux) input hears: a track's (a group's, a
        return's) signal, after its fader, before it, or after one of its devices
        (Sidechain); None: nothing. Raises ValueError for a source it can't take
        (the master, its own track, or one its track feeds: a cycle) or a tap
        after a device that isn't on the source."""
        p = self.project
        device = p.device(track_id, device_id)
        if sidechain is not None:
            source = sidechain.track_id
            if not (p.has_track(source) or p.has_return(source)) or p.sidechain_would_cycle(track_id, source):
                raise ValueError(f"{device_name(device)} can't take its sidechain from that track")
            if sidechain.tap_device is not None and not any(d.id == sidechain.tap for d in p.track(source).devices):
                raise ValueError("a sidechain can only be taken after one of its source's devices")
        if sidechain != device.sidechain:
            text = "Remove Sidechain" if sidechain is None else "Change Sidechain"
            self._push(SetDeviceSidechainCommand(p, track_id, device_id, device.sidechain, sidechain, text))

    # --- Automation ---------------------------------------------------------------
    # Envelopes are normalized (see automation.py); an owner is a track id or MASTER.

    def set_envelope(self, owner: str, key: str, points, text: str = "Change Automation",
                     merge_key: object | None = None) -> None:
        new = automation.normalize(points)
        old = self.project.envelope(owner, key)
        if new != old:
            self._push(SetEnvelopeCommand(self.project, owner, key, old, new, text, merge_key))

    def add_automation_point(self, owner: str, key: str, beat: float, value: float,
                             merge_key: object | None = None) -> int:
        """A new breakpoint; returns its index. With a `merge_key`, dragging it
        right away (move_automation_points with the same key) is the same undo step."""
        points, index = automation.add_point(self.project.envelope(owner, key), beat, value)
        self.set_envelope(owner, key, points, "Add Automation Point", merge_key)
        return index

    def move_automation_points(self, owner: str, key: str, original: Envelope, indices, delta_beats: float,
                               delta_value: float, merge_key: object | None = None) -> dict[int, int]:
        """Move points of `original` (the envelope when the drag began) together.
        Returns where they are now ({index in `original`: index})."""
        points, where = automation.move_points_mapped(original, indices, delta_beats, delta_value)
        self.set_envelope(owner, key, points, "Move Automation", merge_key)
        return where

    def delete_automation_points(self, owner: str, key: str, indices) -> None:
        points = automation.delete_points(self.project.envelope(owner, key), indices)
        self.set_envelope(owner, key, points, "Delete Automation Point")

    def set_automation_curve(self, owner: str, key: str, original: Envelope, index: int, curve: float,
                             merge_key: object | None = None) -> None:
        self.set_envelope(owner, key, automation.set_curve(original, index, curve), "Change Automation Curve",
                          merge_key)

    def clear_envelope(self, owner: str, key: str) -> None:
        self.set_envelope(owner, key, (), "Delete Envelope")

    def _each_lane(self, text: str, lanes, change) -> None:
        """`change(envelope)` on each lane, as one undo step."""
        changed = [(owner, key, change(self.project.envelope(owner, key))) for owner, key in dict.fromkeys(lanes)
                   if self.project.has_owner(owner)]
        changed = [(o, k, new) for o, k, new in changed if new != self.project.envelope(o, k)]
        if not changed:
            return
        self.undo_stack.beginMacro(text)
        for owner, key, new in changed:
            self.set_envelope(owner, key, new, text)
        self.undo_stack.endMacro()

    def delete_automation_range(self, start: float, end: float, lanes: list[LaneRef]) -> None:
        """Delete the automation between two beats on these lanes."""
        self._each_lane("Delete Automation", lanes, lambda points: automation.remove_range(points, start, end))

    def move_automation_range(self, start: float, end: float, originals: dict[LaneRef, Envelope],
                              delta_beats: float, delta_value: float, merge_key: object | None = None) -> None:
        """Move the automation between two beats on these lanes (`originals`: their
        envelopes when the drag began) in time and value, as one undo step."""
        lanes = {lane: points for lane, points in originals.items() if points and self.project.has_owner(lane[0])}
        new = {lane: automation.move_range(points, start, end, delta_beats, delta_value)
               for lane, points in lanes.items()}
        current = {lane: self.project.envelope(*lane) for lane in new}
        if new != current:
            self._push(SetEnvelopesCommand(self.project, current, new, "Move Automation", merge_key))

    def duplicate_automation_range(self, start: float, end: float, lanes: list[LaneRef]) -> None:
        """Copy the automation between two beats to right after `end`, over what was there."""
        def duplicate(points: Envelope) -> Envelope:
            if not points:
                return points
            return automation.paste_range(points, automation.copy_range(points, start, end), end, end - start)
        self._each_lane("Duplicate Automation", lanes, duplicate)

    def copy_automation_range(self, start: float, end: float, lanes) -> CopiedAutomation | None:
        """Ctrl+C on a lane range: the automation between two beats on these lanes
        (those that have any). None if none of them has."""
        if end <= start:
            return None
        copied = tuple((lane, automation.copy_range(self.project.envelope(*lane), start, end))
                       for lane in dict.fromkeys(lanes) if self.project.has_owner(lane[0]))
        copied = tuple((lane, points) for lane, points in copied if points)
        return CopiedAutomation(end - start, copied) if copied else None

    def cut_automation_range(self, start: float, end: float, lanes) -> CopiedAutomation | None:
        """Ctrl+X on a lane range: copy it (copy_automation_range), then delete it. One undo step."""
        content = self.copy_automation_range(start, end, lanes)
        if content is not None:
            self._each_lane("Cut Automation", [lane for lane, _ in content.lanes],
                            lambda points: automation.remove_range(points, start, end))
        return content

    def automation_paste_targets(self, content: CopiedAutomation, lanes=()) -> list[LaneRef | None]:
        """Where each copied lane goes: onto `lanes` (the selected ones, in order)
        one to one if there are as many, or one copied lane onto each of them;
        otherwise onto the lanes it was copied from. None for a lane that is gone
        (its owner, or the device or send it automates)."""
        lanes = list(dict.fromkeys(lanes))
        if lanes and len(content.lanes) in (1, len(lanes)):
            targets = lanes
        else:
            targets = [lane for lane, _ in content.lanes]
        return [lane if self._lane_exists(lane) else None for lane in targets]

    def _lane_exists(self, lane: LaneRef) -> bool:
        owner, key = lane
        if not self.project.has_owner(owner) or not automation.is_key(key):
            return False
        device = automation.key_device(key)
        if device is not None:
            return self.project.has_device(owner, device)
        send = automation.key_send(key)
        return send is None or send in self.project.track(owner).sends

    def paste_automation(self, content: CopiedAutomation, at_beat: float, lanes=()) -> list[LaneRef]:
        """Ctrl+V: copied automation at `at_beat`, replacing what is there, onto the
        lanes automation_paste_targets picks. One undo step; the lanes pasted onto."""
        at = max(0.0, at_beat)
        targets = self.automation_paste_targets(content, lanes)
        sources = [points for _lane, points in content.lanes]
        if len(sources) == 1:
            sources *= len(targets)
        pasted = {lane: points for lane, points in zip(targets, sources, strict=True) if lane is not None}
        edges = (at, at + content.length)
        current = {lane: self.project.envelope(*lane) for lane in pasted}
        new = {lane: automation.drop_redundant(automation.paste_range(current[lane], points, at, content.length),
                                               edges) for lane, points in pasted.items()}
        changed = {lane: points for lane, points in new.items() if points != current[lane]}
        if changed:
            self._push(SetEnvelopesCommand(self.project, {lane: current[lane] for lane in changed}, changed,
                                           "Paste Automation"))
        return list(pasted)

    def set_automation_locked(self, locked: bool) -> None:
        """Lock Envelopes: whether automation stays in place when clips move
        (unlocked, it moves with them). A setting, not an edit: not undoable."""
        if locked != self.project.automation_locked:
            self.project.update_settings(automation_locked=locked)

    # View state: what the arrangement shows of each owner's automation. Saved with
    # the project, but not undoable (like track heights).

    def _update_view(self, owner: str, **changes) -> None:
        view = self.project.automation_view(owner)
        new = replace(view, **changes)
        if new != view:
            self.project.set_automation_view(owner, new)

    def default_automation_key(self, owner: str) -> str:
        """What a lane shows when nothing was chosen: the first automated target, else the volume."""
        return next(iter(self.project.automation(owner)), MIXER_VOLUME)

    def show_automation(self, owner: str, key: str | None = None) -> None:
        """Show an owner's automation, with `key` (if given) in its main lane."""
        view = self.project.automation_view(owner)
        self._update_view(owner, shown=True, key=key or view.key or self.default_automation_key(owner))

    def hide_automation(self, owner: str) -> None:
        self._update_view(owner, shown=False)

    def toggle_all_automation(self) -> bool:
        """Show every track's (and the master's) automation, or hide it all if all
        of it shows. Returns whether it shows now."""
        owners = self.project.owners()
        show = not all(self.project.automation_view(o).shown for o in owners)
        for owner in owners:
            if show:
                self.show_automation(owner)
            else:
                self.hide_automation(owner)
        return show

    def add_automation_lane(self, owner: str) -> None:
        """Another lane below the owner's main lane: the first automated target not
        shown yet (else the first mixer control not shown)."""
        view = self.project.automation_view(owner)
        shown = {view.key, *view.lanes}
        candidates = [*self.project.automation(owner), MIXER_VOLUME, MIXER_PAN]
        key = next((k for k in candidates if k not in shown), candidates[0])
        self._update_view(owner, shown=True, key=view.key or self.default_automation_key(owner),
                          lanes=view.lanes + (key,))

    def set_automation_lane(self, owner: str, index: int, key: str) -> None:
        """Show `key` in lane `index` (-1: the main lane)."""
        view = self.project.automation_view(owner)
        if index < 0:
            self._update_view(owner, shown=True, key=key)
        elif index < len(view.lanes):
            self._update_view(owner, lanes=view.lanes[:index] + (key,) + view.lanes[index + 1:])

    def remove_automation_lane(self, owner: str, index: int) -> None:
        view = self.project.automation_view(owner)
        if 0 <= index < len(view.lanes):
            self._update_view(owner, lanes=view.lanes[:index] + view.lanes[index + 1:])

    def reset_automation_view(self, owner: str) -> None:
        self._update_view(owner, **vars(AutomationView()))
