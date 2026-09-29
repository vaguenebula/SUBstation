"""MIDI in the engine: note scheduling and the built-in Synth.
Rendered offline, so no audio device is needed."""

import time

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE

SPB = int(SAMPLE_RATE * 60 / 120)  # samples per beat at 120 BPM
WAVE, ATTACK, DECAY, SUSTAIN, RELEASE, CUTOFF, RESONANCE, VOLUME = range(8)
SINE_RMS = 0.25 * 100 / 127 / np.sqrt(2)  # one voice at the default velocity (100)


@pytest.fixture
def engine():
    e = ge.Engine()
    yield e
    e.close_device()


@pytest.fixture
def live_engine():
    """An engine with a running output device (the default, else any that opens)."""
    e = ge.Engine()
    for name in [""] + [d.name for d in e.list_output_devices()]:
        try:
            e.open_device(name, 0, 256, False)
            break
        except RuntimeError:
            continue
    else:
        pytest.skip("no audio output device")
    yield e
    e.close_device()


def wait_beats(engine, beats, timeout=5.0):
    """Wait until the playing transport has advanced `beats`."""
    target = engine.position_beats + beats
    deadline = time.monotonic() + timeout
    while engine.position_beats < target:
        assert time.monotonic() < deadline, "the audio device stopped"
        time.sleep(0.002)


def synth_track(engine, notes, params=None):
    """A track with a Synth (sine, no filtering, short release unless `params` say
    otherwise) playing `notes`, each (start beat, length beats, key[, velocity])."""
    track = engine.add_track()
    synth = engine.add_builtin_processor(track, "synth")
    settings = {WAVE: 0, CUTOFF: 20000.0, RELEASE: 5.0, SUSTAIN: 100.0} | (params or {})
    for index, value in settings.items():
        engine.set_processor_param(synth, index, value)
    engine.set_track_notes(track, [ge.NoteDesc(*note) for note in notes])
    return track, synth


def dominant_freq(samples):
    spectrum = np.abs(np.fft.rfft(samples * np.hanning(len(samples))))
    peak = int(np.argmax(spectrum))
    a, b, c = np.log(spectrum[peak - 1 : peak + 2] + 1e-12)
    return (peak + 0.5 * (a - c) / (a - 2 * b + c)) * SAMPLE_RATE / len(samples)


def rms(samples):
    return float(np.sqrt(np.mean(samples**2)))


