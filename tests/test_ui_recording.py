"""Recording in the application, with the fake ASIO driver (tests/asio_driver)
in manual mode: arming tracks and choosing their input and monitoring in the
track headers, the record button, the live waveform, the takes becoming clips
in one undo step, and what ends a recording. MIDI tracks too: their MIDI input
(played with send_midi_input()), live notes, record quantization, and the
MIDI inputs in Preferences."""

import time
from datetime import UTC

import numpy as np
import pytest

from substation.audio.engine_bridge import recordings_folder, take_path
from substation.audio.settings import AudioSettings, record_quantize
from substation.model.project import MidiInput

from .conftest import TEST_ASIO_NAME
from .test_asio import (  # noqa: F401 - skipped without ASIO
    FLOAT32_LSB,
    Driver,
    pytestmark,
)

RATE = 48000
BUFFER = 256
LAG = 2 * BUFFER + 32 + 64  # the driver's input and output latency: how late the cable brings the output back


@pytest.fixture
def driver():
    d = Driver()
    d.SetManual(1)
    d.SetSampleType(FLOAT32_LSB)
    return d


@pytest.fixture
def studio(window, driver, tmp_path, monkeypatch):
    """The main window playing through the test driver (its settings saved, as Preferences would)."""
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    settings = AudioSettings("ASIO", TEST_ASIO_NAME, RATE, BUFFER)
    assert window.bridge.open_device(settings) is None
    settings.save()
    return window


