# Models

The intelligence module's trained models, kept in the repository (each is tens of megabytes at most, under GitHub's
file limits; no Git LFS). The build copies them next to the executables, into `bin/models`, where the application
looks for them (intelligence/CMakeLists.txt). See [docs/intelligence.md](../../docs/intelligence.md).

| File | What it is | Where it came from |
|---|---|---|
| velocity.hbm (15.7 MB) | Humanize › Velocity: HUMANBRO's *quantized* velocity model, an XGBoost regressor of 1 986 trees over 127 features, trained on MAESTRO v3 with every note snapped to the beat grid | HUMANBRO `models/velocity_xgb_quantized/model.hbm` (trained 2026-10-07; exported by `export_cpp_model.py`, which checks the file against XGBoost before writing it), copied 2026-10-08. SHA-256 `287ebb87ab94673fc528b223213f1505d5396c334fb2390f3d0ad0844a232e9a` |

Its test-split figures (MAESTRO's 177 test performances): MAE 11.3, Pearson r 0.62, within a performance r 0.59, on
quantized notes.

To replace it: train and export in HUMANBRO (`python export_cpp_model.py --model <model.json>` writes `model.hbm` next to
it), copy it here under the same name, and run `intelligence_tests` and `test_humanizer`. The runtime refuses models
that don't predict velocities outright (HUMANBRO's residual ones) and those its version can't read.

Timing will have a model of its own here (timing.hbm).
