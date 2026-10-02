"""Sidechains in the model: a device taking a track's (a group's, a return's)
signal into its sidechain input, after its fader, before it or after one of its
devices; cycles refused, and sidechains that would close one dropped when tracks
move into groups or devices to other tracks; a source going takes the
sidechains from it along (one undo step); saving; and the engine taking it all
(through the bridge)."""

import pytest
from PySide6.QtGui import QUndoStack

from gilstudio import _engine as ge
from gilstudio.model.automation import MASTER
from gilstudio.model.editor import ProjectEditor
from gilstudio.model.project import (
    PLUGIN_KIND,
    POST_FADER,
    PRE_FADER,
    PluginRef,
    Project,
    Sidechain,
)
from gilstudio.model.serialization import load_into, project_to_dict

from .conftest import TEST_PLUGINS


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def device(editor, track):
    """A device to take a sidechain (the model doesn't ask whether it has a sidechain input)."""
    return editor.add_device(track.id, "utility")


def test_a_sidechain_and_its_undo(editor):
    p = editor.project
    kick = editor.add_audio_track(name="Kick")
    bass = editor.add_audio_track(name="Bass")
    comp = device(editor, bass)
    tapped = device(editor, kick)
    steps = editor.undo_stack.count()
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id))
    assert p.device(bass.id, comp.id).sidechain == Sidechain(kick.id, POST_FADER)
    assert editor.undo_stack.count() == steps + 1 and editor.undo_stack.undoText() == "Change Sidechain"
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id, PRE_FADER))
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id, tapped.id))
    assert p.device(bass.id, comp.id).sidechain.tap_device == tapped.id
    editor.set_device_sidechain(bass.id, comp.id, None)
    assert p.device(bass.id, comp.id).sidechain is None and editor.undo_stack.undoText() == "Remove Sidechain"
    editor.set_device_sidechain(bass.id, comp.id, None)  # (nothing changes: no step)
    assert editor.undo_stack.count() == steps + 4
    for tap in (tapped.id, PRE_FADER, POST_FADER):
        editor.undo_stack.undo()
        assert p.device(bass.id, comp.id).sidechain == Sidechain(kick.id, tap)
    editor.undo_stack.undo()
    assert p.device(bass.id, comp.id).sidechain is None

    with pytest.raises(ValueError):
        editor.set_device_sidechain(bass.id, comp.id, Sidechain(bass.id))  # its own track
    with pytest.raises(ValueError):
        editor.set_device_sidechain(bass.id, comp.id, Sidechain(MASTER))
    with pytest.raises(ValueError):
        editor.set_device_sidechain(bass.id, comp.id, Sidechain("gone"))
    with pytest.raises(ValueError):
        editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id, comp.id))  # not one of the kick's devices
    on_master = device(editor, p.master)
    editor.set_device_sidechain(MASTER, on_master.id, Sidechain(bass.id))  # the master's devices take any track's
    assert p.master.devices[-1].sidechain == Sidechain(bass.id)


def test_what_can_be_a_source(editor):
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    group = editor.group_tracks([b.id])
    ret = editor.add_return_track()
    editor.set_send(a.id, ret.id, 0.0)
    assert [t.id for t in p.sidechain_sources(b.id)] == [a.id, group.id, ret.id]
    assert [t.id for t in p.sidechain_sources(MASTER)] == [a.id, group.id, b.id, ret.id]
    assert p.sidechain_would_cycle(b.id, group.id)  # what its track goes into
    assert p.sidechain_would_cycle(a.id, ret.id)  # a return it sends to
    assert not p.sidechain_would_cycle(group.id, b.id)  # what goes into the group: fine
    assert not any(p.sidechain_would_cycle(MASTER, t.id) for t in p.sidechain_sources(MASTER))
    with pytest.raises(ValueError):
        editor.set_device_sidechain(b.id, device(editor, b).id, Sidechain(group.id))
    editor.set_device_sidechain(group.id, device(editor, group).id, Sidechain(b.id))


def test_sidechains_are_routing_edges(editor):
    """Sends and inputs that would close a cycle with a sidechain are refused (and greyed out)."""
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.set_device_sidechain(a.id, device(editor, a).id, Sidechain(ret.id))  # ret -> a
    assert p.would_cycle(a.id, ret.id) and ret not in p.send_targets(a.id)
    with pytest.raises(ValueError):
        editor.set_send(a.id, ret.id, 0.0)
    editor.set_device_sidechain(b.id, device(editor, b).id, Sidechain(a.id))  # a -> b
    assert p.input_would_cycle(a.id, b.id)
    with pytest.raises(ValueError):
        editor.set_track_input_track(a.id, b.id)


