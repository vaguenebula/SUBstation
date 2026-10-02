"""Return tracks and sends in the model: making and deleting returns (with the
sends into them, as one undo step), sends' levels and taps, cycles among
returns, saving; and the engine hearing them (through the bridge)."""

import json

import numpy as np
import pytest
from PySide6.QtGui import QUndoStack

from gilstudio import _engine as ge
from gilstudio.model import automation
from gilstudio.model.automation import AutomationPoint
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import Clip, Project, Send, return_letter
from gilstudio.model.serialization import load_into, project_to_dict

from .conftest import SAMPLE_RATE


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def test_return_letters():
    assert [return_letter(i) for i in (0, 1, 25, 26, 27, 52)] == ["A", "B", "Z", "AA", "AB", "BA"]


def test_returns_are_tracks_apart_from_the_arrangement(editor):
    p = editor.project
    track = editor.add_audio_track(name="Vox")
    a = editor.add_return_track()
    b = editor.add_return_track()
    assert [r.name for r in p.returns] == ["A Return", "B Return"]
    assert a.is_return and not a.has_input and p.tracks == [track]
    assert p.track(a.id) is a and p.has_owner(a.id) and not p.has_track(a.id)
    assert p.return_letter(b.id) == "B"
    assert p.owners() == [track.id, a.id, b.id, automation.MASTER]
    assert editor.undo_stack.undoText() == "Insert Return Track"
    editor.undo_stack.undo()
    assert [r.id for r in p.returns] == [a.id]


def test_sends_and_their_undo(editor):
    p = editor.project
    track = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.set_send(track.id, ret.id, -12.0)
    assert p.track(track.id).sends == {ret.id: Send(-12.0, False)}
    editor.set_send(track.id, ret.id, pre_fader=True)
    assert p.track(track.id).sends == {ret.id: Send(-12.0, True)}
    assert editor.undo_stack.undoText() == "Toggle Pre-Fader Send"
    # A knob drag is one undo step.
    for level in (-10.0, -8.0, -6.0):
        editor.set_send(track.id, ret.id, level, merge_key="drag")
    assert p.track(track.id).sends[ret.id].level_db == -6.0
    editor.undo_stack.undo()
    assert p.track(track.id).sends[ret.id] == Send(-12.0, True)
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert p.track(track.id).sends == {}
    editor.set_send(track.id, ret.id, 20.0)  # held to the fader's range
    assert p.track(track.id).sends[ret.id].level_db == automation.MAX_VOLUME_DB
    editor.remove_send(track.id, ret.id)
    assert p.track(track.id).sends == {}


def test_cycles_among_returns_are_refused(editor):
    p = editor.project
    a, b, c = (editor.add_return_track() for _ in range(3))
    group = editor.add_audio_track()
    editor.set_send(a.id, b.id, 0.0)
    editor.set_send(b.id, c.id, 0.0)
    assert p.would_cycle(c.id, a.id) and p.would_cycle(a.id, a.id)
    assert not p.would_cycle(a.id, c.id) and not p.would_cycle(group.id, a.id)
    assert [r.id for r in p.send_targets(c.id)] == []
    assert [r.id for r in p.send_targets(b.id)] == [c.id]
    assert [r.id for r in p.send_targets(group.id)] == [a.id, b.id, c.id]
    for frm, to in ((c.id, a.id), (b.id, a.id), (a.id, a.id), (group.id, group.id), (automation.MASTER, a.id)):
        with pytest.raises(ValueError):
            editor.set_send(frm, to, 0.0)


def test_deleting_a_return_takes_the_sends_to_it_along(editor):
    p = editor.project
    track = editor.add_audio_track()
    a, b = editor.add_return_track(), editor.add_return_track()
    editor.set_send(track.id, a.id, -3.0)
    editor.set_send(track.id, b.id, -6.0)
    editor.set_send(b.id, a.id, 0.0)
    key = automation.send_key(a.id)
    editor.set_envelope(track.id, key, [AutomationPoint(0.0, 0.5)])
    editor.delete_tracks([a.id])
    assert [r.id for r in p.returns] == [b.id]
    assert p.track(track.id).sends == {b.id: Send(-6.0)}
    assert p.track(b.id).sends == {}
    assert key not in p.track(track.id).automation
    assert editor.undo_stack.undoText() == "Delete Return Track"
    editor.undo_stack.undo()  # one step brings it all back
    assert [r.id for r in p.returns] == [a.id, b.id]
    assert p.track(track.id).sends == {a.id: Send(-3.0), b.id: Send(-6.0)}
    assert p.track(b.id).sends == {a.id: Send(0.0)}
    assert p.envelope(track.id, key) == (AutomationPoint(0.0, 0.5),)
    # With tracks: one step too.
    editor.delete_tracks([track.id, b.id])
    assert p.tracks == [] and [r.id for r in p.returns] == [a.id]
    assert editor.undo_stack.undoText() == "Delete Tracks"


