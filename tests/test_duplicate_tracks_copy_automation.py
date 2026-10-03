"""Duplicating tracks (Ctrl+D on tracks: new clips and devices, automation and
routing following the copies, one undo step) and cutting, copying and pasting
automation (Ctrl+C/X/V on a lane range: onto the lanes selected, or those it
came from)."""

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio.model import automation
from gilstudio.model.automation import MIXER_PAN, MIXER_VOLUME, AutomationPoint
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import Project, Sidechain, iter_devices


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def env(*points):
    return tuple(AutomationPoint(b, v) for b, v in points)


# --- Duplicating tracks ----------------------------------------------------------------------


def test_duplicated_tracks_are_new_tracks_like_them(editor):
    p = editor.project
    source = editor.add_audio_track()
    track = editor.add_midi_track()
    clip = editor.add_midi_clip(track.id, 0.0, 4.0)
    synth = p.track(track.id).devices[0]
    utility = editor.add_device(track.id, "utility")
    rack = editor.group_devices(track.id, [utility.id])
    chain = rack.chains[0]
    editor.set_device_sidechain(track.id, synth.id, Sidechain(source.id))
    editor.set_envelope(track.id, MIXER_VOLUME, env((0.0, 0.2), (4.0, 0.8)))
    editor.set_envelope(track.id, automation.device_key(utility.id, "gain"), env((0.0, 0.5)))
    editor.set_envelope(track.id, automation.chain_key(rack.id, chain.id, "volume"), env((0.0, 0.3)))
    editor.show_automation(track.id, automation.device_key(utility.id, "gain"))
    editor.set_devices_folded(track.id, [utility.id], True)
    p.update_track(track.id, armed=True)
    after = editor.add_audio_track()

    steps = editor.undo_stack.count()
    [copy] = editor.duplicate_tracks([track.id])
    assert editor.undo_stack.count() == steps + 1 and editor.undo_stack.undoText() == "Duplicate Track"
    assert [t.id for t in p.tracks] == [source.id, track.id, copy.id, after.id]  # right after it
    assert copy.id != track.id and copy.name != track.name and copy.kind == "midi" and not copy.armed
    assert len(copy.clips) == 1 and copy.clips[0].id != clip[1] and copy.clips[0].notes == p.clip(*clip).notes
    old = {d.id for d in iter_devices(p.track(track.id).devices)}
    new = [d.id for d in iter_devices(copy.devices)]
    assert len(new) == len(old) and not old & set(new)
    copied_synth, copied_rack = copy.devices
    copied_utility = copied_rack.chains[0].devices[0]
    assert copied_synth.sidechain == Sidechain(source.id)  # from a track not copied: the same
    assert p.is_device_folded(copied_utility.id)
    # Its automation is the copies' devices'.
    gain = automation.device_key(copied_utility.id, "gain")
    assert set(copy.automation) == {MIXER_VOLUME, gain,
                                    automation.chain_key(copied_rack.id, copied_rack.chains[0].id, "volume")}
    assert copy.automation_view.key == gain
    editor.undo_stack.undo()
    assert [t.id for t in p.tracks] == [source.id, track.id, after.id]


def test_duplicating_a_group_copies_what_is_in_it(editor):
    p = editor.project
    a, b, outside = editor.add_audio_track(), editor.add_audio_track(), editor.add_audio_track()
    group = editor.group_tracks([a.id, b.id])
    editor.set_track_input_track(b.id, a.id)  # (b hears a, in the same group)
    editor.add_device(b.id, "utility")
    editor.set_device_sidechain(b.id, p.track(b.id).devices[0].id, Sidechain(a.id))
    editor.set_track_input_track(outside.id, a.id)
    copies = editor.duplicate_tracks([a.id, group.id])  # a goes with its group
    assert len(copies) == 1
    tracks = p.tracks
    assert [t.id for t in tracks[:3]] == [group.id, a.id, b.id] and tracks[-1].id == outside.id
    group_copy, a_copy, b_copy = tracks[3:6]
    assert group_copy.id == copies[0].id and group_copy.is_group and group_copy.parent is None
    assert a_copy.parent == b_copy.parent == group_copy.id
    # Routing among the copied tracks goes among the copies.
    assert b_copy.input_track == a_copy.id and b_copy.devices[0].sidechain == Sidechain(a_copy.id)
    assert p.track(outside.id).input_track == a.id
    editor.undo_stack.undo()
    assert len(p.tracks) == 4


# --- Copying automation ----------------------------------------------------------------------


