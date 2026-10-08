# HUMANBRO C++ runtime

This is a dependency-free C++17 port of the inference path: MIDI or a DAW clip goes in, features are
computed, the tree ensemble runs, and humanized velocities come out. Its output is identical to the
Python pipeline: the 127 features match bit for bit, and the written velocities are identical.
`tests/test_cpp_parity.py` checks this.

There is no XGBoost, Python or other runtime dependency. The model is a ~16 MB `.hbm` file evaluated
by a small built-in tree walker.

## 1. Export a model (Python side)

```bash
python export_cpp_model.py --model models/velocity_xgb_quantized/model.json   # -> model.hbm next to it
```

The export checks itself against XGBoost on 20,000 rows and refuses to write a file that doesn't
reproduce it. The `.hbm` file holds the trees, the feature order, and the feature-pipeline settings
the model was trained with: windows, chord tolerance, quantization, and so on.

## 2. Build

```bash
# Windows (Visual Studio 2022)
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64
cmake --build cpp/build --config Release          # -> cpp/build/Release/humanbro.exe

# Linux / macOS / Ninja
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build                           # -> cpp/build/humanbro
```

This produces `humanbro` (the static library) and the `humanbro` CLI. Keep the strict floating-point
flags set in `CMakeLists.txt` (`/fp:precise`, `-ffp-contract=off`, no `-ffast-math`). Fast-math would
change feature values in the last bits and break exact parity.

## 3. Command line

```bash
humanbro --model models/velocity_xgb_quantized/model.hbm --input clip.mid --output clip_humanized.mid
humanbro ... --smoothing --dynamics-scale 1.3 --offset -4 --mix 0.8
```

The options mirror `humanize_midi.py`. `--dump-features` and `--dump-raw` write CSVs for debugging and
parity checks.

## 4. Library API

```cpp
#include <humanbro/humanbro.hpp>

humanbro::Humanizer humanizer("model.hbm");      // load once (~70 ms), reuse; const methods are thread-safe

humanbro::Score score;
score.ticks_per_quarter = 960;
score.tempo_changes   = {{0, 500000.0}};          // microseconds per quarter note (120 BPM)
score.time_signatures = {{0, 3, 4}};
score.notes.push_back({/*onset*/ 0, /*offset*/ 900, /*pitch*/ 60, /*velocity*/ 64});
// ... every non-drum note of the clip

humanbro::Options opt;
opt.dynamics_scale = 1.3;                         // the model compresses dynamics a little; widen them
std::vector<int> velocities = humanizer.humanize(score, opt);   // velocities[i] belongs to score.notes[i]
```

Lower-level calls: `features(score)` returns a `FeatureTable` in the model's column order, and
`predict_raw(table)` returns raw model output. Use them if you want your own post-processing.

### Using it in a plugin or DAW (JUCE, VST3, CLAP, ...)

* **It works on whole clips, not as a real-time stream.** The features use future context (next
  note, time until the next rest, phrase length, piece-level statistics). Run it as an offline "humanize
  selection/clip" action, not inside `processBlock`. It takes about 40 ms for 200 notes and about
  170 ms for 11,000 notes on one desktop CPU. Prediction uses all cores for large clips.
* **Run it off the audio thread.** It allocates memory, and a large clip can take a noticeable fraction
  of a second.
* **Fill `Score` from the host.** Use clip notes in ticks with key-down note-offs (don't extend them by
  sustain pedal), the host tempo map, and time signatures. Leave out drum tracks. The grid comes from
  this tempo map, which is right for DAW material and is exactly what the quantized model expects.
* Untested JUCE sketch:

```cpp
humanbro::Score toScore(const juce::MidiMessageSequence& seq, int ppq, double bpm, int num, int den) {
    humanbro::Score s;
    s.ticks_per_quarter = ppq;
    s.tempo_changes = {{0, 60'000'000.0 / bpm}};
    s.time_signatures = {{0, num, den}};
    for (int i = 0; i < seq.getNumEvents(); ++i) {
        auto* e = seq.getEventPointer(i);
        if (!e->message.isNoteOn() || e->message.getChannel() == 10) continue;
        const auto off = e->noteOffObject ? e->noteOffObject->message.getTimeStamp() : e->message.getTimeStamp();
        s.notes.push_back({(int64_t) e->message.getTimeStamp(), (int64_t) off,
                           e->message.getNoteNumber(), e->message.getVelocity()});
    }
    return s;   // call seq.updateMatchedPairs() first; timestamps in ticks
}
```

## 5. What matches Python, and what doesn't

| Python | C++ |
|---|---|
| `midi_io.parse_midi` (mido semantics: running status, re-strikes, unreleased notes, drums skipped) | `MidiFile::load` |
| tempo map, `grid_from_midi`, `BeatGrid.extended`, `quantize_notes` | `pipeline.cpp` |
| `extract_features` (bidirectional, all 127 features) | `extract_features` |
| residual baseline, smoothing, `shape_dynamics`, `mix`, clipping | `Humanizer::humanize` |
| XGBoost `inplace_predict` | `TreeModel::predict` |

Not ported:

* **The onset-based beat tracker** (`beat_source=tracked`). C++ always uses the file's or host's tempo
  map. That matches Python for grid-aligned or DAW input, which is the quantized model's purpose. For
  free-time recordings without a meaningful tempo map, such as MAESTRO-style performances, Python's
  default `auto` mode would track beats instead. Use Python for those files, or port `beat_tracking.py`.
* **Causal-context models** and **performance-conditioned models**. `.hbm` loading rejects both.

There is one numeric subtlety. NumPy 2 sums floats in a SIMD order that can't be reproduced portably.
The only feature that sums non-integers is `piece_pitch_std`, so C++ computes it with an exact integer
formula. Both versions round to the same float32 value, and the parity test confirms it on every
file tried.

## 6. Verify

```bash
python tests/test_cpp_parity.py --maestro_files 6
```

On the current models this compares 9 files (3 synthetic scores with tempo and time-signature changes,
plus 6 MAESTRO test files) under both models. Every feature value is identical, raw predictions agree to
float32 precision, there are 0 differing velocities with and without smoothing, and all non-velocity
events are preserved.