def test_solo_takes_returns_in(editor):
    p = editor.project
    track = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.solo_tracks([track.id], True)
    editor.solo_tracks([ret.id], True, exclusive=True)  # the others are unsoloed, the tracks too
    assert p.track(ret.id).solo and not p.track(track.id).solo


def test_returns_and_sends_are_saved_and_loaded(editor):
    p = editor.project
    track = editor.add_audio_track()
    group = editor.group_tracks([track.id])
    a, b = editor.add_return_track(), editor.add_return_track()
    editor.set_send(track.id, a.id, -6.0, pre_fader=True)
    editor.set_send(group.id, b.id, -3.0)
    editor.set_send(a.id, b.id, -1.5)
    editor.set_track_param(b.id, "volume_db", -2.0)
    editor.set_envelope(track.id, automation.send_key(a.id), [AutomationPoint(1.0, 0.25)])
    data = json.loads(json.dumps(project_to_dict(p)))
    loaded = Project()
    load_into(loaded, data)
    assert [(r.id, r.name, r.kind) for r in loaded.returns] == [(r.id, r.name, r.kind) for r in p.returns]
    assert loaded.track(track.id).sends == {a.id: Send(-6.0, True)}
    assert loaded.track(group.id).sends == {b.id: Send(-3.0)}
    assert loaded.track(a.id).sends == {b.id: Send(-1.5)}
    assert loaded.track(b.id).volume_db == -2.0
    assert loaded.envelope(track.id, automation.send_key(a.id)) == (AutomationPoint(1.0, 0.25),)
    # Sends a project can't have (the file was edited) are dropped: to a return
    # that isn't there, and the one closing a cycle.
    data["returns"][1]["sends"] = {a.id: {"level_db": 0.0, "pre_fader": False}}
    data["tracks"][1]["sends"]["gone"] = {"level_db": 0.0}
    load_into(loaded, data)
    assert loaded.track(b.id).sends == {}
    assert loaded.track(track.id).sends == {a.id: Send(-6.0, True)}
    # Older files have neither.
    del data["returns"]
    for t in data["tracks"]:
        del t["sends"]
    load_into(loaded, data)
    assert loaded.returns == [] and all(t.sends == {} for t in loaded.tracks)


def test_the_engine_hears_returns_and_sends(app, make_wav):
    from gilstudio.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    engine.set_clip_fade_ms(0)
    project = Project()
    stack = QUndoStack()
    editor = ProjectEditor(project, stack)
    bridge = EngineBridge(engine, project)
    try:
        wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
        engine.load_source(wav)
        track = editor.add_audio_track()
        editor._commit("Add", {track.id: [Clip(id="c", path=wav, name="x", start_beat=0.0, duration_sec=1.0,
                                               source_duration_sec=1.0)]})

        def level() -> float:
            return float(engine.render_offline(0.0, 4000)[-1, 0])

        assert level() == pytest.approx(0.5)
        ret = editor.add_return_track()
        assert level() == pytest.approx(0.5)  # no send yet
        editor.set_send(track.id, ret.id, -6.0206)
        assert level() == pytest.approx(0.75, abs=1e-3)
        editor.set_track_param(track.id, "volume_db", -70.0)  # silent: only a pre-fader send is heard
        assert level() == pytest.approx(0.0)
        editor.set_send(track.id, ret.id, pre_fader=True)
        assert level() == pytest.approx(0.25, abs=1e-3)
        other = editor.add_return_track()
        editor.set_send(ret.id, other.id, 0.0)
        assert level() == pytest.approx(0.5, abs=1e-3)
        editor.set_track_param(ret.id, "mute", True)
        assert level() == pytest.approx(0.0)
        stack.undo()
        editor.set_track_param(track.id, "volume_db", 0.0)
        assert level() == pytest.approx(1.0, abs=1e-3)  # the track, ret, other
        # Automating a send that wasn't set makes it, silent, so that the automation plays.
        third = editor.add_return_track()
        editor.set_envelope(track.id, automation.send_key(third.id), [AutomationPoint(0.0, 1.0)])
        assert [s.track_id for s in engine.track_sends(bridge._track_ids[track.id])] == [
            bridge._track_ids[ret.id], bridge._track_ids[third.id]]
        assert level() == pytest.approx(1.0 + 0.5 * 10 ** (6 / 20), abs=1e-3)  # +6 dB at the lane's top
        editor.clear_envelope(track.id, automation.send_key(third.id))
        assert level() == pytest.approx(1.0, abs=1e-3)
        # Deleting a return takes the sends into it along; undo brings them back.
        editor.delete_tracks([ret.id])
        assert level() == pytest.approx(0.5)
        assert engine.track_sends(bridge._track_ids[track.id]) == []
        stack.undo()
        assert level() == pytest.approx(1.0, abs=1e-3)
        # A project loaded with returns and sends plays them.
        data = project_to_dict(project)
        load_into(project, data)
        assert level() == pytest.approx(1.0, abs=1e-3)
    finally:
        bridge.shutdown()
        engine.close_device()
