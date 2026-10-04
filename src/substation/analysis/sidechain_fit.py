"""Fitting the Sidechain device's curve to a kick: where (in frequency) the
kick and the input (a bass) clash, and how the kick's energy there rises and
dies away; the curve ducks the input just as much and as long as that.

- `Capture` keeps what the device's displays stream (the key, the input and
  the samples since each hit, in step, by their absolute sample index) and
  hands out each hit's kick once enough of it has come.
- `spectra()` compares the kick's spectrum with the input's: their clash is
  the geometric mean of the two (each against its own loudest), so it is loud
  only where both are; its band is where it is within CLASH_DB of its peak.
- `analyze()` weights the kick by that clash (zero-phase, in the frequency
  domain), takes its envelope (the analytic signal's magnitude), and turns it
  into the reduction a curve needs: full while the kick is at its peak there,
  none once it is RANGE_DB below it (per character: Tight ducks only while the
  kick is loud, Loose for as long as it lingers). The curve's length is where
  that ends, and its points come from fit_points().
- `fit_points()` approximates a curve with as few breakpoints as it takes
  (adding one where it is furthest off), each segment bent as automation is
  (model.automation.shape), the bend that fits best.

Pure numpy, and no Qt: the editor (ui/device_editors/sidechain.py) does the
drawing and writes the result to the device's parameters."""

from __future__ import annotations

from dataclasses import dataclass
from itertools import pairwise

import numpy as np

from ..model.automation import CURVATURE

CHARACTERS = ("Tight", "Natural", "Loose")
RANGE_DB = (12.0, 20.0, 30.0)  # per character: how far below its peak the kick still ducks the input
CLASH_LOW, CLASH_HIGH = 20.0, 2000.0  # Hz: where a clash is looked for
CLASH_DB = 10.0  # the clash band: where the clash is within this of its peak
COLUMNS = 160  # spectra's points, log-spaced from CLASH_LOW to CLASH_HIGH
SMOOTH_OCTAVES = 1 / 6  # spectra are averaged over this much either side
FLOOR_DB = -80.0
MAX_POINTS = 10
TOLERANCE = 0.02  # fit_points() stops once the curve is this close everywhere
KICK_SECONDS = 0.8  # of each hit, at most (less when the next comes first)
PRE_SECONDS = 0.005  # kept before each hit (for filtering it)
BASS_SECONDS = 2.0  # of the input before a hit, for its spectrum
FFT_SIZE = 8192
DECAY_SMOOTH_SECONDS = 0.04  # the envelope's decay (in dB) is smoothed over this
LENGTH_MIN, LENGTH_MAX = 10.0, 2000.0  # ms, as the device's `length`


@dataclass(frozen=True)
class Spectra:
    freqs: np.ndarray  # Hz, COLUMNS log-spaced
    kick: np.ndarray  # dB against its loudest
    bass: np.ndarray  # dB against its loudest (FLOOR_DB everywhere if there was none)
    clash: np.ndarray  # dB against its loudest
    low: float  # the clash band, Hz
    high: float
    peak: float
    bass_heard: bool


@dataclass(frozen=True)
class Fit:
    spectra: Spectra
    times: np.ndarray  # ms from the hit
    envelope: np.ndarray  # the kick's envelope in the clash band, 0..1 (linear, against its peak)
    target: np.ndarray  # the curve the envelope calls for (1: untouched, 0: ducked), at `times`
    length: float  # ms
    points: tuple[tuple[float, float, float], ...]  # (x 0..1 of the length, y, curve)


# --- Spectra -----------------------------------------------------------------------------------