def test_moving_a_track_into_its_source_drops_the_sidechain(editor):
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    group = editor.group_tracks([a.id])
    comp = device(editor, b)
    editor.set_device_sidechain(b.id, comp.id, Sidechain(group.id))
    editor.set_device_sidechain(group.id, device(editor, group).id, Sidechain(a.id))  # stays: no cycle
    steps = editor.undo_stack.count()
    assert editor.move_tracks([b.id], p.track_index(a.id) + 1, group.id)
    assert b.parent == group.id and p.device(b.id, comp.id).sidechain is None
    assert group.devices[0].sidechain == Sidechain(a.id)
    assert editor.undo_stack.count() == steps + 1 and editor.undo_stack.undoText() == "Move Track"
    editor.undo_stack.undo()
    assert b.parent is None and p.device(b.id, comp.id).sidechain == Sidechain(group.id)


def test_a_source_going_takes_the_sidechains_from_it(editor):
    p = editor.project
    kick = editor.add_audio_track()
    bass = editor.add_audio_track()
    group = editor.group_tracks([bass.id])
    ret = editor.add_return_track()
    comp = device(editor, bass)
    on_master = device(editor, p.master)
    on_return = device(editor, ret)
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id))
    editor.set_device_sidechain(MASTER, on_master.id, Sidechain(kick.id, PRE_FADER))
    editor.set_device_sidechain(ret.id, on_return.id, Sidechain(group.id))
    steps = editor.undo_stack.count()
    editor.delete_tracks([kick.id])
    assert p.device(bass.id, comp.id).sidechain is None and p.device(MASTER, on_master.id).sidechain is None
    assert editor.undo_stack.count() == steps + 1
    editor.undo_stack.undo()
    assert p.device(bass.id, comp.id).sidechain == Sidechain(kick.id)
    assert p.device(MASTER, on_master.id).sidechain == Sidechain(kick.id, PRE_FADER)
    # A group ungrouped is a source no more; what was in it stays, and so do its sidechains.
    editor.ungroup([group.id])
    assert p.device(ret.id, on_return.id).sidechain is None
    assert p.device(bass.id, comp.id).sidechain == Sidechain(kick.id)
    editor.undo_stack.undo()
    assert p.device(ret.id, on_return.id).sidechain == Sidechain(group.id)
    # A device keyed by a track going with it (in its group) keeps its sidechain: they come back together.
    editor.delete_tracks([group.id])
    assert not p.has_track(bass.id) and p.device(ret.id, on_return.id).sidechain is None
    editor.undo_stack.undo()
    assert p.device(bass.id, comp.id).sidechain == Sidechain(kick.id)


def test_moving_a_device_drops_a_sidechain_that_would_close_a_cycle(editor):
    p = editor.project
    kick = editor.add_audio_track()
    bass = editor.add_audio_track()
    pad = editor.add_audio_track()
    comp = device(editor, bass)
    editor.set_device_sidechain(bass.id, comp.id, Sidechain(kick.id))
    assert editor.move_devices_to_track(bass.id, [comp.id], pad.id)
    assert p.device(pad.id, comp.id).sidechain == Sidechain(kick.id)  # it goes with the device
    assert editor.move_devices_to_track(pad.id, [comp.id], MASTER)
    assert p.device(MASTER, comp.id).sidechain == Sidechain(kick.id)
    assert editor.move_devices_to_track(MASTER, [comp.id], kick.id)  # onto its source: a cycle
    assert p.device(kick.id, comp.id).sidechain is None
    editor.undo_stack.undo()
    assert p.device(MASTER, comp.id).sidechain == Sidechain(kick.id)


def test_sidechains_are_saved_and_loaded(editor):
    p = editor.project
    a = editor.add_audio_track()
    b = editor.add_audio_track()
    c = editor.add_audio_track()
    ret = editor.add_return_track()
    tapped = device(editor, c)
    on_a, on_b, on_ret, on_master = device(editor, a), device(editor, b), device(editor, ret), device(editor, p.master)
    editor.set_device_sidechain(a.id, on_a.id, Sidechain(c.id, tapped.id))  # a track listed after it
    editor.set_device_sidechain(b.id, on_b.id, Sidechain(ret.id, PRE_FADER))
    editor.set_device_sidechain(ret.id, on_ret.id, Sidechain(a.id))
    editor.set_device_sidechain(MASTER, on_master.id, Sidechain(ret.id))
    data = project_to_dict(p)
    assert data["tracks"][0]["devices"][0]["sidechain"] == {"track": c.id, "tap": tapped.id}
    loaded = Project()
    load_into(loaded, data)
    found = [loaded.track(t).devices[-1].sidechain for t in (a.id, b.id, ret.id, MASTER)]
    assert found == [Sidechain(c.id, tapped.id), Sidechain(ret.id, PRE_FADER), Sidechain(a.id), Sidechain(ret.id)]

    # Edited files: a sidechain from a track that isn't there, the master, or one closing a cycle, is dropped.
    data["tracks"][0]["devices"][0]["sidechain"]["track"] = "gone"
    data["master"]["devices"][0]["sidechain"]["track"] = MASTER
    data["returns"][0]["devices"][0]["sidechain"]["track"] = b.id  # b <- ret <- b: the later one goes
    load_into(loaded, data)
    found = [loaded.track(t).devices[-1].sidechain for t in (a.id, b.id, ret.id, MASTER)]
    assert found == [None, Sidechain(ret.id, PRE_FADER), None, None]
    del data["tracks"][1]["devices"][0]["sidechain"]  # (older files have none)
    load_into(loaded, data)
    assert loaded.track(b.id).devices[0].sidechain is None


@pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")
def test_the_engine_takes_sidechains(app):
    from gilstudio.audio.engine_bridge import EngineBridge

    engine = ge.Engine()
    project = Project()
    stack = QUndoStack()
    editor = ProjectEditor(project, stack)
    bridge = EngineBridge(engine, project)
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "GIL Test Sidechain")
    ref = PluginRef(format="VST3", uid=uid, name="GIL Test Sidechain", path=str(TEST_PLUGINS))
    try:
        def engine_sidechain(track, dev):
            info = engine.processor_sidechain(bridge.engine_device_id(track.id, dev.id))
            return None if info is None else (info.track_id, info.tap, info.tap_processor_id)

        kick = editor.add_audio_track()
        bass = editor.add_audio_track()
        keyed = editor.add_device(bass.id, PLUGIN_KIND, plugin=ref)
        utility = editor.add_device(bass.id, "utility")
        assert bridge.has_sidechain_input(bass.id, keyed.id) and not bridge.has_sidechain_input(bass.id, utility.id)
        assert engine_sidechain(bass, keyed) is None
        editor.set_device_sidechain(bass.id, keyed.id, Sidechain(kick.id))
        kick_id = bridge._track_ids[kick.id]
        assert engine_sidechain(bass, keyed) == (kick_id, ge.SidechainTap.POST_FADER, 0)
        tapped = editor.add_device(kick.id, "utility")
        editor.set_device_sidechain(bass.id, keyed.id, Sidechain(kick.id, tapped.id))
        tapped_id = bridge.engine_device_id(kick.id, tapped.id)
        assert engine_sidechain(bass, keyed) == (kick_id, ge.SidechainTap.AFTER_DEVICE, tapped_id)
        editor.remove_device(kick.id, tapped.id)  # before the fader, until it comes back
        assert engine_sidechain(bass, keyed) == (kick_id, ge.SidechainTap.PRE_FADER, 0)
        stack.undo()
        tapped_id = bridge.engine_device_id(kick.id, tapped.id)
        assert engine_sidechain(bass, keyed) == (kick_id, ge.SidechainTap.AFTER_DEVICE, tapped_id)
        # Moving the device keeps it (a processor moving gives it up in the engine until it is there).
        pad = editor.add_audio_track()
        editor.move_devices_to_track(bass.id, [keyed.id], pad.id)
        assert engine_sidechain(pad, keyed) == (kick_id, ge.SidechainTap.AFTER_DEVICE, tapped_id)
        # Into its source's group: the sidechain goes before the route that would close the cycle.
        group = editor.group_tracks([kick.id])
        editor.set_device_sidechain(pad.id, keyed.id, Sidechain(group.id))
        assert engine_sidechain(pad, keyed) == (bridge._track_ids[group.id], ge.SidechainTap.POST_FADER, 0)
        editor.move_tracks([pad.id], project.track_index(kick.id) + 1, group.id)
        assert engine_sidechain(pad, keyed) is None
        assert engine.track_output(bridge._track_ids[pad.id]) == bridge._track_ids[group.id]
        stack.undo()
        assert engine_sidechain(pad, keyed) == (bridge._track_ids[group.id], ge.SidechainTap.POST_FADER, 0)
        # Deleting the source, and undoing it.
        editor.delete_tracks([group.id])
        assert engine_sidechain(pad, keyed) is None
        stack.undo()
        assert engine_sidechain(pad, keyed) == (bridge._track_ids[group.id], ge.SidechainTap.POST_FADER, 0)
        # A project loaded with a device keyed by a track listed after its own.
        later = editor.add_audio_track()
        editor.set_device_sidechain(pad.id, keyed.id, Sidechain(later.id, PRE_FADER))
        load_into(project, project_to_dict(project))
        pad, keyed = project.track(pad.id), project.device(pad.id, keyed.id)
        assert engine_sidechain(pad, keyed) == (bridge._track_ids[later.id], ge.SidechainTap.PRE_FADER, 0)
    finally:
        bridge.shutdown()
        engine.close_device()
