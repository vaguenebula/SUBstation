"""Automation in the model: envelope maths, parameter mappings, undoable edits,
view state and saving."""

import json
import math

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio import _engine as ge
from gilstudio.model import automation as auto
from gilstudio.model.automation import (
    MASTER,
    MIXER_PAN,
    MIXER_VOLUME,
    AutomationPoint,
    AutomationView,
)
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.params import ParamSpec, mixer_specs
from gilstudio.model.project import Project
from gilstudio.model.serialization import load_into, project_to_dict


def env(*points):
    return tuple(AutomationPoint(*p) for p in points)


@pytest.fixture
def editor():
    project = Project()
    return ProjectEditor(project, QUndoStack())


# --- Envelope maths ---------------------------------------------------------------------


def test_the_engine_uses_the_same_curve_and_volume_law():
    assert ge.AUTOMATION_CURVATURE == pytest.approx(auto.CURVATURE)
    assert 20 * math.log10(ge.MAX_VOLUME_GAIN) == pytest.approx(auto.MAX_VOLUME_DB, abs=1e-5)


def test_values_between_before_and_after_the_points():
    points = env((1.0, 0.2), (3.0, 0.6), (3.0, 0.9))  # a ramp, then a step
    assert auto.value_at((), 1.0) is None
    assert auto.value_at(points, 0.0) == 0.2  # holds the first value before it
    assert auto.value_at(points, 2.0) == pytest.approx(0.4)
    assert auto.value_at(points, 3.0) == 0.9  # a step: the later point from its beat on
    assert auto.left_value(points, 3.0) == pytest.approx(0.6)
    assert auto.value_at(points, 10.0) == 0.9


def test_curves_bulge_upward_whichever_way_the_segment_goes():
    rising = env((0.0, 0.0, 0.5), (1.0, 1.0))
    falling = env((0.0, 1.0, 0.5), (1.0, 0.0))
    assert auto.value_at(rising, 0.5) > 0.5
    assert auto.value_at(falling, 0.5) > 0.5
    assert auto.value_at(env((0.0, 0.0, -0.5), (1.0, 1.0)), 0.5) < 0.5
    assert auto.value_at(env((0.0, 0.0, 1.0), (1.0, 1.0)), 1.0) == 1.0


def test_splitting_keeps_curved_segments_exactly():
    points = env((0.0, 0.1, 0.7), (4.0, 0.9, -0.3), (6.0, 0.2))
    for beat in (0.5, 3.9, 4.0, 5.0):
        split, index = auto.split_at(points, beat)
        assert len(split) == 4 and split[index].beat == beat
        for probe in [i / 8 for i in range(56)]:
            assert auto.value_at(split, probe) == pytest.approx(auto.value_at(points, probe), abs=1e-9)


def test_add_point():
    points, index = auto.add_point((), 2.0, 0.5)
    assert points == env((2.0, 0.5)) and index == 0
    points, index = auto.add_point(points, 1.0, 1.5)
    assert index == 0 and points[0] == AutomationPoint(1.0, 1.0)  # clamped
    points, index = auto.add_point(points, 2.0, 0.0)  # a step after the point at the same beat
    assert index == 2 and [p.value for p in points] == [1.0, 0.5, 0.0]


def test_moving_points_keeps_their_order():
    points = env((0.0, 0.5), (1.0, 0.5), (2.0, 0.5), (3.0, 0.5))
    moved = auto.move_points(points, [1], 5.0, 0.25)
    assert moved[1] == AutomationPoint(2.0, 0.75)  # stops at its neighbour
    moved = auto.move_points(points, [1, 2], -3.0, -1.0)
    assert [p.beat for p in moved] == [0.0, 0.0, 1.0, 3.0] and moved[1].value == 0.0
    assert auto.move_points(points, [0], -1.0, 0.0)[0].beat == 0.0  # not before 0


