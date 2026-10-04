"""Freezing in the model: what can be frozen, freezing and unfreezing (undoable),
what a frozen track (and what is in a frozen group) refuses, flattening, and
saving; and through the bridge, a frozen track playing its frozen audio with its
devices unloaded, and coming back as it was."""

import json
from pathlib import Path

import numpy as np
import pytest
from PySide6.QtGui import QUndoStack

from substation import _engine as ge
from substation.model import automation
from substation.model.automation import AutomationPoint
from substation.model.editor import ProjectEditor
from substation.model.project import (
    PLUGIN_KIND,
    PRE_FADER,
    PRE_FX,
    Clip,
    Freeze,
    MidiClip,
    Note,
    PluginRef,
    Project,
    Sidechain,
)
from substation.model.serialization import load_into, project_to_dict

from .conftest import SAMPLE_RATE, TEST_PLUGINS

FREEZE = Freeze(path="frozen.wav", duration_sec=2.0, tempo=120.0)


@pytest.fixture
def editor(app):
    e = ProjectEditor(Project(), QUndoStack())
    e.messages = []
    e.refused.connect(e.messages.append)
    return e


def clip(name="c", start=0.0, path="a.wav") -> Clip:
    return Clip(id=name, path=path, name=name, start_beat=start, duration_sec=1.0, source_duration_sec=1.0)


def audio_track(editor, name="A", **kwargs):
    track = editor.add_audio_track(name=name, **kwargs)
    editor._commit("Add", {track.id: [clip(name + "c")]})
    return track.id


# --- What can be frozen ----------------------------------------------------------------


def test_what_can_be_frozen(editor):
    p = editor.project
    a = audio_track(editor, "A")
    b = audio_track(editor, "B")
    assert p.freeze_problem(a) is None
    assert p.freeze_problem(automation.MASTER) is not None
    # A sidechain taking A's signal after its fader or before it: in the frozen audio.
    comp = editor.add_device(b, "compressor")
    editor.set_device_sidechain(b, comp.id, Sidechain(a, PRE_FADER))
    assert p.freeze_problem(a) is None
    # Before its devices (or after one of them): not.
    editor.set_device_sidechain(b, comp.id, Sidechain(a, PRE_FX))
    assert "sidechain" in p.freeze_problem(a)
    editor.set_device_sidechain(b, comp.id, None)
    editor.freeze_tracks({a: FREEZE})
    assert "frozen already" in p.freeze_problem(a)


def test_freezing_and_unfreezing_are_undoable(editor):
    p = editor.project
    a = audio_track(editor)
    editor.arm_tracks([a], True)
    assert editor.freeze_tracks({a: FREEZE}) == [a]
    assert p.track(a).frozen == FREEZE and p.is_frozen(a) and p.frozen_by(a) == a
    assert not p.track(a).armed  # a frozen track doesn't record
    assert editor.undo_stack.undoText() == "Freeze Track"
    editor.undo_stack.undo()
    assert p.track(a).frozen is None
    editor.undo_stack.redo()
    assert editor.unfreeze_tracks([a]) == [a]
    assert p.track(a).frozen is None and editor.undo_stack.undoText() == "Unfreeze Track"
    editor.undo_stack.undo()
    assert p.track(a).frozen == FREEZE


def test_a_frozen_track_plays_its_audio_warped_at_other_tempos():
    played = FREEZE.clip("t", "Track")
    assert played.start_beat == 0.0 and played.path == FREEZE.path and played.duration_sec == 2.0
    assert played.is_warped and played.segment_bpm == 120.0
    assert played.length_beats(60.0) == pytest.approx(4.0)  # its length in beats is fixed


# --- What frozen tracks refuse -----------------------------------------------------------