def test_synth_plays_notes_on_their_beats(engine):
    synth_track(engine, [(1.0, 2.0, 69)])  # A3 (440 Hz) from beat 1 to beat 3
    out = engine.render_offline(0.0, 5 * SPB)
    assert np.all(out[:SPB] == 0.0)
    assert np.abs(out[SPB : SPB + 480]).max() > 0.05  # within 10 ms (3 ms attack)
    np.testing.assert_array_equal(out[:, 0], out[:, 1])  # mono on both channels
    assert dominant_freq(out[SPB + 4800 : SPB + 4800 + 16384, 0]) == pytest.approx(440.0, rel=1e-3)
    held = out[2 * SPB : 3 * SPB - 100, 0]
    assert rms(held) == pytest.approx(SINE_RMS, rel=0.02)  # sustain 100 %
    assert np.abs(out[3 * SPB + SAMPLE_RATE // 50 :]).max() < 1e-4  # released: 5 ms to -60 dB


def test_notes_are_in_beats_and_follow_the_tempo(engine):
    engine.tempo = 60.0
    synth_track(engine, [(2.0, 1.0, 60)])
    out = engine.render_offline(0.0, 4 * SAMPLE_RATE)
    first = int(np.nonzero(np.abs(out[:, 0]) > 1e-6)[0][0])
    assert 2 * SAMPLE_RATE <= first < 2 * SAMPLE_RATE + 10
    assert np.abs(out[3 * SAMPLE_RATE + SAMPLE_RATE // 50 :]).max() < 1e-4


def test_velocity_sets_the_level(engine):
    track, _ = synth_track(engine, [(0.0, 2.0, 60, 127)])
    loud = rms(engine.render_offline(0.0, SPB)[SPB // 2 :, 0])
    engine.set_track_notes(track, [ge.NoteDesc(0.0, 2.0, 60, 64)])
    soft = rms(engine.render_offline(0.0, SPB)[SPB // 2 :, 0])
    assert soft / loud == pytest.approx(64 / 127, rel=0.01)


def test_chords_and_repeated_keys(engine):
    # A note ending where the next one on the same key starts doesn't cut it short.
    track, _ = synth_track(engine, [(0.0, 1.0, 60), (1.0, 1.0, 60)])
    out = engine.render_offline(0.0, 3 * SPB)
    assert rms(out[SPB + 1000 : 2 * SPB - 100, 0]) == pytest.approx(SINE_RMS, rel=0.02)
    assert np.abs(out[2 * SPB + SAMPLE_RATE // 50 :]).max() < 1e-4
    # Two keys at once sound together: the level adds up.
    engine.set_track_notes(track, [ge.NoteDesc(0.0, 1.0, 60), ge.NoteDesc(0.0, 1.0, 67)])
    chord = engine.render_offline(0.0, SPB)[SPB // 2 :, 0]
    assert rms(chord) == pytest.approx(np.sqrt(2) * SINE_RMS, rel=0.05)


def test_offline_renders_leave_no_hanging_notes(engine):
    synth_track(engine, [(0.0, 16.0, 60)])
    full = engine.render_offline(0.0, 2 * SPB)
    engine.render_offline(0.0, SPB // 2)  # stops in the middle of the note...
    # ...which must not keep sounding: rendering from later on (past the note-on) is silent.
    assert np.abs(engine.render_offline(4.0, SPB)).max() == 0.0
    np.testing.assert_array_equal(engine.render_offline(0.0, 2 * SPB), full)


def test_loop_wrap_releases_and_retriggers_notes(engine):
    synth_track(engine, [(1.0, 4.0, 60)], {RELEASE: 1.0})
    engine.set_loop(True, 1.0, 2.0)  # shorter than the note
    out = engine.render_offline(1.0, 3 * SPB, loop=True)[:, 0]
    # Each pass restarts the note (fresh attack) and ends it at the wrap.
    for wrap in (SPB, 2 * SPB):
        assert np.abs(out[wrap - 40 : wrap]).max() > 0.1  # still sounding right before the wrap
        assert np.abs(out[wrap + 60 : wrap + 70]).max() < np.abs(out[wrap + 2000 : wrap + 3000]).max() / 2
    np.testing.assert_allclose(out[SPB : 2 * SPB], out[2 * SPB :], atol=1e-4)


def test_instrument_output_goes_through_the_chain(engine):
    track, synth = synth_track(engine, [(0.0, 2.0, 60)])
    utility = engine.add_builtin_processor(track, "utility")
    engine.set_processor_param(utility, 0, -6.0206)  # half the level
    assert rms(engine.render_offline(0.0, SPB)[SPB // 2 :, 0]) == pytest.approx(SINE_RMS / 2, rel=0.02)
    engine.set_processor_enabled(synth, False)
    assert np.abs(engine.render_offline(0.0, SPB)).max() == 0.0
    engine.set_processor_enabled(synth, True)
    assert rms(engine.render_offline(0.0, SPB)[SPB // 2 :, 0]) == pytest.approx(SINE_RMS / 2, rel=0.02)


def test_notes_without_an_instrument_are_silent(engine):
    track = engine.add_track()
    engine.set_track_notes(track, [ge.NoteDesc(0.0, 1.0, 60)])
    assert np.abs(engine.render_offline(0.0, SPB)).max() == 0.0
    engine.add_builtin_processor(track, "synth")
    assert np.abs(engine.render_offline(0.0, SPB)).max() > 0.05


def test_cutoff_filters_the_saw(engine):
    _, synth = synth_track(engine, [(0.0, 2.0, 45)], {WAVE: 2})  # saw, 110 Hz

    def high_band_energy():
        out = engine.render_offline(0.0, SPB)[SPB // 2 :, 0]
        spectrum = np.abs(np.fft.rfft(out)) ** 2
        freqs = np.fft.rfftfreq(len(out), 1 / SAMPLE_RATE)
        return spectrum[freqs > 2000].sum() / spectrum.sum()

    open_filter = high_band_energy()
    engine.set_processor_param(synth, CUTOFF, 300.0)
    assert high_band_energy() < open_filter / 20  # 12 dB/octave


def test_synth_parameters(engine):
    track = engine.add_track()
    synth = engine.add_builtin_processor(track, "synth")
    params = engine.processor_params(synth)
    assert [p.id for p in params] == ["wave", "attack", "decay", "sustain", "release", "cutoff", "resonance",
                                      "volume"]
    assert params[WAVE].value_labels == ["Sine", "Triangle", "Saw", "Square"]
    assert [p.log_scale for p in params] == [False, True, True, False, True, True, False, False]
    engine.set_processor_param(synth, CUTOFF, 1e9)
    assert engine.processor_param(synth, CUTOFF) == 20000.0  # clamped


def test_preview_notes_need_a_running_device(engine):
    track, _ = synth_track(engine, [])
    engine.preview_note(track, 60, 100)  # no device: dropped, so it can't hang later
    assert np.abs(engine.render_offline(0.0, SPB)).max() == 0.0
    with pytest.raises(ValueError):
        engine.preview_note(track + 100, 60, 100)


def test_quick_preview_notes_all_end(live_engine):
    """Dragging a note across keys in the piano roll plays and releases notes
    faster than audio blocks go by. Several arrive in one block; each must end."""
    engine = live_engine
    track, _ = synth_track(engine, [], {RELEASE: 1.0})
    engine.play()  # the transport measures audio time
    engine.preview_note(track, 60, 100)
    wait_beats(engine, 0.25)
    engine.take_meters()
    wait_beats(engine, 0.25)
    assert max(m.left for m in engine.take_meters() if m.track_id == track) > 0.05  # heard

    for key in range(61, 73):  # as the dragged note moves up: release the last key, play the next
        engine.preview_note(track, key - 1, 0)
        engine.preview_note(track, key, 100)
    engine.preview_note(track, 72, 0)  # mouse up
    wait_beats(engine, 0.25)
    engine.take_meters()
    wait_beats(engine, 0.5)
    assert max(m.left for m in engine.take_meters() if m.track_id == track) < 1e-4  # nothing left sounding
