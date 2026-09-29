"""Warping in the engine: tempo-following clips, pitch shifting and Re-Pitch.
Rendered offline, so no audio device is needed."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE

SPB = SAMPLE_RATE * 60 / 120  # samples per beat at 120 BPM
SINE_RMS = 0.5 / np.sqrt(2)  # of the 0.5-amplitude test tones


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def sine(freq, seconds=2.0, amplitude=0.5, channels=1):
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    wave = amplitude * np.sin(2 * np.pi * freq * t)
    return wave if channels == 1 else np.column_stack([wave] * channels)


def burst_at(seconds, total=2.0, freq=1000.0):
    """Silence, then a sine burst from `seconds` on: a sharp, easy-to-find onset."""
    wave = sine(freq, total)
    wave[: int(seconds * SAMPLE_RATE)] = 0.0
    return wave


def add_clip(engine, path, start_beat=0.0, duration_sec=2.0, **kwargs):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, duration_sec, 0.0, 1.0, **kwargs)])
    return track


def dominant_freq(samples):
    window = np.hanning(len(samples))
    spectrum = np.abs(np.fft.rfft(samples * window))
    peak = int(np.argmax(spectrum))
    # Parabolic interpolation around the peak bin.
    a, b, c = np.log(spectrum[peak - 1 : peak + 2] + 1e-12)
    offset = 0.5 * (a - c) / (a - 2 * b + c)
    return (peak + offset) * SAMPLE_RATE / len(samples)


def audible(out, threshold=1e-3):
    nonzero = np.nonzero(np.abs(out[:, 0]) > threshold)[0]
    return int(nonzero[0]), int(nonzero[-1]) + 1


def envelope(samples, width=48):
    return np.sqrt(np.convolve(samples**2, np.ones(width) / width, mode="same"))


def onset(out, level=SINE_RMS):
    """Where the (1 ms RMS) envelope first reaches half of `level`. Phase-vocoder
    output smears a little before transients, so the raw first sample is early."""
    return int(np.argmax(envelope(out[:, 0]) > 0.5 * level))


def test_transpose_shifts_pitch_but_not_length(engine, make_wav):
    add_clip(engine, make_wav(sine(440.0)), transpose=12.0)
    out = engine.render_offline(0.0, 3 * SAMPLE_RATE)
    first, last = audible(out)
    assert first < 200 and abs(last - 2 * SAMPLE_RATE) < 200
    assert dominant_freq(out[SAMPLE_RATE // 2 : SAMPLE_RATE // 2 + 16384, 0]) == pytest.approx(880.0, rel=0.005)


def test_detune_in_fractional_semitones(engine, make_wav):
    add_clip(engine, make_wav(sine(440.0)), transpose=-0.5)  # -50 cents
    out = engine.render_offline(0.0, 2 * SAMPLE_RATE)
    freq = dominant_freq(out[SAMPLE_RATE // 2 : SAMPLE_RATE // 2 + 32768, 0])
    assert freq == pytest.approx(440.0 * 2 ** (-0.5 / 12), rel=0.002)


@pytest.mark.parametrize("mode", [ge.WarpMode.BEATS, ge.WarpMode.TONES, ge.WarpMode.TEXTURE,
                                  ge.WarpMode.COMPLEX, ge.WarpMode.COMPLEX_PRO])
def test_warped_clip_follows_tempo_and_keeps_pitch(engine, make_wav, mode):
    # 2 s of audio at 60 BPM is 2 beats: at 120 BPM that takes 1 s, at the same pitch.
    add_clip(engine, make_wav(sine(440.0)), start_beat=1.0, warp=True, segment_bpm=60.0, warp_mode=mode)
    out = engine.render_offline(0.0, 3 * SAMPLE_RATE)
    first, last = audible(out)
    assert first == pytest.approx(SPB, abs=2)
    assert last == int(3 * SPB)  # ends exactly on beat 3
    middle = int(SPB) + SAMPLE_RATE // 4
    assert dominant_freq(out[middle : middle + 16384, 0]) == pytest.approx(440.0, rel=0.005)
    assert np.sqrt(np.mean(out[middle : middle + 16384, 0] ** 2)) == pytest.approx(SINE_RMS, rel=0.15)


def test_warped_clip_slows_down(engine, make_wav):
    add_clip(engine, make_wav(sine(440.0)), warp=True, segment_bpm=240.0)  # 2 s -> 8 beats... at 120: 4 s
    out = engine.render_offline(0.0, 5 * SAMPLE_RATE)
    _, last = audible(out)
    assert last == 4 * SAMPLE_RATE
    assert dominant_freq(out[SAMPLE_RATE : SAMPLE_RATE + 16384, 0]) == pytest.approx(440.0, rel=0.005)


def test_warped_clip_at_its_own_tempo_is_bit_exact(engine, ramp_wav):
    add_clip(engine, ramp_wav, duration_sec=1.0, warp=True, segment_bpm=120.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    expected = (np.arange(SAMPLE_RATE) % 32768) / 32768.0
    np.testing.assert_array_equal(out[:, 0], expected.astype(np.float32))


def test_tempo_change_rescales_warped_clips_only(engine, make_wav):
    path = make_wav(sine(440.0, 1.0))
    add_clip(engine, path, duration_sec=1.0, warp=True, segment_bpm=120.0)  # 2 beats
    add_clip(engine, path, start_beat=8.0, duration_sec=1.0)  # unwarped: 1 s
    engine.tempo = 90.0
    spb = SAMPLE_RATE * 60 / 90
    out = engine.render_offline(0.0, int(12 * spb))
    warped = out[: int(4 * spb)]
    assert audible(warped)[1] == round(2 * spb)  # still 2 beats long, now 1.33 s
    assert audible(out[int(8 * spb) :])[1] == pytest.approx(SAMPLE_RATE, abs=1)


def test_warped_clip_is_sample_aligned(engine, make_wav):
    # The burst starts 0.5 s into the source; at double speed it must be heard
    # 0.25 s after the clip start, with no stretcher latency.
    add_clip(engine, make_wav(burst_at(0.5)), start_beat=2.0, warp=True, segment_bpm=60.0,
             warp_mode=ge.WarpMode.BEATS)
    out = engine.render_offline(0.0, 4 * SAMPLE_RATE)
    expected = int(2 * SPB + 0.25 * SAMPLE_RATE)
    assert abs(onset(out) - expected) < 0.004 * SAMPLE_RATE


def test_transposed_clip_is_sample_aligned(engine, make_wav):
    add_clip(engine, make_wav(burst_at(0.5)), start_beat=2.0, transpose=-5.0)
    out = engine.render_offline(0.0, 4 * SAMPLE_RATE)
    assert abs(onset(out) - int(2 * SPB + 0.5 * SAMPLE_RATE)) < 0.004 * SAMPLE_RATE


def test_starting_inside_a_warped_clip_matches_playing_through(engine, make_wav):
    # A locate into the middle of a stretched clip re-seeks the stretcher; the
    # bursts must land where a continuous playthrough puts them. (The waveforms
    # differ in phase, as stretched audio does; the timing must not.)
    pulses = sine(1000.0)
    pulses[(np.arange(len(pulses)) % (SAMPLE_RATE // 4)) >= SAMPLE_RATE // 20] = 0.0  # 50 ms every 250 ms
    add_clip(engine, make_wav(pulses), warp=True, segment_bpm=100.0)
    full = engine.render_offline(0.0, 2 * SAMPLE_RATE)
    start = int(1.5 * SPB)
    part = engine.render_offline(1.5, SAMPLE_RATE // 2)
    a = envelope(full[start + 2000 : start + 22000, 0], 240)
    b = envelope(part[2000:22000, 0], 240)
    assert np.corrcoef(a, b)[0, 1] > 0.97


def test_offline_renders_are_repeatable(engine, make_wav):
    rng = np.random.default_rng(3)
    add_clip(engine, make_wav(rng.uniform(-0.5, 0.5, SAMPLE_RATE)), duration_sec=1.0, warp=True,
             segment_bpm=77.0, transpose=3.0, warp_mode=ge.WarpMode.TEXTURE)
    first = engine.render_offline(0.0, SAMPLE_RATE)
    second = engine.render_offline(0.0, SAMPLE_RATE)
    np.testing.assert_array_equal(first, second)


def test_repitch_changes_speed_and_pitch_together(engine, make_wav):
    add_clip(engine, make_wav(sine(440.0)), warp=True, segment_bpm=60.0, warp_mode=ge.WarpMode.RE_PITCH,
             transpose=7.0)  # transpose is ignored in Re-Pitch
    out = engine.render_offline(0.0, 2 * SAMPLE_RATE)
    assert audible(out)[1] == SAMPLE_RATE
    assert dominant_freq(out[SAMPLE_RATE // 4 : SAMPLE_RATE // 4 + 16384, 0]) == pytest.approx(880.0, rel=0.002)


def test_repitch_filters_instead_of_aliasing(engine, make_wav):
    # 15 kHz at double speed would be 30 kHz, above Nyquist: it must be filtered
    # out, not fold back down to 18 kHz.
    add_clip(engine, make_wav(sine(15000.0)), warp=True, segment_bpm=60.0, warp_mode=ge.WarpMode.RE_PITCH)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    middle = out[SAMPLE_RATE // 4 : SAMPLE_RATE // 4 + 8192, 0]
    assert np.max(np.abs(middle)) < 0.02


def test_stretching_stereo_and_mono_sources(engine, make_wav):
    stereo = np.column_stack([sine(440.0), sine(660.0)])
    add_clip(engine, make_wav(stereo), warp=True, segment_bpm=90.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    window = out[SAMPLE_RATE // 4 : SAMPLE_RATE // 4 + 16384]
    assert dominant_freq(window[:, 0]) == pytest.approx(440.0, rel=0.005)
    assert dominant_freq(window[:, 1]) == pytest.approx(660.0, rel=0.005)


def test_clip_pan(engine, dc_wav):
    add_clip(engine, dc_wav, duration_sec=1.0, pan=-1.0)
    out = engine.render_offline(0.0, 1000)
    assert out[500] == pytest.approx([0.5, 0.0], abs=1e-6)


def test_many_warped_clips_in_sequence(engine, make_wav):
    # Back-to-back warped clips share a few stretch voices; each must still play.
    path = make_wav(sine(440.0, 1.0))
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [
        ge.ClipDesc(path, float(beat), 0.25, 0.0, 1.0, warp=True, segment_bpm=60.0, id=f"c{beat}")
        for beat in range(0, 16, 2)
    ])  # each: 0.25 s of audio at 60 BPM = 0.25 beats
    out = engine.render_offline(0.0, int(16 * SPB))
    for beat in range(0, 16, 2):
        piece = out[int(beat * SPB) + 200 : int((beat + 0.25) * SPB) - 200, 0]
        assert np.sqrt(np.mean(piece**2)) > 0.2, beat
        assert np.all(out[int((beat + 0.25) * SPB) + 1 : int((beat + 1) * SPB), 0] == 0.0)


def test_export_includes_warped_audio(engine, make_wav, tmp_path):
    add_clip(engine, make_wav(sine(440.0)), warp=True, segment_bpm=60.0, transpose=12.0)
    path = tmp_path / "out.wav"
    engine.export_wav(str(path), 0.0, 2.0, 16)
    import wave

    with wave.open(str(path)) as f:
        frames = np.frombuffer(f.readframes(f.getnframes()), dtype="<i2").reshape(-1, 2) / 32768.0
    assert len(frames) == SAMPLE_RATE
    assert dominant_freq(frames[SAMPLE_RATE // 4 : SAMPLE_RATE // 4 + 16384, 0]) == pytest.approx(880.0, rel=0.005)