def test_a_frozen_track_refuses_changes_to_its_clips_devices_and_their_automation(editor):
    p = editor.project
    a = audio_track(editor)
    device = editor.add_device(a, "utility")
    editor.freeze_tracks({a: FREEZE})
    count = editor.undo_stack.count()
    editor.delete_clips([(a, "Ac")])
    editor.add_device(a, "utility")
    editor.remove_device(a, device.id)
    editor.set_device_enabled(a, device.id, False)
    editor.set_device_param(a, device.id, "gain", 0.5)
    editor.set_envelope(a, automation.device_key(device.id, "gain"), [AutomationPoint(0.0, 0.5)])
    assert editor.undo_stack.count() == count
    assert [c.id for c in p.track(a).clips] == ["Ac"] and len(p.track(a).devices) == 1
    assert editor.messages and "frozen" in editor.messages[-1]
    # Its mixer stays live, and so does its automation.
    editor.set_track_param(a, "volume_db", -6.0)
    editor.set_envelope(a, automation.MIXER_VOLUME, [AutomationPoint(0.0, 0.5)])
    assert p.track(a).volume_db == -6.0 and p.envelope(a, automation.MIXER_VOLUME)
    # Nor is it armed.
    editor.arm_tracks([a], True)
    assert not p.track(a).armed


def test_what_is_in_a_frozen_group_cant_change(editor):
    p = editor.project
    a = audio_track(editor, "A")
    b = audio_track(editor, "B")
    outside = audio_track(editor, "C")
    group = editor.group_tracks([a, b])
    ret = editor.add_return_track()
    editor.freeze_tracks({group.id: FREEZE})
    assert p.is_frozen(a) and p.frozen_by(a) == group.id and p.track(a).frozen is None
    assert "frozen" in p.freeze_problem(a)
    count = editor.undo_stack.count()
    editor._commit("Edit", {a: []})  # its clips
    editor.set_envelope(a, automation.MIXER_VOLUME, [AutomationPoint(0.0, 0.5)])  # its mixer's automation: baked
    assert not editor.move_tracks([outside], 1, group.id)  # into the group
    assert not editor.move_tracks([a], len(p.tracks), None)  # out of it
    editor.delete_tracks([a])
    assert editor.undo_stack.count() == count
    assert [t.parent for t in p.tracks[:3]] == [None, group.id, group.id]
    # A send from what is in it to a return outside it stays live.
    editor.set_send(a, ret.id, -6.0)
    assert p.track(a).sends
    # A new track after one in it goes after the group, not into it.
    new = editor.add_audio_track(*editor.insertion_point(b))
    assert new.parent is None and p.track_index(new.id) == 3
    # The group itself goes, with what is in it.
    editor.delete_tracks([group.id])
    assert not p.has_track(a)


def test_ungrouping_a_frozen_group(editor):
    p = editor.project
    a = audio_track(editor, "A")
    group = editor.group_tracks([a])
    editor.freeze_tracks({group.id: FREEZE})
    editor.ungroup([group.id])  # it goes: what was in it plays again
    assert not p.has_track(group.id) and not p.is_frozen(a)


def test_a_group_and_a_track_in_it_freeze_as_the_group(editor):
    p = editor.project
    a = audio_track(editor, "A")
    group = editor.group_tracks([a])
    assert editor.freeze_tracks({group.id: FREEZE, a: FREEZE}) == [group.id]
    assert p.track(a).frozen is None
    assert editor.unfreeze_tracks([a]) == []  # (its group holds it)


# --- Flattening --------------------------------------------------------------------------


def test_flattening_a_frozen_midi_track(editor):
    p = editor.project
    track = editor.add_midi_track(name="Keys")
    synth = track.devices[0]
    editor._commit("Add", {track.id: [MidiClip(id="m", name="m", start_beat=0.0, duration_beats=4.0,
                                               notes=(Note(60, 0.0, 1.0),))]})
    editor.set_envelope(track.id, automation.device_key(synth.id, "volume"), [AutomationPoint(0.0, 0.3)])
    editor.set_envelope(track.id, automation.MIXER_PAN, [AutomationPoint(0.0, 0.2)])
    editor.set_track_param(track.id, "volume_db", -3.0)
    assert p.flatten_problem(track.id) is not None  # not frozen yet
    editor.freeze_tracks({track.id: FREEZE})
    assert editor.flatten_tracks([track.id]) == [track.id]
    flat = p.track(track.id)
    assert flat.is_audio and flat.frozen is None and flat.devices == [] and flat.volume_db == -3.0
    assert len(flat.clips) == 1 and flat.clips[0].path == FREEZE.path and flat.clips[0].is_warped
    assert flat.clips[0].name == "Keys"
    assert list(flat.automation) == [automation.MIXER_PAN]  # its devices' automation went with them
    assert editor.undo_stack.undoText() == "Flatten Track"
    editor.undo_stack.undo()
    back = p.track(track.id)
    assert back.is_midi and back.frozen == FREEZE and back.devices[0].id == synth.id
    assert len(back.automation) == 2


