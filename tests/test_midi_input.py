"""MIDI input in the engine, with the fake ASIO driver (tests/asio_driver) in
manual mode: live notes reaching a track's instrument at the right offset in a
block, which tracks hear which input, monitoring, notes released when the
transport stops (or a track stops hearing its input), and recorded notes
landing on the beats they were played, latency and all.

Messages are sent with send_midi_input(), stamped against the audio clock the
engine reports after the last buffer, so where each one plays is known to the
sample. The instrument is GIL Test Synth in its DC mode: each held note adds its
velocity (/127) to every sample, which shows exactly when it starts and stops."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import TEST_PLUGINS
from .test_recording import (  # noqa: F401 - the fixtures; skipped without ASIO
    BUFFER,
    RATE,
    SPB,
    driver,
    engine,
    latent_effect,
    output,
    pytestmark,
)
from .test_asio import open_asio

NOTE_ON, NOTE_OFF = 0x90, 0x80


def synth(engine, track) -> int:
    if not TEST_PLUGINS.exists():
        pytest.skip("test plug-ins not built")
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "GIL Test Synth")
    synth = engine.add_plugin_processor(engine.track_chain(track), "VST3", str(TEST_PLUGINS), uid)
    engine.set_processor_param(synth, engine.processor_param_index(synth, "1"), 0.0)  # Wave: DC
    return synth


def send(engine, message, offset, device="Keys"):
    """Sends `message` so that it plays `offset` samples into the next buffer
    (it plays one buffer after it arrives)."""
    clock = engine.audio_clock
    assert clock.running and clock.midi_delay == BUFFER
    engine.send_midi_input(device, message, clock.host_time_ns + round(offset * 1e9 / RATE))


def heard(driver, buffers=1, channel=0) -> np.ndarray:
    driver.ClearOutput()
    driver.process(buffers)
    return output(driver, channel)


def held(*notes, length=BUFFER) -> np.ndarray:
    """What the synth plays for notes (start, end, velocity) within one stretch."""
    expected = np.zeros(length)
    for start, end, velocity in notes:
        expected[start:end] += velocity / 127
    return expected


@pytest.fixture
def keys(engine, driver):
    """A running device and a track with the synth, hearing every MIDI input (armed)."""
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    track = engine.add_track()
    synth(engine, track)
    engine.set_track_midi_input(track, True)
    engine.set_track_armed(track, True)
    driver.process(2)
    return track


# --- Playing ------------------------------------------------------------------------


def test_live_notes_reach_the_instrument_at_their_offsets(engine, driver, keys):
    send(engine, [NOTE_ON, 60, 127], 100)
    send(engine, [NOTE_ON, 64, 64], 150)
    send(engine, [NOTE_OFF, 60, 0], 200)
    np.testing.assert_allclose(heard(driver), held((100, 200, 127), (150, BUFFER, 64)), atol=1e-6)
    # Velocity 0 releases too; one sent further ahead waits for its buffer.
    send(engine, [NOTE_ON, 64, 0], BUFFER + 30)
    np.testing.assert_allclose(heard(driver, 2), held((0, BUFFER + 30, 64), length=2 * BUFFER), atol=1e-6)
    assert np.all(heard(driver) == 0.0)


def test_a_note_released_as_it_is_played_still_sounds(engine, driver, keys):
    # Both late (stamped before the last buffer began): the note-off follows the note-on.
    send(engine, [NOTE_ON, 60, 127], -2 * BUFFER)
    send(engine, [NOTE_OFF, 60, 0], -2 * BUFFER)
    np.testing.assert_allclose(heard(driver), held((0, 1, 127)), atol=1e-6)
    # In the last sample of a buffer: it ends in the next one.
    send(engine, [NOTE_OFF, 62, 0], BUFFER - 1)  # (a stray note-off first: ignored)
    send(engine, [NOTE_ON, 62, 127], BUFFER - 1)
    send(engine, [NOTE_OFF, 62, 0], BUFFER - 1)
    np.testing.assert_allclose(heard(driver, 2), held((BUFFER - 1, BUFFER, 127), length=2 * BUFFER), atol=1e-6)


def test_which_tracks_hear_which_input(engine, driver):
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    left, right = engine.add_track(), engine.add_track()
    for track, pan in ((left, -1.0), (right, 1.0)):
        synth(engine, track)
        engine.set_track_pan(track, pan)
        engine.set_track_monitor(track, ge.MonitorMode.IN)
    engine.set_track_midi_input(left, True, "Keys", 1)  # one input, channel 1
    engine.set_track_midi_input(right, True, "", 2)  # every input, channel 2
    driver.process(1)

    def play(device, status):
        send(engine, [status, 60, 127], 0, device)
        send(engine, [NOTE_OFF | status & 0x0F, 60, 0], 10, device)
        driver.ClearOutput()
        driver.process(1)
        return [bool(np.abs(output(driver, c)).max() > 0.5) for c in (0, 1)]

    assert play("Keys", 0x90) == [True, False]
    assert play("Keys", 0x91) == [False, True]
    assert play("Pads", 0x91) == [False, True]
    assert play("Pads", 0x90) == [False, False]
    assert play("Keys", 0x92) == [False, False]
    engine.set_track_midi_input(left, True)  # every input, every channel
    assert play("Pads", 0x92) == [True, False]
    engine.set_track_midi_input(left, False)
    assert play("Keys", 0x90) == [False, False]
    with pytest.raises(ValueError):
        engine.set_track_midi_input(left, True, "Keys", 17)
    with pytest.raises(ValueError):
        engine.set_track_midi_input(ge.MASTER, True)
    with pytest.raises(ValueError):
        engine.send_midi_input("Keys", [60, 100])  # no status byte


def test_monitoring(engine, driver, keys):
    engine.set_track_notes(keys, [ge.NoteDesc(0.0, 64.0, 72, 127)])  # a clip's note, all along

    def levels():
        send(engine, [NOTE_ON, 60, 64], 0)
        out = heard(driver)
        send(engine, [NOTE_OFF, 60, 0], 0)
        driver.process(1)
        return round(float(out[-1]) * 127)

    engine.set_track_monitor(keys, ge.MonitorMode.AUTO)
    assert levels() == 64  # armed, stopped
    engine.play()
    driver.process(1)
    assert levels() == 127 + 64  # armed: the clip and the input
    engine.set_track_armed(keys, False)
    assert levels() == 127  # only the clip
    engine.set_track_monitor(keys, ge.MonitorMode.IN)
    assert levels() == 64  # only the input
    engine.set_track_monitor(keys, ge.MonitorMode.OFF)
    engine.set_track_armed(keys, True)
    engine.stop()
    engine.play()  # the clip's note starts again (it stopped when In took over)
    driver.process(1)
    assert levels() == 127  # only the clip


def test_held_keys_are_released(engine, driver, keys):
    send(engine, [NOTE_ON, 60, 127], 0)
    engine.play()
    assert np.all(heard(driver) == 1.0)
    engine.stop()  # stopping releases the held keys
    assert np.all(heard(driver) == 0.0)
    send(engine, [NOTE_OFF, 60, 0], 0)  # its note-off now changes nothing
    send(engine, [NOTE_ON, 62, 127], 10)
    np.testing.assert_allclose(heard(driver), held((10, BUFFER, 127)), atol=1e-6)

    engine.set_track_armed(keys, False)  # the track stops hearing its input (Auto)
    assert np.all(heard(driver) == 0.0)
    engine.set_track_armed(keys, True)
    send(engine, [NOTE_OFF, 62, 0], 0)
    send(engine, [NOTE_ON, 64, 127], 0)
    assert np.all(heard(driver) == 1.0)
    engine.set_track_midi_input(keys, True, "Pads")  # nor from this input
    assert np.all(heard(driver) == 0.0)

    engine.set_track_midi_input(keys, True)
    send(engine, [NOTE_ON, 65, 127], 0)
    driver.process(1)
    engine.remove_track(keys)  # its notes go with it
    other = engine.add_track()
    synth(engine, other)
    engine.set_track_midi_input(other, True)
    engine.set_track_monitor(other, ge.MonitorMode.IN)
    assert np.all(heard(driver) == 0.0)
    send(engine, [NOTE_OFF, 65, 0], 0)  # a note-off for a note it never had
    assert np.all(heard(driver) == 0.0)


def test_without_a_running_device_input_is_dropped(engine, driver):
    track = engine.add_track()
    synth(engine, track)
    engine.set_track_midi_input(track, True)
    engine.set_track_monitor(track, ge.MonitorMode.IN)
    assert not engine.audio_clock.running
    engine.send_midi_input("Keys", [NOTE_ON, 60, 127])
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    assert np.all(heard(driver, 4) == 0.0)
    assert engine.audio_clock.running
    engine.close_device()
    assert not engine.audio_clock.running


def test_the_midi_device_list(engine):
    # Whatever is connected to this computer: names, and none open until asked.
    names = engine.midi_input_devices()
    assert all(isinstance(name, str) and name for name in names)
    assert len(set(names)) == len(names)
    assert engine.open_midi_inputs() == []
    with pytest.raises(RuntimeError, match="not connected"):
        engine.open_midi_input("No Such MIDI Device")
    engine.close_midi_input("No Such MIDI Device")  # not open: nothing to do


# --- Recording ----------------------------------------------------------------------


def playhead(engine) -> int:
    return round(engine.position_beats * SPB)


def send_heard(engine, beat, message, latency):
    """Sends `message` as a player would who hears the timeline at `beat` then:
    the output plays a buffer's position `latency` samples after the renderer
    rendered it (the device's output latency, and the delay of the master's
    devices or of delay compensation)."""
    clock = engine.audio_clock
    assert clock.running
    rendered = playhead(engine) - BUFFER  # the timeline position of the last buffer's first sample
    ahead = round(beat * SPB) - rendered + latency  # samples after the last buffer began
    assert 0 <= ahead < RATE // 2, "send it closer to its time"
    engine.send_midi_input("Keys", message, clock.host_time_ns + round(ahead * 1e9 / RATE))


def process_until(driver, engine, beat):
    while engine.position_beats < beat:
        driver.process(1)


@pytest.mark.parametrize("latency", [None, "track", "master"])
def test_recorded_notes_land_on_the_beats_they_were_played(engine, driver, latency):
    driver.SetLatencies(40, 90)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    track = engine.add_track()
    synth(engine, track)
    engine.set_track_midi_input(track, True)
    engine.set_track_armed(track, True)
    if latency == "track":
        latent_effect(engine, track, 333)  # every track is delayed to line up with it...
    elif latency == "master":
        latent_effect(engine, ge.MASTER, 333)  # ...or comes out of the master this late
    lag = round(engine.device_status.latency_ms * RATE / 1000) + (333 if latency else 0)
    assert lag >= BUFFER + 90

    engine.position_beats = 4.0
    engine.start_recording([(track, "")])
    assert engine.is_recording and engine.is_playing
    driver.process(1)
    played = [(4.5, 5.0, 60, 100), (5.25, 5.5, 64, 80), (5.5, 6.0, 67, 127)]
    for start, end, key, velocity in played:
        process_until(driver, engine, start - 0.5)
        send_heard(engine, start, [NOTE_ON, key, velocity], lag)
        process_until(driver, engine, end - 0.5)
        send_heard(engine, end, [NOTE_OFF, key, 0], lag)
    process_until(driver, engine, 6.25)
    send_heard(engine, 6.5, [NOTE_ON | 3, 48, 90], lag)  # held when the take ends (channel 4)
    process_until(driver, engine, 6.75)

    progress = engine.recording_progress()
    assert [(p.track_id, p.midi, p.started, p.start_sample) for p in progress] == [(track, True, True, 4 * SPB)]
    assert progress[0].notes.tolist()[-1] == [round(6.5 * SPB), -1, 48, 90, 3]

    takes = engine.stop_recording()
    assert len(takes) == 1
    take = takes[0]
    assert (take.track_id, take.midi, take.path, take.start_sample, take.error) == (track, True, "", 4 * SPB, "")
    end = playhead(engine)
    assert take.frames == end - 4 * SPB
    expected = [[round(s * SPB), round(e * SPB), key, velocity, 0] for s, e, key, velocity in played]
    assert take.notes.tolist() == expected + [[round(6.5 * SPB), end, 48, 90, 3]]


def test_midi_and_audio_record_together(engine, driver, tmp_path):
    driver.SetInputLevel(0, 0.5)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    audio, midi = engine.add_track(), engine.add_track()
    engine.set_track_input(audio, [0])
    engine.set_track_midi_input(midi, True, "Keys", 1)
    engine.set_track_monitor(midi, ge.MonitorMode.OFF)  # recorded, though not heard
    engine.start_recording([(audio, str(tmp_path / "a.wav")), (midi, "")], count_in_beats=1.0)
    driver.process(1)
    send(engine, [NOTE_ON, 50, 100], 0)  # during the count-in: not recorded
    driver.process(SPB // BUFFER + 4)
    send(engine, [NOTE_ON | 1, 51, 100], 0)  # another channel: not this track's
    send(engine, [NOTE_ON, 52, 100], 0)
    send(engine, [NOTE_OFF, 52, 0], 10)
    driver.process(2)
    takes = {t.track_id: t for t in engine.stop_recording()}
    assert takes[audio].frames > 0 and not takes[audio].midi and takes[audio].notes.shape == (0, 5)
    take = takes[midi]
    assert take.midi and take.frames == takes[audio].frames
    assert take.notes[:, 2].tolist() == [52]
    assert take.notes[0, 1] - take.notes[0, 0] == 10


def test_recording_midi_needs_a_midi_input(engine, driver):
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    track = engine.add_track()
    with pytest.raises(ValueError, match="no MIDI input"):
        engine.start_recording([(track, "")])
    engine.set_track_midi_input(track, True)
    with pytest.raises(ValueError, match="twice"):
        engine.start_recording([(track, ""), (track, "")])
    engine.start_recording([(track, "")])
    engine.stop()  # stopped before the playhead moved: an empty take
    take = engine.stop_recording()[0]
    assert take.midi and take.frames == 0 and take.notes.shape == (0, 5)
