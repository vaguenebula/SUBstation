"""The built-in Sampler: its sample (its state besides its parameters), playing
it across the keyboard, and swapping it while it plays. Rendered offline."""

import numpy as np
import pytest

from substation import _engine as ge
from substation.model import device_state

from .conftest import SAMPLE_RATE
from .test_midi_engine import dominant_freq, rms

SPB = int(SAMPLE_RATE * 60 / 120)  # samples per beat at 120 BPM


@pytest.fixture
def engine():
    e = ge.Engine()
    yield e
    e.close_device()


def sine(freq, seconds=1.0, rate=SAMPLE_RATE, level=0.5):
    t = np.arange(int(rate * seconds)) / rate
    return level * np.sin(2 * np.pi * freq * t)


def state(path: str) -> bytes:
    return device_state.encode({"sample": path})


def sampler_track(engine, notes, path=None, **values):
    """A track with a Sampler playing `notes` ((start beat, length beats, key[, velocity]) each)."""
    track = engine.add_track()
    device = engine.add_builtin_processor(engine.track_chain(track), "sampler")
    for name, value in ({"velocity": 0.0, "release": 1.0} | values).items():
        engine.set_processor_param(device, engine.processor_param_index(device, name), value)
    if path is not None:
        engine.set_processor_state(device, state(path))
    engine.set_track_notes(track, [ge.NoteDesc(*note) for note in notes])
    return track, device


def test_is_an_instrument_listed_with_its_parameters():
    info = next(d for d in ge.builtin_devices() if d.id == "sampler")
    assert info.name == "Sampler" and info.is_instrument
    assert [p.id for p in info.params] == ["root", "tune", "fine", "start", "end", "loop",
                                           "attack", "decay", "sustain", "release", "velocity", "volume"]


def test_without_a_sample_it_is_silent_and_has_no_state(engine):
    _, device = sampler_track(engine, [(0.0, 1.0, 60)])
    assert engine.processor_state(device) == b""
    assert not np.any(engine.render_offline(0.0, SPB))


def test_state_round_trips_its_path(engine, make_wav):
    path = make_wav(sine(440))  # a Windows path: backslashes, escaped and back
    _, device = sampler_track(engine, [], path)
    assert engine.processor_state(device) == state(path)
    assert device_state.decode(engine.processor_state(device)) == {"sample": path}


def test_state_text_escapes_backslashes_and_newlines():
    values = {"a": "C:\\x\\n", "b": "two\nlines", "c": ""}
    assert device_state.encode(values) == b"a=C:\\\\x\\\\n\nb=two\\nlines\nc=\n"
    assert device_state.decode(device_state.encode(values)) == values
    assert device_state.from_model(device_state.to_model(values)) == values
    assert device_state.to_model({}) is None and device_state.from_model("not base64!") == {}


def test_plays_the_sample_at_its_root_key(engine, make_wav):
    path = make_wav(sine(440))
    sampler_track(engine, [(0.0, 1.0, 60, 127)], path)
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:, 0]) == pytest.approx(440, rel=1e-3)
    assert rms(out[1000:, 0]) == pytest.approx(0.5 / np.sqrt(2), rel=0.02)
    assert np.allclose(out[:, 0], out[:, 1])  # a mono sample on both channels


@pytest.mark.parametrize(("key", "values", "freq"), [
    (72, {}, 880),                       # an octave up
    (60, {"tune": -12.0}, 220),          # transposed down
    (60, {"root": 48.0}, 880),           # played an octave above its root
    (60, {"fine": 100.0}, 440 * 2 ** (1 / 12)),
])
def test_pitch_follows_key_root_and_tuning(engine, make_wav, key, values, freq):
    path = make_wav(sine(440))
    sampler_track(engine, [(0.0, 1.0, key, 127)], path, **values)
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:, 0]) == pytest.approx(freq, rel=2e-3)


def test_a_file_at_another_rate_plays_at_its_pitch(engine, make_wav):
    path = make_wav(sine(440, rate=22050), sample_rate=22050)
    sampler_track(engine, [(0.0, 1.0, 60, 127)], path)
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:, 0]) == pytest.approx(440, rel=2e-3)