def test_only_frozen_audio_and_midi_tracks_flatten(editor):
    p = editor.project
    a = audio_track(editor)
    group = editor.group_tracks([a])
    editor.freeze_tracks({group.id: FREEZE})
    assert p.flatten_problem(group.id) is not None
    assert editor.flatten_tracks([group.id, a]) == []


# --- Saving ------------------------------------------------------------------------------


def test_frozen_tracks_are_saved(editor, tmp_path):
    p = editor.project
    a = audio_track(editor)
    ret = editor.add_return_track()
    frozen = Freeze(path=str(tmp_path / "Freeze" / "a.wav"), duration_sec=3.5, tempo=98.0)
    editor.freeze_tracks({a: frozen, ret.id: FREEZE})
    data = json.loads(json.dumps(project_to_dict(p, tmp_path / "x.gilproj")))
    assert data["tracks"][0]["frozen"]["relative_path"].replace("\\", "/") == "Freeze/a.wav"
    loaded = Project()
    load_into(loaded, data, tmp_path / "x.gilproj")
    assert loaded.track(a).frozen == frozen and loaded.track(ret.id).frozen == FREEZE


# --- Through the bridge ------------------------------------------------------------------


@pytest.fixture
def bridged(editor, tmp_path, monkeypatch):
    from substation.audio.engine_bridge import EngineBridge

    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    engine = ge.Engine()
    engine.set_clip_fade_ms(0)
    bridge = EngineBridge(engine, editor.project)
    yield editor, bridge, engine
    engine.close_device()
    bridge.shutdown()


def level(engine) -> np.ndarray:
    return engine.render_offline(0.0, 4000)[-100:]


def test_a_frozen_track_plays_its_frozen_audio_without_its_devices(bridged, make_wav, tmp_path):
    editor, bridge, engine = bridged
    p = editor.project
    wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
    engine.load_source(wav)
    track = editor.add_audio_track(name="A")
    editor._commit("Add", {track.id: [clip("c", path=wav)]})
    utility = editor.add_device(track.id, "utility")
    editor.set_device_param(track.id, utility.id, "gain", -6.0206)  # halves it
    bridge.wait_for_device_states()
    level(engine)
    np.testing.assert_allclose(level(engine), 0.25, atol=1e-4)

    freeze = bridge.render_freeze(track.id)
    assert freeze.tempo == p.tempo and freeze.duration_sec >= p.end_beat() / 2
    assert (tmp_path / "Recordings" / "Freeze").exists()
    editor.freeze_tracks({track.id: freeze})
    assert bridge.engine_device_id(track.id, utility.id) is None  # unloaded
    assert engine.track_frozen(bridge._track_ids[track.id])
    np.testing.assert_allclose(level(engine), 0.25, atol=1e-4)
    editor.set_track_param(track.id, "volume_db", -6.0206)  # its fader stays live
    np.testing.assert_allclose(level(engine), 0.125, atol=1e-4)

    editor.unfreeze_tracks([track.id])  # its devices come back as they were
    assert bridge.engine_device_id(track.id, utility.id) is not None
    bridge.wait_for_device_states()
    level(engine)
    np.testing.assert_allclose(level(engine), 0.125, atol=1e-4)

    editor.undo_stack.undo()  # frozen again, then flattened: its frozen audio as a clip
    editor.flatten_tracks([track.id])
    assert p.track(track.id).devices == []
    np.testing.assert_allclose(level(engine), 0.125, atol=1e-4)