def test_delete_and_curves():
    points = env((0.0, 0.0), (1.0, 1.0), (2.0, 0.0))
    assert auto.delete_points(points, [1]) == env((0.0, 0.0), (2.0, 0.0))
    assert auto.set_curve(points, 0, 2.0)[0].curve == 1.0
    assert auto.segment_index(points, 0.5) == 0 and auto.segment_index(points, 1.5) == 1
    assert auto.segment_index(points, 2.5) is None


def test_remove_range_keeps_the_outside():
    points = env((0.0, 0.0), (1.0, 1.0), (2.0, 0.0), (3.0, 1.0), (4.0, 0.0))
    removed = auto.remove_range(points, 0.5, 2.5)
    for beat in (0.0, 0.25, 0.5, 2.5, 3.0, 3.5):
        assert auto.value_at(removed, beat) == pytest.approx(auto.value_at(points, beat))
    assert auto.value_at(removed, 1.5) == pytest.approx(0.5)  # straight across: 0.5 to 0.5
    assert auto.remove_range(points, -1.0, 5.0) == ()  # every point: no envelope left
    assert auto.remove_range(points, 0.2, 0.8) == points  # no point in it: nothing to delete


def test_duplicate_range():
    points = env((0.0, 0.0), (2.0, 1.0), (4.0, 0.0))
    doubled = auto.paste_range(points, auto.copy_range(points, 0.0, 2.0), 2.0, 2.0)
    for beat in (0.0, 1.0, 1.5):
        assert auto.value_at(doubled, beat + 2.0) == pytest.approx(auto.value_at(points, beat))
        assert auto.value_at(doubled, beat) == pytest.approx(auto.value_at(points, beat))
    assert auto.value_at(doubled, 5.0) == pytest.approx(0.0)


def test_move_range():
    ramp = env((0.0, 0.0), (8.0, 1.0))
    up = auto.move_range(ramp, 2.0, 4.0, 0.0, 0.5)
    assert auto.value_at(up, 1.0) == pytest.approx(0.125) and auto.value_at(up, 5.0) == pytest.approx(0.625)
    assert auto.value_at(up, 3.0) == pytest.approx(0.875)
    assert auto.value_at(up, 2.0) == pytest.approx(0.75) and auto.left_value(up, 2.0) == pytest.approx(0.25)  # a step
    assert auto.move_range(ramp, 2.0, 4.0, 0.0, 0.0) == ramp  # no stray points
    later = auto.move_range(ramp, 2.0, 4.0, 4.0, 0.0)
    assert auto.value_at(later, 7.0) == pytest.approx(0.375) and auto.value_at(later, 1.0) == pytest.approx(0.125)
    assert auto.move_range(ramp, 2.0, 4.0, -10.0, 0.0)[0].beat == 0.0  # not before the start
    assert auto.value_at(auto.move_range(ramp, 2.0, 4.0, 0.0, 2.0), 3.0) == 1.0  # values stay in range


def test_moving_a_range_keeps_the_envelope_outside_it():
    # Every breakpoint is inside the range: the outside still holds its values.
    points = env((1.5, 0.3), (4.0, 0.7))
    moved = auto.move_range(points, 1.0, 4.0, 2.0, -0.2)
    assert auto.value_at(moved, 0.5) == pytest.approx(0.3) and auto.value_at(moved, 7.0) == pytest.approx(0.7)
    assert auto.value_at(moved, 3.5) == pytest.approx(0.1) and auto.value_at(moved, 5.99) == pytest.approx(0.5, abs=0.01)
    # Moved over breakpoints, it replaces them.
    busy = env((0.0, 0.5), (1.0, 0.5), (5.0, 0.9), (5.5, 0.1), (6.0, 0.9), (8.0, 0.5))
    over = auto.move_range(busy, 0.0, 2.0, 5.0, 0.0)
    assert auto.value_at(over, 5.5) == pytest.approx(0.5) and auto.value_at(over, 6.0) == pytest.approx(0.5)
    assert all(p.value in (0.5, 0.6) for p in over if 5.0 < p.beat < 7.0)  # its own, not the 0.9s and 0.1