def _power(samples: np.ndarray) -> np.ndarray:
    """Mean power per FFT bin (Hann windows, half overlapping; one zero-padded if short)."""
    window = np.hanning(FFT_SIZE)
    if len(samples) <= FFT_SIZE:
        frames = [np.pad(samples, (0, FFT_SIZE - len(samples)))]
    else:
        frames = [samples[i:i + FFT_SIZE] for i in range(0, len(samples) - FFT_SIZE + 1, FFT_SIZE // 2)]
    return np.mean([np.abs(np.fft.rfft(frame * window)) ** 2 for frame in frames], axis=0)


def _columns(power: np.ndarray, sample_rate: float, freqs: np.ndarray) -> np.ndarray:
    """Power at each frequency: the mean of the bins within SMOOTH_OCTAVES of it."""
    bin_width = sample_rate / FFT_SIZE
    total = np.concatenate(([0.0], np.cumsum(power)))
    low = np.clip(np.floor(freqs * 2 ** -SMOOTH_OCTAVES / bin_width).astype(int), 0, len(power) - 1)
    high = np.clip(np.ceil(freqs * 2 ** SMOOTH_OCTAVES / bin_width).astype(int), low + 1, len(power))
    return (total[high] - total[low]) / (high - low)


def _relative_db(power: np.ndarray) -> np.ndarray:
    peak = float(power.max()) if len(power) else 0.0
    if peak <= 0.0:
        return np.full(len(power), FLOOR_DB)
    return np.maximum(10.0 * np.log10(np.maximum(power / peak, 1e-30)), FLOOR_DB)


def spectra(kick: np.ndarray, bass: np.ndarray, sample_rate: float) -> Spectra:
    """The kick's and the bass's spectra, where they clash, and the band they clash most in."""
    freqs = np.geomspace(CLASH_LOW, CLASH_HIGH, COLUMNS)
    kick_power = _columns(_power(np.asarray(kick, np.float64)), sample_rate, freqs)
    bass = np.asarray(bass, np.float64)
    bass_heard = len(bass) > 0 and float(np.sqrt(np.mean(bass ** 2))) > 1e-4  # above -80 dBFS
    bass_power = _columns(_power(bass), sample_rate, freqs) if bass_heard else np.zeros(COLUMNS)
    kick_db, bass_db = _relative_db(kick_power), _relative_db(bass_power)
    # Without a bass, the kick alone: where it is loudest is where it would clash.
    clash_db = _relative_db(np.sqrt(kick_power * bass_power)) if bass_heard else kick_db
    peak = int(np.argmax(clash_db))
    low = high = peak
    while low > 0 and clash_db[low - 1] > -CLASH_DB:
        low -= 1
    while high < COLUMNS - 1 and clash_db[high + 1] > -CLASH_DB:
        high += 1
    return Spectra(freqs, kick_db, bass_db, clash_db, float(freqs[low]), float(freqs[high]), float(freqs[peak]),
                   bass_heard)


# --- The envelope ------------------------------------------------------------------------------


def clash_envelope(kick: np.ndarray, sample_rate: float, found: Spectra, pre: int = 0) -> np.ndarray:
    """The kick's envelope where it clashes: weighted by the clash (in amplitude, zero-phase), the
    analytic signal's magnitude, smoothed over a few ms. From `pre` samples in (the hit) on."""
    kick = np.asarray(kick, np.float64)
    size = 1 << int(np.ceil(np.log2(max(2, 2 * len(kick)))))
    spectrum = np.fft.rfft(kick, size)
    bins = np.fft.rfftfreq(size, 1.0 / sample_rate)
    # The clash in dB (a power ratio) is the weight in amplitude at /20; nothing outside the band's surroundings.
    weight_db = np.interp(np.log(np.maximum(bins, 1.0)), np.log(found.freqs), np.maximum(found.clash, -40.0),
                          left=-120.0, right=-120.0)
    analytic = np.zeros(size, complex)
    analytic[:size // 2 + 1] = spectrum * 10.0 ** (weight_db / 20.0)
    analytic[1:size // 2] *= 2.0
    envelope = np.abs(np.fft.ifft(analytic))[:len(kick)]
    # Smoothed over a cycle of the band's bottom (5..25 ms): no ripple from its partials beating.
    width = int(sample_rate * float(np.clip(1.0 / max(found.low, 1.0), 0.005, 0.025)))
    return _smoothed(envelope, width)[pre:]


def _smoothed(values: np.ndarray, width: int) -> np.ndarray:
    """A Hann-weighted moving average, the ends held."""
    if width < 3 or len(values) < 2:
        return values
    window = np.hanning(width + 2)[1:-1]
    padded = np.pad(values, width // 2 + 1, mode="edge")
    return np.convolve(padded, window / window.sum(), "same")[width // 2 + 1:width // 2 + 1 + len(values)]


def reduction(envelope: np.ndarray, range_db: float, smooth: int = 0) -> np.ndarray:
    """How much the input must give way (0..1) along the kick's envelope: all of it from the hit to
    the envelope's peak (a kick whose pitch falls into the band gets there late, but the input must
    be out of the way of its attack too), then less as it falls, none `range_db` below the peak (and
    never more again). Its decay is in dB, smoothed over `smooth` samples: it falls evenly."""
    peak = float(envelope.max()) if len(envelope) else 0.0
    if peak <= 0.0:
        return np.zeros(len(envelope))
    level = 20.0 * np.log10(np.maximum(envelope / peak, 1e-9))
    top = int(np.argmax(envelope))
    level[top:] = _smoothed(level[top:], smooth)
    amount = np.clip((level + range_db) / range_db, 0.0, 1.0)
    amount[:top] = 1.0
    amount[top:] = np.minimum.accumulate(amount[top:])
    return amount


# --- Points ------------------------------------------------------------------------------------

BENDS = np.linspace(-1.0, 1.0, 81)


def shape(x: np.ndarray, bend: float | np.ndarray) -> np.ndarray:
    """model.automation.shape, for arrays: how far a bent segment has come at x (0..1)."""
    a = -np.asarray(bend, np.float64) * CURVATURE
    a = np.where(np.abs(a) < 1e-4, 1e-4, a)
    return np.expm1(a * x) / np.expm1(a)


def curve_at(points, x: np.ndarray) -> np.ndarray:
    """A curve of (x, y, curve) points (in order of x) at x: as the device plays it."""
    x = np.asarray(x, np.float64)
    if not points:
        return np.ones_like(x)
    result = np.full_like(x, points[-1][1])
    result[x <= points[0][0]] = points[0][1]
    for (x0, y0, c0), (x1, y1, _c1) in pairwise(points):
        inside = (x >= x0) & (x < x1) if x1 > x0 else np.zeros_like(x, bool)
        if np.any(inside):
            bend = c0 if y1 >= y0 else -c0
            result[inside] = y0 + (y1 - y0) * shape((x[inside] - x0) / (x1 - x0), bend)
    return result


def _best_bend(x: np.ndarray, y: np.ndarray, i: int, j: int) -> tuple[float, np.ndarray]:
    """The segment from point i to point j: the curve (as a breakpoint's) fitting y best, and its values."""
    x0, y0, x1, y1 = x[i], y[i], x[j], y[j]
    u = (x[i:j + 1] - x0) / max(x1 - x0, 1e-12)
    if abs(y1 - y0) < 1e-6:
        return 0.0, np.full(len(u), y0)
    candidates = y0 + (y1 - y0) * shape(u[None, :], BENDS[:, None])
    best = int(np.argmin(np.sum((candidates - y[i:j + 1]) ** 2, axis=1)))
    bend = float(BENDS[best])
    return (bend if y1 >= y0 else -bend), candidates[best]


def fit_points(x: np.ndarray, y: np.ndarray, max_points: int = MAX_POINTS,
               tolerance: float = TOLERANCE) -> tuple[tuple[float, float, float], ...]:
    """Breakpoints (x, y, curve) for the curve y(x), x from 0 to 1: the ends, then one at a time where
    the fitted curve is furthest off, until it is within `tolerance` or there are `max_points`."""
    chosen = [0, len(x) - 1]
    while True:
        bends, worst, worst_at = [], 0.0, -1
        for i, j in pairwise(chosen):
            bend, values = _best_bend(x, y, i, j)
            bends.append(bend)
            errors = np.abs(values - y[i:j + 1])
            k = int(np.argmax(errors))
            if errors[k] > worst and i < i + k < j:
                worst, worst_at = float(errors[k]), i + k
        if worst <= tolerance or worst_at < 0 or len(chosen) >= max_points:
            break
        chosen = sorted([*chosen, worst_at])
    return tuple((round(float(x[i]), 5), round(float(y[i]), 5), round(bend, 4))
                 for i, bend in zip(chosen, [*bends, 0.0], strict=True))


# --- Analysis ----------------------------------------------------------------------------------


def analyze(kicks: list[np.ndarray], bass: np.ndarray, sample_rate: float, character: int = 1,
            pre: int = 0) -> Fit | None:
    """The fit for these hits (each from `pre` samples before its hit) over this input. None without
    a kick to speak of."""
    kicks = [np.asarray(k, np.float64) for k in kicks if len(k) > pre + 16]
    if not kicks or max(float(np.abs(k).max()) for k in kicks) < 1e-4:
        return None
    found = spectra(np.concatenate([k[pre:pre + int(0.3 * sample_rate)] for k in kicks]), bass, sample_rate)
    # Each hit's envelope against its own peak, averaged over the hits (cut to the shortest).
    envelopes = [clash_envelope(k, sample_rate, found, pre) for k in kicks]
    shortest = min(len(e) for e in envelopes)
    envelope = np.mean([e[:shortest] / max(float(e[:shortest].max()), 1e-12) for e in envelopes], axis=0)
    amount = reduction(envelope, RANGE_DB[int(np.clip(character, 0, len(RANGE_DB) - 1))],
                       int(DECAY_SMOOTH_SECONDS * sample_rate))
    times = np.arange(shortest) * 1000.0 / sample_rate

    # The length: until the reduction is over (a little after), within the device's range.
    top = int(np.argmax(amount))
    over = np.flatnonzero(amount[top:] <= 0.005)
    end = times[top + int(over[0])] if len(over) else times[-1]
    length = float(np.clip(round(end * 1.08), LENGTH_MIN, LENGTH_MAX))
    x = np.linspace(0.0, 1.0, 400)
    target = 1.0 - np.interp(x * length, times, amount, right=0.0)
    points = fit_points(x, target)
    return Fit(found, times, envelope, 1.0 - amount, length, points)


# --- Capturing hits ----------------------------------------------------------------------------


class _Ring:
    """A stream's latest values, by absolute index."""

    def __init__(self, size: int):
        self.data = np.zeros(size, np.float32)
        self.start = self.end = 0  # what it holds: [start, end)

    def feed(self, start: int, values: np.ndarray) -> None:
        size = len(self.data)
        values = np.asarray(values, np.float32)
        if start != self.end:  # a gap (or the first values): start again from these
            self.start = self.end = start
        if len(values) > size:  # (only the latest fit)
            self.end += len(values) - size
            values = values[-size:]
        self.data[(np.arange(len(values)) + self.end) % size] = values
        self.end += len(values)
        self.start = max(self.start, self.end - size)

    def get(self, start: int, end: int) -> np.ndarray:
        start, end = max(start, self.start), min(end, self.end)
        if end <= start:
            return np.zeros(0, np.float32)
        return self.data[np.arange(start, end) % len(self.data)]


class Capture:
    """The device's displays as they come ("key", "input", "phase", in step), and its hits: each
    hit's kick, once KICK_SECONDS of it (or all of it until the next hit) has come."""

    def __init__(self, sample_rate: float = 48000.0, keep_seconds: float = 6.0):
        self.sample_rate = sample_rate
        size = int(keep_seconds * sample_rate)
        self.streams = {name: _Ring(size) for name in ("key", "input", "phase")}
        self.pending: list[int] = []  # hits whose kick hasn't all come yet
        self.last_phase = -1.0  # the latest phase (-1: no curve playing, or nothing came)
        self.hit_count = 0
        self.key_peak = 0.0  # the key's loudest since the last call to take_key_peak()

    def feed(self, name: str, start: int, values: np.ndarray) -> None:
        ring = self.streams[name]
        ring.feed(start, values)
        if name == "phase":
            values = np.asarray(values)
            hits = [int(start + i) for i in np.flatnonzero(values == 0.0)]
            self.pending.extend(hits)
            self.hit_count += len(hits)
            self.last_phase = float(values[-1]) if len(values) else -1.0
        elif name == "key" and len(values):
            self.key_peak = max(self.key_peak, float(np.abs(values).max()))

    def take_key_peak(self) -> float:
        peak, self.key_peak = self.key_peak, 0.0
        return peak

    def hits(self) -> list[tuple[np.ndarray, np.ndarray]]:
        """The hits whose kick has all come since the last call: (kick from PRE_SECONDS before it,
        the input from BASS_SECONDS before it to the kick's end)."""
        key, inputs = self.streams["key"], self.streams["input"]
        ready = min(key.end, inputs.end)
        pre = int(PRE_SECONDS * self.sample_rate)
        done = []
        while self.pending:
            hit = self.pending[0]
            following = self.pending[1] if len(self.pending) > 1 else None
            end = hit + int(KICK_SECONDS * self.sample_rate)
            if following is not None:
                end = min(end, following)
            if ready < end:
                break
            self.pending.pop(0)
            if hit - pre >= key.start:
                done.append((key.get(hit - pre, end), inputs.get(hit - int(BASS_SECONDS * self.sample_rate), end)))
        return done