def test_a_frozen_midi_track_plays_its_frozen_audio(bridged):
    editor, bridge, engine = bridged
    track = editor.add_midi_track(name="Keys")
    editor._commit("Add", {track.id: [MidiClip(id="m", name="m", start_beat=0.0, duration_beats=2.0,
                                               notes=(Note(60, 0.0, 1.0),))]})
    before = engine.render_offline(0.0, SAMPLE_RATE)
    assert np.abs(before).max() > 0.01
    editor.freeze_tracks({track.id: bridge.render_freeze(track.id)})
    np.testing.assert_allclose(engine.render_offline(0.0, SAMPLE_RATE), before, atol=1e-5)
    editor.unfreeze_tracks([track.id])  # its notes again, not its frozen audio as well
    np.testing.assert_allclose(engine.render_offline(0.0, SAMPLE_RATE), before, atol=1e-5)


def test_nothing_to_freeze(bridged):
    editor, bridge, _engine = bridged
    track = editor.add_audio_track()
    with pytest.raises(ValueError, match="nothing to freeze"):
        bridge.render_freeze(track.id)


@pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")
def test_a_plugin_comes_back_as_it_was(bridged, dc_wav):
    editor, bridge, engine = bridged
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "SUB Test Effect")
    ref = PluginRef(format="VST3", uid=uid, name="SUB Test Effect", path=str(TEST_PLUGINS))
    engine.load_source(dc_wav)
    track = editor.add_audio_track(name="A")
    editor._commit("Add", {track.id: [clip("c", path=dc_wav)]})
    effect = editor.add_device(track.id, PLUGIN_KIND, plugin=ref)
    processor = bridge.engine_device_id(track.id, effect.id)
    engine.set_processor_param(processor, 0, 0.25)  # as if in its own editor: only the plug-in knows
    editor.freeze_tracks({track.id: bridge.render_freeze(track.id)})
    assert bridge.engine_device_id(track.id, effect.id) is None
    assert editor.project.track(track.id).devices[0].state  # (stored with the project meanwhile)
    editor.unfreeze_tracks([track.id])
    processor = bridge.engine_device_id(track.id, effect.id)
    assert processor is not None and engine.processor_param(processor, 0) == pytest.approx(0.25)


def test_automation_doesnt_move_without_the_frozen_clips_under_it(editor):
    p = editor.project
    a = audio_track(editor)
    editor.set_envelope(a, automation.MIXER_VOLUME, [AutomationPoint(0.5, 0.2), AutomationPoint(1.5, 0.8)])
    editor.freeze_tracks({a: FREEZE})
    envelope = p.envelope(a, automation.MIXER_VOLUME)
    editor.move_clips([(a, "Ac")], 4.0)  # (its mixer's automation would go with them)
    assert p.track(a).clips[0].start_beat == 0.0 and p.envelope(a, automation.MIXER_VOLUME) == envelope
    assert "frozen" in editor.messages[-1]


def test_renders_no_track_plays_are_deleted(app, bridged, dc_wav, monkeypatch):
    from substation.ui import freezing

    editor, bridge, engine = bridged
    engine.load_source(dc_wav)
    tracks = []
    for name in ("A", "B"):
        track = editor.add_audio_track(name=name)
        editor._commit("Add", {track.id: [clip(name + "c", path=dc_wav)]})
        tracks.append(track.id)
    rendered = []
    finish = bridge.finish_freeze

    def failing(render):
        if render.track_id == tracks[1]:
            render.job.cancel()
            render.job.finish()
            raise RuntimeError("Could not write it")
        rendered.append(finish(render))
        return rendered[-1]

    monkeypatch.setattr(bridge, "finish_freeze", failing)
    assert freezing.freeze_tracks(editor, bridge, tracks) == []
    assert len(rendered) == 1 and not Path(rendered[0].path).exists()  # A's render, played by no track
    assert editor.project.track(tracks[0]).frozen is None and "could not be frozen" in editor.messages[-1]