def test_cut_copy_and_paste_automation(editor):
    p = editor.project
    a, b = editor.add_audio_track(), editor.add_audio_track()
    editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (2.0, 1.0), (4.0, 0.0)))
    editor.set_envelope(a.id, MIXER_VOLUME, env((0.0, 0.5)))
    assert editor.copy_automation_range(1.0, 3.0, [(b.id, MIXER_PAN)]) is None  # nothing there

    content = editor.copy_automation_range(1.0, 3.0, [(a.id, MIXER_PAN), (b.id, MIXER_PAN)])
    assert content.length == 2.0 and [lane for lane, _ in content.lanes] == [(a.id, MIXER_PAN)]
    # Onto the lane it came from, later on.
    assert editor.paste_automation(content, 6.0) == [(a.id, MIXER_PAN)]
    pan = p.envelope(a.id, MIXER_PAN)
    assert automation.value_at(pan, 7.0) == pytest.approx(1.0)
    assert automation.value_at(pan, 6.0) == pytest.approx(0.5)
    assert editor.undo_stack.undoText() == "Paste Automation"
    # One lane copied: onto each lane selected.
    editor.paste_automation(content, 0.0, [(b.id, MIXER_PAN), (b.id, MIXER_VOLUME)])
    assert automation.value_at(p.envelope(b.id, MIXER_PAN), 1.0) == pytest.approx(1.0)
    assert automation.value_at(p.envelope(b.id, MIXER_VOLUME), 1.0) == pytest.approx(1.0)
    editor.undo_stack.undo()
    assert not p.envelope(b.id, MIXER_PAN) and not p.envelope(b.id, MIXER_VOLUME)

    # Two lanes: one to one onto two selected; onto another number, where they came from.
    two = editor.copy_automation_range(0.0, 4.0, [(a.id, MIXER_VOLUME), (a.id, MIXER_PAN)])
    editor.paste_automation(two, 0.0, [(b.id, MIXER_PAN), (b.id, MIXER_VOLUME)])
    assert automation.value_at(p.envelope(b.id, MIXER_PAN), 2.0) == pytest.approx(0.5)  # a's volume
    assert automation.value_at(p.envelope(b.id, MIXER_VOLUME), 2.0) == pytest.approx(1.0)  # a's pan
    assert editor.automation_paste_targets(two, [(b.id, MIXER_PAN)]) == [(a.id, MIXER_VOLUME), (a.id, MIXER_PAN)]

    # Cut: copied, and gone from the range, as one step.
    steps = editor.undo_stack.count()
    cut = editor.cut_automation_range(1.0, 3.0, [(a.id, MIXER_PAN)])
    assert cut == content and editor.undo_stack.count() == steps + 1
    assert automation.value_at(p.envelope(a.id, MIXER_PAN), 2.0) == pytest.approx(0.5)  # straight across

    # A lane whose device is gone takes nothing.
    utility = editor.add_device(a.id, "utility")
    gain = automation.device_key(utility.id, "gain")
    editor.set_envelope(a.id, gain, env((0.0, 0.25)))
    copied = editor.copy_automation_range(0.0, 1.0, [(a.id, gain)])
    editor.remove_device(a.id, utility.id)
    assert editor.paste_automation(copied, 0.0) == []


# --- In the window ---------------------------------------------------------------------------


def test_ctrl_d_duplicates_the_selected_tracks(window):
    window.insert_track()
    window.insert_midi_track()
    first, second = window.project.tracks[-2:]
    window.selection.select_track(first.id, focus_track=True)
    window.selection.select_track(second.id, focus_track=True, mode="toggle")
    window.duplicate()
    tracks = window.project.tracks
    assert [t.id for t in tracks[-4:-2]] == [first.id, second.id]
    copies = tracks[-2:]
    assert [t.kind for t in copies] == ["audio", "midi"]
    assert window.bridge.engine_device_id(copies[1].id, copies[1].devices[0].id) is not None  # its synth plays
    assert set(window.selection.track_ids) == {t.id for t in copies} and window.selection.focus == "track"
    window.undo_stack.undo()
    assert [t.id for t in window.project.tracks[-2:]] == [first.id, second.id]


def test_ctrl_c_and_ctrl_v_copy_automation_between_lanes(window):
    window.insert_track()
    window.insert_track()
    a, b = window.project.tracks[-2:]
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (2.0, 1.0), (4.0, 0.0)))
    selection = window.selection
    selection.set_time_range(1.0, 3.0, [a.id], lanes=[(a.id, MIXER_PAN)])
    window.copy()
    assert window.arrangement.lanes.clipboard.lanes[0][0] == (a.id, MIXER_PAN)
    # Select a range on b's lane: the copy goes there, at its start, and is selected.
    selection.set_time_range(4.0, 5.0, [b.id], lanes=[(b.id, MIXER_VOLUME)])
    selection.set_insert(4.0)
    window.paste()
    assert automation.value_at(window.project.envelope(b.id, MIXER_VOLUME), 5.0) == pytest.approx(1.0)
    assert selection.time_range == (4.0, 6.0, (b.id,)) and selection.lanes == ((b.id, MIXER_VOLUME),)
    assert selection.insert_beat == 6.0
    # Cut: out of the range, which stays selected.
    selection.set_time_range(1.0, 3.0, [a.id], lanes=[(a.id, MIXER_PAN)])
    window.cut()
    assert automation.value_at(window.project.envelope(a.id, MIXER_PAN), 2.0) == pytest.approx(0.5)
    assert selection.lanes == ((a.id, MIXER_PAN),)
    # Breakpoints aren't a range: nothing is copied.
    selection.select_points(a.id, MIXER_PAN, {0})
    window.copy()
    assert window.arrangement.lanes.clipboard.lanes[0][0] == (a.id, MIXER_PAN)