def wait_until(app, predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        app.processEvents()
        if predicate():
            return True
        time.sleep(0.005)
    return False


def header(window, track_id):
    return window.arrangement.headers.headers[track_id]


def choose(menu, text):
    action = next(a for a in menu.actions() if a.text().startswith(text))
    action.trigger()


def test_header_controls(studio, app):
    window = studio
    track = window.editor.add_audio_track()
    other = window.editor.add_audio_track()
    midi = window.editor.add_midi_track()
    app.processEvents()
    h = header(window, track.id)
    assert h.arm.isVisible() and h.input.text() == "No Input" and h.monitor.text() == "Auto"
    assert header(window, midi.id).arm.isVisible() and header(window, midi.id).input.text() == "All Ins"

    menu = h.input_menu()
    labels = [a.text() for a in menu.actions() if a.text()]
    assert labels == (["No Input"] + [f"In {i}  (Test In {i})" for i in range(1, 5)] + ["In 1/2", "In 3/4"]
                      + ["Resampling", other.name, midi.name])  # (other tracks' outputs: resampling)
    choose(menu, "In 3/4")
    assert track.input == (2, 3) and h.input.text() == "In 3/4"
    # The device didn't have them open: it has now (and keeps them).
    assert window.engine.device_status.input_channels == [2, 3]
    assert AudioSettings.load().input_channels == (2, 3)
    choose(h.monitor_menu(), "Off")
    assert track.monitor == "off" and h.monitor.text() == "Off"
    window.undo_stack.undo()
    assert track.monitor == "auto"

    h.arm.click()
    header(window, other.id).arm.click()  # arming one disarms the others
    assert (track.armed, other.armed) == (False, True)
    assert header(window, other.id).arm.isChecked() and not h.arm.isChecked()


def test_record_a_take_through_the_cable(studio, app, driver, make_wav):
    window = studio
    driver.SetLoopback(0, 0)
    rng = np.random.default_rng(3)
    signal = np.round(rng.uniform(-0.5, 0.5, RATE) * 32768) / 32768
    wav = make_wav(signal)
    window.add_file_at_insert(wav)  # a track playing it from beat 0
    player = window.project.tracks[0]
    assert wait_until(app, lambda: window.bridge.source(wav) is not None)
    recorder = window.editor.add_audio_track()
    app.processEvents()
    h = header(window, recorder.id)
    choose(h.input_menu(), "In 1  ")
    choose(h.monitor_menu(), "Off")  # or it would play its input back into the cable
    h.arm.click()
    assert recorder.armed

    window.transport.record.click()
    assert window.bridge.is_recording and window.transport.record.isChecked()
    driver.process(RATE // BUFFER)  # a second
    window.bridge._poll_meters()
    live = window.bridge.live_takes[recorder.id]
    assert live.started and live.start_sample == -LAG and live.frames == RATE // BUFFER * BUFFER
    assert len(live.peaks) == live.frames // live.PEAK_FRAMES and np.abs(live.peaks).max() > 0.4
    window.arrangement.lanes.grab()  # draws the live take

    steps = window.undo_stack.index()
    window.transport.record.click()  # punch out: it keeps playing
    assert not window.bridge.is_recording and window.bridge.is_playing
    assert not window.transport.record.isChecked()
    assert window.undo_stack.index() == steps + 1 and window.undo_stack.undoText() == "Record"
    take = recorder.clips[0]
    assert take.path.startswith(str(recordings_folder(window.project)))
    assert take.name.startswith(recorder.name)
    # The take begins with the timeline (what came in before it was the latency).
    assert take.start_beat == 0.0 and take.offset_sec == pytest.approx(LAG / RATE)
    assert take.source_duration_sec == pytest.approx((RATE // BUFFER * BUFFER) / RATE)
    assert [c.id for c in player.clips] != [] and window.selection.clips == {(recorder.id, take.id)}

    # It holds what the player played, in time with it (once its file is decoded: that's in the background).
    assert wait_until(app, lambda: window.bridge.source(take.path) is not None)
    window.toggle_play()
    window.engine.set_track_mute(window.bridge._track_ids[player.id], True)
    out = window.engine.render_offline(0.0, RATE // 2)[:, 0]
    fade = 200  # past the clips' 4 ms fade-in (the player's, and the take's)
    np.testing.assert_allclose(out[fade:], signal[fade:RATE // 2], atol=1e-4)

    window.undo_stack.undo()
    assert recorder.clips == []


def test_space_stops_recording_and_count_in(studio, app, driver):
    window = studio
    driver.SetInputLevel(0, 0.5)
    track = window.editor.add_audio_track()
    window.editor.set_track_input(track.id, (0,))
    window.editor.arm_tracks([track.id], True)
    window.transport.count_in.setCurrentIndex(1)  # a bar
    window.selection.set_insert(4.0)
    window.toggle_record()
    driver.process(4)
    assert window.bridge.is_counting_in and window.bridge.position == 4.0
    driver.process(4 * RATE // 2 // BUFFER + 8)  # the bar of 4/4, and a bit
    assert not window.bridge.is_counting_in and window.bridge.position > 4.0
    window.toggle_play()  # Space: stops playing and recording
    assert not window.bridge.is_recording and not window.bridge.is_playing
    assert window.bridge.position == 4.0  # back where it started
    take = track.clips[0]
    assert take.start_beat == pytest.approx(4.0 - LAG / (RATE / 2))


def test_a_device_change_ends_the_recording(studio, app, driver):
    window = studio
    track = window.editor.add_audio_track()
    window.editor.set_track_input(track.id, (1,))
    window.editor.arm_tracks([track.id], True)
    assert window.bridge.start_recording() is None
    driver.process(10)
    window.bridge.open_device(AudioSettings("ASIO", TEST_ASIO_NAME, 96000, BUFFER, input_channels=(1,)))
    window.bridge._poll_meters()
    assert not window.bridge.is_recording and not window.transport.record.isChecked()
    assert len(track.clips) == 1 and track.clips[0].source_duration_sec == pytest.approx(10 * BUFFER / RATE)


def test_nothing_to_record(window, studio):
    assert window.bridge.start_recording() == "Arm a MIDI track, or an audio track that has an input, to record."
    window.toggle_record()
    assert "Arm a MIDI track" in window.statusBar().currentMessage()
    assert not window.bridge.is_playing


def test_take_names(tmp_path):
    from datetime import datetime

    when = datetime(2026, 10, 1, 12, 30, 5, tzinfo=UTC)
    first = take_path(tmp_path, 'Vox: "lead"', when)
    assert first.name == "Vox_ _lead_ 2026-10-01 123005.wav"
    first.write_bytes(b"")
    assert take_path(tmp_path, 'Vox: "lead"', when).name == "Vox_ _lead_ 2026-10-01 123005 2.wav"


# --- MIDI -----------------------------------------------------------------------------


def test_midi_header_controls(studio, app):
    window = studio
    track = window.editor.add_midi_track()
    app.processEvents()
    h = header(window, track.id)
    assert h.input.isVisible() and h.monitor.isVisible() and h.input.text() == "All Ins"
    menu = h.input_menu()
    assert [a.text() for a in menu.actions() if a.text() and not a.menu()][:2] == ["No Input", "All Ins"]
    channels = next(a.menu() for a in menu.actions() if a.menu())
    choose(channels, "Channel 10")
    assert track.midi_input == MidiInput("", 10) and h.input.text() == "All Ins · Ch 10"
    choose(h.input_menu(), "No Input")
    assert track.midi_input is None and h.input.text() == "No Input"
    h.arm.click()
    assert track.armed
    assert "has no MIDI input" in window.statusBar().currentMessage()
    window.undo_stack.undo()
    assert track.midi_input == MidiInput("", 10)
    # An input that isn't connected (now) can still be chosen, and shows as such.
    window.editor.set_track_midi_input(track.id, MidiInput("Old Keyboard"))
    assert any(a.text() == "Old Keyboard (not connected)" and a.isChecked() for a in h.input_menu().actions())


def test_record_midi(studio, app, driver):
    window = studio
    track = window.editor.add_midi_track()
    app.processEvents()
    header(window, track.id).arm.click()
    next(a for a in window.record_quantize_actions if a.text() == "1/16").trigger()
    assert record_quantize() == 0.25
    window.transport.record.click()
    assert window.bridge.is_recording
    driver.process(8)

    def send(message):
        clock = window.engine.audio_clock
        window.engine.send_midi_input("Keys", message, clock.host_time_ns)  # plays in the next buffer

    send([0x90, 60, 100])
    driver.process(25)
    send([0x80, 60, 0])
    send([0x90, 67, 90])  # held when recording stops
    driver.process(4)
    window.bridge._poll_meters()
    live = window.bridge.live_takes[track.id]
    assert live.midi and live.started and live.notes[:, 2].tolist() == [60, 67] and live.notes[1, 1] == -1
    window.arrangement.lanes.grab()  # draws the live notes

    steps = window.undo_stack.index()
    window.transport.record.click()
    assert not window.bridge.is_recording and window.undo_stack.index() == steps + 1
    [clip] = track.clips
    assert clip.start_beat == 0.0 and window.selection.clips == {(track.id, clip.id)}
    assert [(n.pitch, n.velocity) for n in clip.notes] == [(60, 100), (67, 90)]
    assert all(n.start % 0.25 == 0 for n in clip.notes)  # record quantization
    assert clip.notes[0].length == pytest.approx(25 * BUFFER / (RATE / 2))
    assert clip.notes[1].end == pytest.approx(clip.duration_beats, abs=0.25)  # held: until recording stopped
    window.undo_stack.undo()
    assert track.clips == []


def test_preferences_list_the_midi_inputs(window):
    from substation.ui.dialogs import PreferencesDialog

    dialog = PreferencesDialog(window.bridge, window)
    tabs = [dialog.tabs.tabText(i) for i in range(dialog.tabs.count())]
    assert tabs[:2] == ["Audio", "MIDI"]
    names = window.bridge.midi_inputs()  # whatever this computer has
    assert dialog.midi.inputs.count() == len(names)
    if not names:
        assert dialog.midi.status.text() == "No MIDI input is connected."
    dialog.midi.refresh()
    dialog.deleteLater()