def test_moving_several_points_overrides_where_they_land():
    points = env((0.0, 0.5), (2.0, 0.2), (3.0, 0.9), (4.0, 0.3), (6.0, 0.5))
    moved, where = auto.move_points_mapped(points, {1, 2}, 1.5, 0.0)
    assert [p.beat for p in moved] == [0.0, 3.5, 4.5, 6.0] and where == {1: 1, 2: 2}
    moved, where = auto.move_points_mapped(points, {1, 2}, 3.0, 0.0)
    assert [p.beat for p in moved] == [0.0, 4.0, 5.0, 6.0, 6.0] and where == {1: 2, 2: 3}  # edges stay
    moved, where = auto.move_points_mapped(points, {1, 2}, -2.0, 0.0)
    assert [(p.beat, p.value) for p in moved[:3]] == [(0.0, 0.5), (0.0, 0.2), (1.0, 0.9)] and where == {1: 1, 2: 2}
    # One point still stops at its neighbours.
    assert [p.beat for p in auto.move_points(points, {1}, 5.0, 0.0)] == [0.0, 3.0, 3.0, 4.0, 6.0]


def test_keys():
    key = auto.device_key("abc", "7:x")
    assert auto.parse_key(key) == ("device", "abc", "7:x") and auto.key_device(key) == "abc"
    assert auto.parse_key(MIXER_VOLUME) == ("mixer", "volume") and auto.key_device(MIXER_PAN) is None
    assert not auto.is_key("mixer:bogus") and not auto.is_key("nothing")


# --- Parameters -------------------------------------------------------------------------


def test_mixer_mappings():
    volume, pan = mixer_specs()
    assert volume.name == "Track Volume" and mixer_specs(master=True)[0].name == "Master Volume"
    assert volume.to_normalized(6.0) == pytest.approx(1.0)
    assert volume.to_normalized(-70.0) == 0.0
    for db in (-40.0, -12.0, 0.0, 3.0):
        assert volume.from_normalized(volume.to_normalized(db)) == pytest.approx(db)
    # The engine's gain for the lane's value is the fader's gain.
    assert (volume.to_normalized(-12.0) ** 3 * ge.MAX_VOLUME_GAIN) == pytest.approx(10 ** (-12 / 20))
    assert volume.format(0.0) == "0.0 dB" and volume.format_normalized(0.0) == "-inf dB"
    assert pan.to_normalized(-1.0) == 0.0 and pan.from_normalized(0.75) == 0.5 and pan.format(0.5) == "25R"


def test_device_parameters_map_as_the_engine_does():
    engine = ge.Engine()
    track = engine.add_track()
    synth = engine.add_builtin_processor(track, "synth")
    for info in engine.processor_params(synth):
        spec = ParamSpec.from_info(info, auto.device_key("d", info.id), "Synth")
        for value in (0.0, 0.1, 0.33, 0.5, 0.74, 0.76, 1.0):
            assert spec.from_normalized(value) == pytest.approx(info.from_normalized(value), rel=1e-5)
        assert spec.to_normalized(info.default_value) == pytest.approx(info.to_normalized(info.default_value),
                                                                       abs=1e-6)
    wave = ParamSpec.from_info(engine.processor_params(synth)[0], "w", "Synth")
    assert wave.discrete and wave.format(2.0) == "Saw" and wave.quantize(0.6) == pytest.approx(2 / 3)
    engine.close_device()


# --- Editing --------------------------------------------------------------------------------


