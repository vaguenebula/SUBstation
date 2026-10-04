# Analysis

`src/substation/analysis/` holds signal maths that devices' editors run on what the
engine's displays stream: numpy only, no Qt, and not real-time (it runs on the UI
thread as an editor reads its displays). Its results become ordinary device
parameters again, through `ProjectEditor`, so they save and undo like any edit; the
engine only ever plays parameters. It is kept out of `model/`, which is the project
and its edits.

| File | Contents |
|---|---|
| [sidechain_fit.py](../../src/substation/analysis/sidechain_fit.py) | Fitting the Sidechain device's curve to a kick: `Capture` (its displays, by absolute index, and the hits), `spectra` (where the kick and the input clash), `clash_envelope`, `reduction`, `fit_points`, `analyze` → `Fit` |

## The Sidechain's fit

The Sidechain device (see [engine/devices.md](../engine/devices.md)) plays a curve from
each hit; its editor ([device-view.md](../ui/device-view.md)) fits that curve to the kick:

1. **The clash**: the kick's spectrum against the input's (each against its own
   loudest, averaged over a sixth of an octave either side); their geometric mean is
   loud only where both are. The clash band is where it is within 10 dB of its peak.
   Without an input to speak of, the kick's own spectrum stands in.
2. **The envelope**: the kick weighted by the clash (zero-phase, in the frequency
   domain), the analytic signal's magnitude, smoothed over a cycle of the band's bottom.
3. **The reduction**: full from the hit to the envelope's peak (a kick whose pitch falls
   into the band gets there late), then following its decay in dB (smoothed over 40 ms,
   never rising again), none once it is 12, 20 or 30 dB below the peak (Tight, Natural,
   Loose). The curve's length is where that ends, plus 8 %.
4. **The points**: the ends, then a point where the fitted curve is furthest off, until
   it is within 0.02 everywhere or there are 10; each segment gets the bend (as
   automation bends) that fits it best.

The displays it reads (`key`, `input`, `phase`, one value per sample, pushed together)
are a contract with the engine: `Capture` lines them up by their absolute index, and the
phase's 0s are the hits.

Tests: [test_sidechain_fit.py](../../tests/test_sidechain_fit.py).
