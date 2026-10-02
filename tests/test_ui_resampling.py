"""Resampling in the application, with the fake ASIO driver in manual mode: a
track's input menu lists the master ("Resampling") and the other tracks,
groups and returns (greyed out where taking their output would close a cycle);
a take recorded from one becomes a clip holding what it played, in one undo
step."""

import numpy as np
import pytest

from gilstudio.model.automation import MASTER

from .test_asio import pytestmark  # noqa: F401 - skipped without ASIO
from .test_ui_recording import (  # noqa: F401
    RATE,
    choose,
    driver,
    header,
    studio,
    wait_until,
)


def test_the_input_menu_lists_tracks(studio, app):
    window = studio
    editor = window.editor
    a = editor.add_audio_track(name="Drums")
    b = editor.add_audio_track(name="Bass")
    group = editor.group_tracks([b.id])
    ret = editor.add_return_track()
    app.processEvents()
    h = header(window, b.id)
    menu = h.input_menu()
    actions = {a.text(): a for a in menu.actions() if a.text()}
    assert [t for t in actions if not t.startswith("In ")] == [
        "No Input", "Resampling", "Drums", f"{group.name} (it takes this track's output)", ret.name]
    assert not actions[f"{group.name} (it takes this track's output)"].isEnabled()  # its own group
    assert actions["Drums"].isEnabled() and actions["No Input"].isChecked()

    choose(menu, "Drums")
    assert b.input_track == a.id and h.input.text() == "Drums" and b.has_input
    assert "Drums's output" in h.input.toolTip()
    assert next(x for x in h.input_menu().actions() if x.text() == "Drums").isChecked()
    editor.rename_track(a.id, "Beat")
    app.processEvents()
    assert h.input.text() == "Beat"  # it follows its source's name
    choose(h.input_menu(), "Resampling")
    assert b.input_track == MASTER and h.input.text() == "Resampling"
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert b.input_track == a.id
    window.undo_stack.undo()
    assert b.input_track is None and h.input.text() == "No Input"

    # A return the track sends to can't be its source.
    editor.set_send(b.id, ret.id, 0.0)
    assert not next(x for x in h.input_menu().actions() if x.text().startswith(ret.name)).isEnabled()


@pytest.mark.parametrize("source", ["track", "master"])
def test_record_a_resampled_take(studio, app, driver, make_wav, source):
    """A take of a track's output (or the master's) becomes a clip on the
    recording track holding what was played, where it was played."""
    window = studio
    rng = np.random.default_rng(5)
    signal = np.round(rng.uniform(-0.5, 0.5, RATE) * 32768) / 32768
    wav = make_wav(signal)
    window.add_file_at_insert(wav)  # a track playing it from beat 0
    player = window.project.tracks[0]
    assert wait_until(app, lambda: window.bridge.source(wav) is not None)
    recorder = window.editor.add_audio_track()
    app.processEvents()
    h = header(window, recorder.id)
    choose(h.input_menu(), "Resampling" if source == "master" else player.name)
    choose(h.monitor_menu(), "Off")
    h.arm.click()
    assert recorder.armed and window.bridge.record_targets() == [recorder]

    window.transport.record.click()
    assert window.bridge.is_recording
    driver.process(RATE // 2 // 256)  # half a second
    window.bridge._poll_meters()
    live = window.bridge.live_takes[recorder.id]
    assert live.started and live.start_sample == 0 and np.abs(live.peaks).max() > 0.4
    steps = window.undo_stack.index()
    window.transport.record.click()
    assert window.undo_stack.index() == steps + 1 and window.undo_stack.undoText() == "Record"
    take = recorder.clips[0]
    assert take.start_beat == 0.0 and take.offset_sec == 0.0
    assert wait_until(app, lambda: window.bridge.source(take.path) is not None)
    assert window.bridge.source(take.path).channels == 2

    window.toggle_play()
    window.engine.set_track_mute(window.bridge._track_ids[player.id], True)
    out = window.engine.render_offline(0.0, RATE // 4)
    fade = 200  # past the clips' 4 ms fade-in (the player's, and the take's)
    for channel in (0, 1):
        np.testing.assert_allclose(out[fade:, channel], signal[fade:RATE // 4], atol=1e-4)

    window.undo_stack.undo()
    assert recorder.clips == []


def test_send_knobs_follow_inputs(studio, app):
    """A track's input is a routing edge: it changes which sends other tracks
    can make (a group whose track takes a return's output can't send to it)."""
    window = studio
    editor = window.editor
    track = editor.add_audio_track()
    other = editor.add_audio_track()
    group = editor.group_tracks([track.id])
    ret = editor.add_return_track()
    app.processEvents()

    def usable(owner) -> bool:
        return header(window, owner).sends.knobs[ret.id][1].isEnabled()
    assert usable(group.id) and usable(other.id)
    editor.set_track_input_track(track.id, ret.id)  # ret -> track -> group
    assert not usable(group.id) and not usable(track.id) and usable(other.id)
    window.undo_stack.undo()
    assert usable(group.id)
    editor.set_track_input_track(other.id, ret.id)
    assert not usable(other.id) and usable(group.id)
    editor.move_tracks([other.id], window.project.track_index(track.id) + 1, group.id)
    app.processEvents()
    assert other.parent == group.id and other.input_track == ret.id
    assert not usable(group.id)  # moved into the group: ret -> other -> group