def test_envelope_edits_are_undoable(editor):
    track = editor.add_midi_track()
    changes = []
    editor.project.automation_changed.connect(lambda owner, key: changes.append((owner, key)))
    editor.add_automation_point(track.id, MIXER_VOLUME, 1.0, 0.5)
    index = editor.add_automation_point(track.id, MIXER_VOLUME, 3.0, 0.8)
    assert index == 1 and changes == [(track.id, MIXER_VOLUME)] * 2
    original = editor.project.envelope(track.id, MIXER_VOLUME)
    gesture = object()
    for step in range(1, 5):  # one drag: one undo step
        editor.move_automation_points(track.id, MIXER_VOLUME, original, [1], 0.25 * step, -0.1 * step, gesture)
    assert editor.project.envelope(track.id, MIXER_VOLUME)[1] == AutomationPoint(4.0, pytest.approx(0.4))
    editor.undo_stack.undo()
    assert editor.project.envelope(track.id, MIXER_VOLUME) == original
    editor.set_automation_curve(track.id, MIXER_VOLUME, original, 0, 0.5)
    editor.delete_automation_points(track.id, MIXER_VOLUME, [0, 1])
    assert MIXER_VOLUME not in editor.project.track(track.id).automation  # empty: no automation
    editor.undo_stack.undo()
    assert editor.project.envelope(track.id, MIXER_VOLUME)[0].curve == 0.5


def test_range_edits_across_lanes(editor):
    a, b = editor.add_audio_track(), editor.add_audio_track()
    for owner in (a.id, b.id, MASTER):
        editor.set_envelope(owner, MIXER_PAN, env((0.0, 0.0), (2.0, 1.0), (4.0, 0.0)))
    lanes = [(a.id, MIXER_PAN), (MASTER, MIXER_PAN)]
    editor.delete_automation_range(1.0, 3.0, lanes)
    editor.undo_stack.undo()
    editor.undo_stack.redo()
    assert len(editor.project.envelope(a.id, MIXER_PAN)) == 4  # the peak went; edges keep the outside
    assert len(editor.project.envelope(MASTER, MIXER_PAN)) == 4
    assert len(editor.project.envelope(b.id, MIXER_PAN)) == 3
    editor.undo_stack.undo()  # both lanes in one step
    assert len(editor.project.envelope(MASTER, MIXER_PAN)) == 3
    editor.duplicate_automation_range(0.0, 2.0, [(b.id, MIXER_PAN)])
    assert auto.value_at(editor.project.envelope(b.id, MIXER_PAN), 3.0) == pytest.approx(0.5)


def test_deleting_a_device_deletes_its_automation(editor):
    track = editor.add_midi_track()
    synth = track.devices[0]
    utility = editor.add_device(track.id, "utility")
    key = auto.device_key(utility.id, "gain")
    editor.set_envelope(track.id, key, env((0.0, 0.5)))
    editor.set_envelope(track.id, auto.device_key(synth.id, "cutoff"), env((0.0, 0.5)))
    editor.remove_device(track.id, utility.id)
    assert list(editor.project.track(track.id).automation) == [auto.device_key(synth.id, "cutoff")]
    editor.undo_stack.undo()  # the device and its automation come back together
    assert key in editor.project.track(track.id).automation
    assert [d.id for d in editor.project.track(track.id).devices] == [synth.id, utility.id]
    editor.add_device(track.id, "synth")  # replaces the instrument, and its automation goes
    assert list(editor.project.track(track.id).automation) == [key]


def test_touching_a_parameter_is_reported(editor):
    track = editor.add_midi_track()
    touched = []
    editor.parameter_touched.connect(lambda owner, key: touched.append((owner, key)))
    editor.set_track_param(track.id, "volume_db", -3.0)
    editor.set_track_param(track.id, "solo", True)  # not automatable
    editor.set_device_param(track.id, track.devices[0].id, "cutoff", 1000.0)
    editor.set_track_param(MASTER, "pan", 0.5)
    assert touched == [(track.id, MIXER_VOLUME), (track.id, auto.device_key(track.devices[0].id, "cutoff")),
                       (MASTER, MIXER_PAN)]
    assert editor.project.master.pan == 0.5
    assert editor.undo_stack.undoText() == "Change Master Pan"
    editor.undo_stack.undo()
    assert editor.project.master.pan == 0.0
    with pytest.raises(AttributeError):
        editor.set_track_param(MASTER, "mute", True)  # the master is always heard


