"""The built-in Compressor: its gain curve, its displays, and keying from a sidechain."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def add_clip_track(engine, path):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, 1.0, 0.0, 1.0)])
    return track


def compressor(engine, track, **values):
    device = engine.add_builtin_processor(engine.track_chain(track), "compressor")
    for name, value in values.items():
        engine.set_processor_param(device, engine.processor_param_index(device, name), value)
    return device


LEVEL_DB = 20 * np.log10(0.5)  # dc_wav's


def test_hard_knee_reduction_follows_the_ratio(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    compressor(engine, track, threshold=-18.0, ratio=4.0, knee=0.0, attack=1.0)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    reduction = (LEVEL_DB + 18.0) * (1 - 1 / 4)
    assert out[-1, 0] == pytest.approx(0.5 * 10 ** (-reduction / 20), rel=1e-3)


def test_below_the_knee_nothing_changes(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    compressor(engine, track, threshold=0.0, knee=6.0)  # -6 dB is just below the knee's start (-3 dB)
    out = engine.render_offline(0.0, 4800)
    assert out[-1, 0] == pytest.approx(0.5, rel=1e-5)


def test_displays_report_levels_and_reduction(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    device = compressor(engine, track, threshold=-18.0, ratio=4.0, knee=0.0, attack=1.0)
    displays = engine.processor_displays(device)
    assert [(d.id, d.samples_per_value) for d in displays] == [("input", 256), ("reduction", 256), ("output", 256)]

    engine.render_offline(0.0, 256 * 100)
    reduction, position = engine.read_processor_display(device, 1)
    assert position == 100 and reduction.dtype == np.float32 and len(reduction) == 100
    assert reduction[-1] == pytest.approx((LEVEL_DB + 18.0) * 0.75, abs=0.01)
    level, _ = engine.read_processor_display(device, 0)
    assert level[-1] == pytest.approx(LEVEL_DB, abs=0.01)

    # A reader picks up where it left off; one that fell behind gets the latest 8192.
    engine.render_offline(0.0, 256 * 10)
    more, position = engine.read_processor_display(device, 1, position)
    assert len(more) == 10 and position == 110
    assert len(engine.read_processor_display(device, 1, position)[0]) == 0
    engine.render_offline(0.0, 256 * 9000)
    assert len(engine.read_processor_display(device, 1, position)[0]) == 8192


def test_sidechain_keys_the_compressor(engine, dc_wav, make_wav):
    track = add_clip_track(engine, dc_wav)
    key = add_clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 1.0)))
    engine.set_track_gain(key, 0.0)  # heard only through the sidechain, taken before its fader
    device = compressor(engine, track, threshold=-18.0, ratio=4.0, knee=0.0, attack=1.0)
    engine.set_processor_sidechain(device, key, ge.SidechainTap.PRE_FADER)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    assert out[-1, 0] == pytest.approx(0.5 * 10 ** (-18.0 * 0.75 / 20), rel=1e-3)  # keyed at 0 dB

    # A silent sidechain keys nothing: the compressor doesn't fall back to its own input.
    engine.set_processor_sidechain(device, engine.add_track(), ge.SidechainTap.PRE_FADER)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    assert out[-1, 0] == pytest.approx(0.5, rel=1e-4)

    engine.clear_processor_sidechain(device)  # without one, its own input keys it
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    assert out[-1, 0] == pytest.approx(0.5 * 10 ** (-(LEVEL_DB + 18.0) * 0.75 / 20), rel=1e-3)


def test_attack_holds_across_a_low_notes_cycles(engine, make_wav):
    """The reduction reaches its level within a few attack times on a low sine too, not only on DC."""
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    tone = 0.9 * np.sin(2 * np.pi * 55.0 * t)
    track = add_clip_track(engine, make_wav(np.stack([tone, tone], axis=1)))
    device = compressor(engine, track, threshold=-24.0, ratio=4.0, knee=0.0, attack=10.0)
    engine.render_offline(0.0, SAMPLE_RATE // 2)
    reduction, _ = engine.read_processor_display(device, 1)
    steady = (20 * np.log10(0.9) + 24.0) * 0.75
    after_50ms = reduction[50 * SAMPLE_RATE // 1000 // 256]
    assert after_50ms > 0.9 * steady and reduction[-1] == pytest.approx(steady, abs=0.1)
