# Analysis

[app/src/analysis](../../app/src/analysis) holds the signal maths that devices' editors run on what the engine's
displays stream: the spectra behind the Delay's and the EQ's curves, and the Sidechain device's fit to a kick. It is
plain C++ (the standard library, and Qt Core for a few types), part of `sub_app`, and not real-time: it runs on the GUI
thread, each time an editor reads its device's displays. The editors' scene-graph items in
[ui/src/devices](../../ui/src/devices) draw the results. A fit becomes ordinary device parameters again, through the
`ProjectEditor`, so it saves and undoes like any edit; the engine only ever plays parameters. It is kept out of
`model/`, which is the project and its edits.

## Overview

```
 engine: a built-in device publishes displays (one value per sample: "input", "key", "phase", ...)
    │  EngineBridge::readProcessorDisplay, when the bridge's meter poll emits metersUpdated
    ▼
 ui/src/devices: DeviceCanvas::readDisplay / readDisplayAt (the values since the last read, by absolute index)
    │
    ├─ FilterGraph (the Delay)    ─► analysis::FallingSpectrum::add ─► columns() ─► drawn behind the filter curve
    ├─ EqGraph (the EQ)           ─► analysis::EqAnalyzer::feed (input, output) ─► columns() ─► behind the bands
    └─ CurveGraph (the Sidechain) ─► sidechainFit::Capture::feed ─► hits() ─► sidechainFit::analyze ─► Fit
                                                                                      │ Fit, or Auto
                                                                                      ▼
                                ProjectEditor::setDeviceParams (the points, length, crossover): one undo step
```

## Files

| File | What it holds |
|---|---|
| [Fft.h](../../app/src/analysis/Fft.h) | `sub::app::analysis`: `fft` (radix-2, in place, forward or inverse, in `double`), `rfft` (the first n / 2 + 1 bins of a real signal cut or zero-padded to n), `hann`, `isPowerOfTwo` |
| [Spectrum.h](../../app/src/analysis/Spectrum.h) | `sub::app::analysis`: `FallingSpectrum` (the Delay's analyzer), `EqAnalyzer` (the EQ's) |
| [SidechainFit.h](../../app/src/analysis/SidechainFit.h) | `sub::app::sidechainFit`: `Ring` and `Capture` (the displays and the hits), `spectra` → `Spectra`, `clashEnvelope`, `reduction`, `shape`, `curveAt`, `fitPoints` → `FitPoint`s, `analyze` → `Fit`; the constants (`kCharacters`, `kRangeDb`, `kClashDb`, ...). Its header comment is the design. |

The transforms follow numpy's conventions (an unscaled forward transform, the inverse divided by n; a symmetric Hann
window, zero at both ends), so values computed with numpy from the same signals serve as the tests' expected values.

## Spectra

Both analyzers keep the latest samples of a display, take a Hann-windowed FFT of them each time more come, and smooth
the levels from one read to the next. Levels are in dB, with the window's gain taken out (`2 / sum(window)`).

- **`FallingSpectrum`** (the Delay, [FilterGraph.h](../../ui/src/devices/FilterGraph.h), display `"input"`): the
  latest 4096 samples (`kFftSize`). A bin rises at once to a louder level and falls back by `kFallDb` (1 dB) a read,
  never below `kFloorDb` (-90 dB); a read with no samples lets it fall. `add()` says whether anything changed, so the
  item repaints only then. `columns(n, low, high)` gives the spectrum under each of n columns on a log axis: the
  loudest bin under a column, or, low down where a bin spans several columns, the level interpolated between bins.
- **`EqAnalyzer`** (the EQ, [EqGraph.h](../../ui/src/devices/EqGraph.h), displays `"input"` and `"output"`): the
  latest 8192 samples of each. Each read, a level moves `kRise` (0.55) of the way up to a louder one, or `kFall` (0.09)
  of the way down to a quieter one, as Pro-Q's analyzer does. `live()` says whether a channel shows anything above the
  floor. `columns(channel, freqs)` gives the level at each of the graph's frequencies (the loudest bin between a
  column and the next, or interpolated between bins), smoothed across neighbours (¼, ½, ¼), and tilted by `kTilt`
  (4.5 dB an octave around 1 kHz) so that music looks about level. The tilt fades in over the `kTiltFade` (18 dB)
  above the floor (`kFloorDb`, -96 dB), so silence, or a spectrum falling back to it, stays flat instead of the tilt
  lifting its high end into view.

The EQ's band curves are not analysis: they come from the engine's own filter design (`eqResponseDb()` in
[audio/EqResponse.h](../../app/src/audio/EqResponse.h), see
[engine-bridge.md](engine-bridge.md#what-the-ui-may-include)), so the curve drawn is the one that plays.

## The Sidechain's fit

The Sidechain device ([engine/devices.md](../engine/devices.md)) plays a curve from each hit: every time its key (a
kick) hits, it ducks its input (a bass, say) by that curve. Its editor, `CurveGraph`
([CurveGraph.h](../../ui/src/devices/CurveGraph.h)), fits the curve to the kick: it finds where in frequency the kick
and the input clash, and how the kick's energy there rises and dies away. The curve ducks the input just as much and
as long as that.

### Capturing hits

The device publishes three displays, one value per sample, pushed together: `"key"` (the key's signal), `"input"` (the
input's, before it is ducked) and `"phase"` (the samples since the last hit, 0 at the hit; -1 while no curve plays).
They are a contract with the engine's Sidechain processor
([Sidechain.cpp](../../engine/src/builtin/devices/Sidechain.cpp)).

- A `Ring` keeps a stream's latest values by absolute sample index (`feed(start, values)`; a gap, or the first
  values, starts it again). `get(start, end)` returns what it still holds of that span.
- `Capture` lines the three streams up by their absolute index, in a `Ring` each (`keepSeconds`, 6 s, of them). Each
  0 in the phase is a hit, kept in `pending()` until enough of the key and the input has come. Then `hits()` hands out
  the hit's kick (from `kPreSeconds`, 5 ms, before the hit to `kKickSeconds`, 0.8 s, after it, or to the next hit if
  that comes first) and the input from `kBassSeconds` (2 s) before the hit to the same end. A hit whose start has
  already left the ring is dropped. `takeKeyPeak()` gives the key's loudest value since the last call (the editor's
  key meter), and `lastPhase()` where the curve is now (its playhead).