def test_automation_view(editor):
    a, b = editor.add_audio_track(), editor.add_audio_track()
    editor.set_envelope(b.id, MIXER_PAN, env((0.0, 0.5)))
    editor.show_automation(a.id)
    assert editor.project.automation_view(a.id) == AutomationView(True, MIXER_VOLUME)
    assert editor.toggle_all_automation()  # not all showed: now all do
    assert editor.project.automation_view(b.id).key == MIXER_PAN  # its automated target first
    assert editor.project.automation_view(MASTER).shown
    assert not editor.toggle_all_automation()
    assert not any(editor.project.automation_view(o).shown for o in editor.project.owners())
    assert editor.project.automation_view(b.id).key == MIXER_PAN  # remembered
    editor.add_automation_lane(b.id)
    editor.add_automation_lane(b.id)
    assert editor.project.automation_view(b.id) == AutomationView(True, MIXER_PAN, (MIXER_VOLUME, MIXER_PAN))
    editor.set_automation_lane(b.id, 1, MIXER_VOLUME)
    editor.remove_automation_lane(b.id, 0)
    assert editor.project.automation_view(b.id).lanes == (MIXER_VOLUME,)
    assert editor.undo_stack.count() == 3  # two tracks and the envelope: view changes are not undo steps


def test_save_and_load(editor):
    track = editor.add_midi_track()
    device_key = auto.device_key(track.devices[0].id, "cutoff")
    editor.set_envelope(track.id, device_key, env((0.0, 0.2, 0.3), (2.0, 0.9)))
    editor.set_envelope(MASTER, MIXER_VOLUME, env((1.0, 0.5)))
    editor.set_track_param(MASTER, "pan", -0.25)
    editor.show_automation(track.id, device_key)
    editor.add_automation_lane(track.id)
    editor.set_automation_locked(True)
    data = json.loads(json.dumps(project_to_dict(editor.project)))
    assert data["version"] == 6
    data["tracks"][0]["automation"]["mixer:unknown"] = [[0, 1, 0]]  # from a later version: dropped
    loaded = Project()
    load_into(loaded, data)
    restored = loaded.tracks[0]
    assert restored.automation == editor.project.track(track.id).automation
    assert restored.automation_view == editor.project.track(track.id).automation_view
    assert loaded.master.automation == {MIXER_VOLUME: env((1.0, 0.5))}
    assert loaded.master.pan == -0.25
    assert loaded.automation_locked
    # Projects from before automation load without any.
    del data["tracks"][0]["automation"], data["tracks"][0]["automation_view"], data["master"]["pan"]
    del data["automation_locked"]
    load_into(loaded, data)
    assert loaded.tracks[0].automation == {} and loaded.master.pan == 0.0 and not loaded.automation_locked


# --- Automation moving with clips (unless locked) -----------------------------------------


def two_tracks(editor):
    a = editor.add_midi_track()
    b = editor.add_midi_track()
    clip = editor.add_midi_clip(a.id, 4.0, 4.0)[1]  # under it: pan from 5 to 7, the cutoff's only point
    synth = auto.device_key(a.devices[0].id, "cutoff")
    editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.5), (5.0, 0.0), (7.0, 1.0), (12.0, 0.5)))
    editor.set_envelope(a.id, synth, env((6.0, 0.25)))
    editor.set_envelope(a.id, MIXER_VOLUME, env((0.0, 0.8), (2.0, 0.6)))  # nothing under the clip
    return a, b, clip, synth


