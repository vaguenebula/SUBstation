"""The intelligence layer: a headless description of the song, operations that
read and edit it (for suggestions, the assistant and outside agents alike),
and the rails they run on (undo labels, revisions, the activity log).

See INTELLIGENCE.md for the design and docs/python/intel.md for what is built.

- ops/: the operations registry (typed, schema'd, permissioned, undoable) and
  the runner that applies them through the ProjectEditor.
- context/: SongContext (an immutable, JSON-able description of the song), the
  role classifier, and the revision counter.
- facts.py: the narrow protocols the layer reads the engine (EngineFacts) and
  the UI (UiFacts) through.
- activity.py: the log of what actors other than the user did.
- qt/: the only Qt-facing piece: IntelController, MainThreadDispatcher and the
  bridge's EngineFacts adapter.

Rules (tests/test_intel_imports.py): nothing here imports substation.ui or
QtWidgets; QtCore only in qt/; substation._engine only in qt/engine_facts.py.
Writes go only through ops/ and the ProjectEditor."""
