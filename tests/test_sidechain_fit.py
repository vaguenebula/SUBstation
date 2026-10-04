"""Fitting the Sidechain device's curve to a kick (analysis/sidechain_fit): the clash between the kick and
the input, the kick's envelope there, the reduction it calls for, the points, and capturing hits."""

import numpy as np
import pytest

from substation.analysis import sidechain_fit as sf
from substation.model import automation

RATE = 48000


def kick(seconds=0.6, tail=0.12, pre=0):
    """A kick: a sine falling from 170 Hz to 50 Hz, dying away over `tail` seconds."""
    t = np.arange(int(seconds * RATE)) / RATE
    freq = 50 + 120 * np.exp(-t / 0.02)
    return np.concatenate([np.zeros(pre), np.sin(2 * np.pi * np.cumsum(freq) / RATE) * np.exp(-t / tail)])


def bass(*freqs, seconds=2.0):
    t = np.arange(int(seconds * RATE)) / RATE
    return sum(0.4 * np.sin(2 * np.pi * f * t) for f in freqs)


def test_clash_is_where_both_are_loud():
    found = sf.spectra(kick()[:int(0.3 * RATE)], bass(55, 600), RATE)
    assert found.bass_heard
    assert found.low < 55 < found.high < 120  # the bass's 600 Hz doesn't clash: the kick has none there
    assert 45 < found.peak < 70
    assert found.kick.max() == pytest.approx(0) and found.bass.max() == pytest.approx(0)
    assert found.clash.max() == pytest.approx(0) and len(found.freqs) == sf.COLUMNS

    # A bass an octave up clashes where the kick has that octave: higher.
    higher = sf.spectra(kick()[:int(0.3 * RATE)], bass(120), RATE)
    assert higher.peak > found.peak


def test_without_a_bass_the_kick_alone():
    found = sf.spectra(kick(), np.zeros(RATE), RATE)
    assert not found.bass_heard and np.all(found.bass == sf.FLOOR_DB)
    assert np.array_equal(found.clash, found.kick) and found.low < 55 < found.high


def test_reduction_holds_to_the_peak_then_follows_the_decay():
    times = np.arange(1000)
    envelope = np.where(times < 100, times / 100, np.exp(-(times - 100) / 200))  # up to its peak at 100
    amount = sf.reduction(envelope, 20.0)
    assert np.all(amount[:101] == 1.0)
    assert np.all(np.diff(amount[100:]) <= 1e-12)  # never back up
    gone = 100 + 200 * np.log(10)  # 20 dB down
    assert amount[int(gone) + 2] == 0.0 and amount[int(gone) - 50] > 0
    assert sf.reduction(np.zeros(10), 20.0).tolist() == [0.0] * 10


def test_curve_at_bends_as_automation():
    points = ((0.0, 0.0, 0.5), (0.4, 1.0, -0.3), (1.0, 0.2, 0.0))
    xs = np.linspace(0, 1, 101)
    ours = sf.curve_at(points, xs)
    model = [automation.AutomationPoint(x, y, c) for x, y, c in points]
    for x, value in zip(xs, ours, strict=True):
        i = max(0, min(len(model) - 2, automation.count_at_or_before(model, x) - 1))
        assert value == pytest.approx(automation.segment_value(model[i], model[i + 1], x), abs=1e-9)


def test_fit_points_finds_a_curve_again():
    points = ((0.0, 0.0, 0.0), (0.15, 0.0, -0.5), (0.6, 0.8, 0.4), (1.0, 1.0, 0.0))
    xs = np.linspace(0, 1, 400)
    fitted = sf.fit_points(xs, sf.curve_at(points, xs))
    assert fitted[0][0] == 0.0 and fitted[-1][0] == 1.0
    assert len(fitted) <= 6
    assert np.abs(sf.curve_at(fitted, xs) - sf.curve_at(points, xs)).max() <= sf.TOLERANCE + 1e-9
    assert sf.fit_points(xs, np.ones(400)) == ((0.0, 1.0, 0.0), (1.0, 1.0, 0.0))  # flat: the ends


def test_analyze_fits_the_kick():
    pre = int(sf.PRE_SECONDS * RATE)
    fits = [sf.analyze([kick(pre=pre)], bass(55), RATE, character, pre) for character in range(3)]
    tight, natural, loose = fits
    assert tight.length < natural.length < loose.length  # Loose keeps out of the way for longer
    for fit in fits:
        assert sf.LENGTH_MIN <= fit.length <= sf.LENGTH_MAX
        assert fit.points[0][:2] == (0.0, 0.0)  # ducked from the hit
        assert fit.points[-1][:2] == (1.0, 1.0)  # and back by the end
        assert 3 <= len(fit.points) <= sf.MAX_POINTS
        assert fit.envelope.max() == pytest.approx(1.0) and len(fit.times) == len(fit.target)
        xs = np.linspace(0, 1, 400)
        wanted = np.interp(xs * fit.length, fit.times, fit.target, right=1.0)
        assert np.abs(sf.curve_at(fit.points, xs) - wanted).max() < 0.05
    # The kick dies away (to -20 dB, Natural) after about 0.12 * ln(10) s from its peak.
    assert 250 < natural.length < 420

    assert sf.analyze([np.zeros(RATE)], bass(55), RATE) is None  # nothing to fit to
    assert sf.analyze([], bass(55), RATE) is None


def test_capture_hands_out_each_hit():
    capture = sf.Capture(RATE, keep_seconds=4.0)
    pre = int(sf.PRE_SECONDS * RATE)
    total = 3 * RATE
    key, inputs, phase = np.zeros(total), np.full(total, 0.25), np.full(total, -1.0)
    hits = (RATE // 2, RATE // 2 + 12000)  # the second 250 ms after the first: the first is cut there
    for hit in hits:
        key[hit:hit + 2000] = 0.9
        phase[hit:hit + 4800] = np.arange(4800)
    taken = []
    for start in range(0, total, 800):  # as the displays come, a refresh at a time
        for name, values in (("key", key), ("input", inputs), ("phase", phase)):
            capture.feed(name, start, values[start:start + 800])
        taken += capture.hits()
    assert capture.hit_count == 2 and len(taken) == 2
    first, second = taken
    assert len(first[0]) == pre + 12000 and first[0][pre] == pytest.approx(0.9) and first[0][pre - 1] == 0.0
    assert len(second[0]) == pre + int(sf.KICK_SECONDS * RATE)
    assert np.all(first[1] == 0.25) and len(first[1]) == hits[0] + 12000  # the input since it started (< 2 s)
    assert capture.take_key_peak() == pytest.approx(0.9) and capture.take_key_peak() == 0.0

    # After a gap (the editor hidden), it starts again from what comes.
    capture.feed("phase", 10 * RATE, np.zeros(1))
    capture.feed("key", 10 * RATE, np.ones(1))
    assert capture.streams["key"].start == 10 * RATE and capture.hits() == []
    assert capture.last_phase == 0.0
    capture.feed("phase", 10 * RATE + 1, np.zeros(0))
    assert capture.last_phase == -1.0  # nothing came: no playhead
