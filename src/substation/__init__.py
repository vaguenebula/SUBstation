"""SUBstation: a basic DAW with a Python/Qt UI and a C++ audio engine."""

__version__ = "0.1.0"

APP_NAME = "SUBstation"

# The engine's bindings this code needs (engine/src/bindings.cpp: API_VERSION).
# 2: ProcessorEventType.PARAM_TOUCHED. 3: the master (track id MASTER) has devices.
# 4: input, monitoring and recording. 5: MIDI input. 6: chain ids. 7: track outputs (group buses).
# 8: audio threads. 9: track costs, cost ordering. 10: sends (routing edges). 11: track inputs (resampling).
# 12: sidechains. 16: eq_response (the EQ's curves). 17: freezing (set_track_frozen, render_track_to_wav).
# 18: renders in the background (start_export, start_track_render: RenderJob).
ENGINE_API = 18


def engine_mismatch() -> str | None:
    """Why the compiled engine doesn't match this code, or None if it does."""
    from . import _engine

    built = getattr(_engine, "API_VERSION", 1)
    if built == ENGINE_API:
        return None
    return (f"The audio engine was built from {'older' if built < ENGINE_API else 'newer'} code than the app "
            f"(engine API {built}, app needs {ENGINE_API}).\n\n"
            "Rebuild it from the project folder:\n    python -m pip install --no-build-isolation -e .")