def test_a_note_ends_with_the_sample_unless_it_loops(engine, make_wav):
    path = make_wav(sine(440, seconds=0.1))  # shorter than the note
    _, device = sampler_track(engine, [(0.0, 2.0, 60, 127)], path)
    out = engine.render_offline(0.0, SPB)
    assert rms(out[2000:4000, 0]) > 0.3
    assert not np.any(out[int(0.11 * SAMPLE_RATE):, 0])

    engine.set_processor_param(device, engine.processor_param_index(device, "loop"), 1.0)
    out = engine.render_offline(0.0, SPB)
    assert rms(out[SPB - 4800:, 0]) > 0.3  # still going, half a second in


def test_start_and_end_choose_the_part_played(engine, make_wav):
    # Half a second of 440 Hz, then half a second of 880 Hz: start in the second half.
    path = make_wav(np.concatenate([sine(440, 0.5), sine(880, 0.5)]))
    sampler_track(engine, [(0.0, 1.0, 60, 127)], path, start=50.0)
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:20000, 0]) == pytest.approx(880, rel=2e-3)
    assert not np.any(out[int(0.51 * SAMPLE_RATE):, 0])


def test_velocity_sensitivity(engine, make_wav):
    path = make_wav(sine(440))
    sampler_track(engine, [(0.0, 1.0, 60, 64)], path, velocity=100.0)
    out = engine.render_offline(0.0, SPB)
    assert rms(out[1000:, 0]) == pytest.approx(0.5 / np.sqrt(2) * 64 / 127, rel=0.02)


def test_swapping_the_sample_while_it_plays(engine, make_wav):
    low, high = make_wav(sine(440)), make_wav(sine(880))
    _, device = sampler_track(engine, [(0.0, 8.0, 60, 127)], low, loop=1.0)
    first = engine.render_offline(0.0, SPB)
    assert dominant_freq(first[1000:, 0]) == pytest.approx(440, rel=2e-3)

    # The playing note stops with its sample; the next one plays the new sample.
    engine.set_processor_state(device, state(high))
    assert not np.any(engine.render_offline(0.5, SPB))
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:, 0]) == pytest.approx(880, rel=2e-3)

    # Many swaps, faster than blocks render: the last one wins.
    for n in range(40):
        engine.set_processor_state(device, state(low if n % 2 else high))
        if n % 3 == 0:
            engine.render_offline(0.0, 256)
    engine.idle()
    out = engine.render_offline(0.0, SPB)
    assert dominant_freq(out[1000:, 0]) == pytest.approx(440, rel=2e-3)

    engine.set_processor_state(device, b"")  # no sample
    assert engine.processor_state(device) == b""
    assert not np.any(engine.render_offline(0.0, SPB))


def test_a_missing_file_is_an_error_but_stays_in_its_state(engine, tmp_path):
    missing = str(tmp_path / "gone.wav")
    _, device = sampler_track(engine, [(0.0, 1.0, 60)])
    with pytest.raises(RuntimeError):
        engine.set_processor_state(device, state(missing))
    assert engine.processor_state(device) == state(missing)
    assert not np.any(engine.render_offline(0.0, SPB))


def test_unknown_state_values_are_ignored(engine, make_wav):
    path = make_wav(sine(440))
    _, device = sampler_track(engine, [(0.0, 1.0, 60, 127)])
    engine.set_processor_state(device, b"from_the_future=1\n" + state(path) + b"no equals sign\n")
    assert rms(engine.render_offline(0.0, SPB)[1000:, 0]) > 0.3


def test_position_display_follows_the_note(engine, make_wav):
    path = make_wav(sine(440))  # one second
    _, device = sampler_track(engine, [(0.0, 1.0, 60, 127)], path)
    assert [(d.id, d.samples_per_value) for d in engine.processor_displays(device)] == [("position", 256)]
    engine.render_offline(0.0, SPB + 4800)  # the note plays half of it, then releases (1 ms)
    values, _ = engine.read_processor_display(device, 0)
    playing = values[values >= 0]
    assert len(playing) > 80 and np.all(np.diff(playing) > 0)
    assert playing[-1] == pytest.approx(0.5, abs=0.02)
    assert values[-1] == -1.0  # none plays at the end