`CurveGraph` makes a new `Capture` when the device's sample rate changes. It keeps the latest `kKeepKicks` (3) kicks
and the latest hit's input, and analyses them again at every hit and when the *character* changes.

### Analysing

`analyze(kicks, bass, sampleRate, character, pre)` returns a `Fit`, or none without a kick to speak of (no kick longer
than `pre + 16` samples, or none louder than -80 dBFS):

1. **The clash** (`spectra`): the kicks' first 300 ms after the hit, joined, against the input. Each spectrum is the
   mean power of half-overlapping 8192-point Hann frames (one zero-padded frame if the signal is shorter), averaged
   over a sixth of an octave either side (`kSmoothOctaves`) at `kColumns` (160) frequencies from 20 Hz to 2 kHz, in
   dB against its own loudest (floor -80 dB). The clash is their geometric mean, so it is loud only where both are.
   Its band is the stretch around its peak where it stays within `kClashDb` (10 dB) of it. Without an input to speak
   of (an RMS at or below -80 dBFS: `bassHeard` false) the kick's own spectrum stands in.
2. **The envelope** (`clashEnvelope`): each kick weighted by the clash in the frequency domain, zero-phase (the
   clash's dB, at least -40, as an amplitude weight; -120 dB outside 20 Hz..2 kHz); the analytic signal's magnitude;
   smoothed over a cycle of the band's bottom (5 to 25 ms), so its partials beating leave no ripple. Each kick's
   envelope is taken against its own peak, then they are averaged (cut to the shortest).
3. **The reduction** (`reduction`): how much the input must give way, 0..1. All of it from the hit to the envelope's
   peak (a kick whose pitch falls into the band gets there late, but the input must be out of the way of its attack
   too); then following the envelope's decay in dB, smoothed over `kDecaySmoothSeconds` (40 ms) and never rising
   again; none once it is `kRangeDb` below the peak: 12, 20 or 30 dB for the characters Tight, Natural and Loose
   (`kCharacters`). Tight ducks only while the kick is loud, Loose for as long as it lingers.
4. **The length**: where the reduction has ended (at or below 0.005), plus 8 %, in ms, rounded, and kept within the
   device's `length` range (`kLengthMin`..`kLengthMax`, 10..2000 ms).
5. **The points** (`fitPoints`): the curve the reduction calls for (1 minus the reduction, 400 values over the
   length) is approximated by breakpoints: the two ends, then one at a time where the fitted curve is furthest off,
   until it is within `kTolerance` (0.02) everywhere or there are `kMaxPoints` (10). Each segment gets the bend (one
   of 81 from -1 to 1) that fits it best, bent as automation's segments are (`shape`, with
   [model/Automation.h](../../app/src/model/Automation.h)'s `kCurvature`), so the device plays the curve that was
   fitted (`curveAt`). Points are rounded: x and y to 5 places, the bend to 4.

A `Fit` holds the `Spectra` (the kick's, the input's and the clash, and the band: `low`, `high`, `peak`), the envelope
and the target curve over time (`times`, in ms from the hit), the `length` and the `points`. The editor draws the
envelope and the target behind the curve; `ClashView` ([ClashView.h](../../ui/src/devices/ClashView.h)) draws the
spectra and the band.

### Writing it

*Fit* writes the fit as the device's parameters in one undo step (`CurveGraph::fitValues`): every curve point's
parameters, `length`, `sync` off, and `crossover` at 1.5 times the top of the clash band (30..1000 Hz). *Auto*
(`autofit` on) fits again at every hit: turning it on is one undo step, and the fits after it merge into one more
while nothing else is done. Nothing is written while the user drags the curve.

## Gotchas

- The displays' absolute indexes are per processor: a device whose processor is made again (a preset, a rack change)
  starts again from 0. `DeviceCanvas::readDisplayAt` keeps its read position per processor, and `Ring::feed` starts
  again at a gap, so a new processor's samples never line up with the old one's.
- A hit is a 0 in the phase stream. Two hits closer than `kKickSeconds` cut the first kick short at the second.
- The maths is in `double`; the displays are `float`. The kicks are converted once, as `CurveGraph` keeps them.
- Nothing here is real-time safe (it allocates freely): never call it from the engine.

## Tests

- [test_sidechain_fit.cpp](../../tests/app/test_sidechain_fit.cpp): the FFT against its definition, the clash where
  both are loud, the kick alone without an input, the reduction holding to the peak and never rising again, `curveAt`
  bending as automation does, `fitPoints` finding a curve again, `analyze` per character, and `Capture` handing out
  each hit. Where it checks numbers, they are values numpy computed from the same signals.
- [test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp): the Delay's spectrum (a tone peaks at its
  frequency, then falls back 1 dB a read), the EQ's analyzer (silence stays flat, a tone peaks at its frequency), and
  the Sidechain's editor fitting a kick played through the engine: *Fit* in one undo step, Loose longer than Natural,
  *Auto* merging its fits.

See [testing.md](../testing.md).