def test_moving_a_clip_moves_its_automation(editor):
    a, _, clip, synth = two_tracks(editor)
    steps = editor.undo_stack.count()
    editor.move_range(4.0, 8.0, [a.id], 8.0)
    assert editor.project.clip(a.id, clip).start_beat == 12.0
    pan = editor.project.envelope(a.id, MIXER_PAN)
    # Moved: the stretch under the clip, what it replaced at 12..16, and a straight line where it was.
    assert auto.value_at(pan, 13.0) == pytest.approx(0.0) and auto.value_at(pan, 15.0) == pytest.approx(1.0)
    assert auto.value_at(pan, 5.0) == pytest.approx(0.3)  # (4, 0.1) to (8, 0.9)
    assert auto.value_at(pan, 2.0) == pytest.approx(auto.value_at(two_tracks_pan(), 2.0))
    assert [p.beat for p in editor.project.envelope(a.id, synth)] == [14.0]
    assert editor.project.envelope(a.id, MIXER_VOLUME) == env((0.0, 0.8), (2.0, 0.6))  # untouched
    assert editor.undo_stack.count() == steps + 1
    editor.undo_stack.undo()
    assert editor.project.envelope(a.id, MIXER_PAN) == two_tracks_pan()
    assert editor.project.clip(a.id, clip).start_beat == 4.0


def two_tracks_pan():
    return env((0.0, 0.5), (5.0, 0.0), (7.0, 1.0), (12.0, 0.5))


def test_locked_automation_stays(editor):
    a, _, clip, synth = two_tracks(editor)
    editor.set_automation_locked(True)
    assert editor.undo_stack.count() == 6  # a setting, not an edit
    editor.move_range(4.0, 8.0, [a.id], 8.0)
    editor.duplicate_range(12.0, 16.0, [a.id])
    assert editor.project.envelope(a.id, MIXER_PAN) == two_tracks_pan()
    assert editor.project.envelope(a.id, synth) == env((6.0, 0.25))


def test_copies_of_clips_copy_their_automation(editor):
    a, _, clip, synth = two_tracks(editor)
    editor.duplicate_range(4.0, 8.0, [a.id])  # Ctrl+D
    pan = editor.project.envelope(a.id, MIXER_PAN)
    assert auto.value_at(pan, 5.0) == pytest.approx(0.0) and auto.value_at(pan, 9.0) == pytest.approx(0.0)
    assert auto.value_at(pan, 11.0) == pytest.approx(1.0)
    assert [p.beat for p in editor.project.envelope(a.id, synth)] == [6.0, 10.0]
    editor.move_range(4.0, 8.0, [a.id], 16.0, copy_clips=True)  # Ctrl-drag
    assert editor.project.envelope(a.id, synth) == env((6.0, 0.25), (10.0, 0.25), (22.0, 0.25))


def test_across_tracks_only_the_mixer_automation_goes_along(editor):
    a, b, clip, synth = two_tracks(editor)
    editor.move_range(4.0, 8.0, [a.id], 0.0, track_delta=1)
    assert editor.project.clip(b.id, clip).start_beat == 4.0
    pan = editor.project.envelope(b.id, MIXER_PAN)
    assert auto.value_at(pan, 5.0) == pytest.approx(0.0) and auto.value_at(pan, 7.0) == pytest.approx(1.0)
    assert all(not 4.0 < p.beat < 8.0 for p in editor.project.envelope(a.id, MIXER_PAN))  # gone from a
    assert editor.project.envelope(a.id, synth) == env((6.0, 0.25))  # a device's stays on its track
    assert not editor.project.automation(b.id).keys() - {MIXER_PAN}


def test_moving_clips_by_reference_moves_their_automation(editor):
    a, _, clip, synth = two_tracks(editor)
    editor.move_clips([(a.id, clip)], -4.0)
    assert [p.beat for p in editor.project.envelope(a.id, synth)] == [2.0]
    editor.duplicate_clips([(a.id, clip)])
    assert [p.beat for p in editor.project.envelope(a.id, synth)] == [2.0, 6.0]



def test_the_app_checks_the_engine_matches(monkeypatch):
    import gilstudio

    assert gilstudio.engine_mismatch() is None
    monkeypatch.setattr(ge, "API_VERSION", gilstudio.ENGINE_API - 1)
    assert "older code" in gilstudio.engine_mismatch() and "pip install" in gilstudio.engine_mismatch()
    monkeypatch.delattr(ge, "API_VERSION")  # built before there was one
    assert "older code" in gilstudio.engine_mismatch()
