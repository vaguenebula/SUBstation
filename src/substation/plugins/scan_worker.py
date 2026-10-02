"""The child process that reads plug-in files (see scanner.py).

Says {"ready": true} once it can scan, then reads one JSON-encoded path per line
from stdin and answers each with one JSON line on stdout: {"path": ...,
"plugins": [...]} or {"path": ..., "error": ...}. If a plug-in crashes or hangs
the process, the parent notices the missing answer and carries on without it.
"""

from __future__ import annotations

import json
import os
import sys


def _quiet() -> object:
    """Keep stdout for our answers: whatever plug-ins print goes nowhere. Returns
    the stream to answer on."""
    answers = os.fdopen(os.dup(sys.stdout.fileno()), "w", encoding="utf-8", buffering=1)
    devnull = os.open(os.devnull, os.O_WRONLY)
    os.dup2(devnull, 1)
    os.dup2(devnull, 2)
    if sys.platform == "win32":
        import ctypes

        # No "program stopped working" or "insert a disk" dialogs if a plug-in misbehaves.
        sem_failcriticalerrors, sem_nogpfaulterrorbox, sem_noopenfileerrorbox = 0x0001, 0x0002, 0x8000
        ctypes.windll.kernel32.SetErrorMode(sem_failcriticalerrors | sem_nogpfaulterrorbox | sem_noopenfileerrorbox)
    return answers


def main() -> int:
    answers = _quiet()
    from substation import _engine as ge

    answers.write(json.dumps({"ready": True}) + "\n")
    answers.flush()
    for line in sys.stdin:
        if not line.strip():
            continue
        path = json.loads(line)
        try:
            plugins = [{"uid": d.uid, "name": d.name, "vendor": d.vendor, "version": d.version,
                        "category": d.category, "instrument": d.is_instrument} for d in ge.scan_vst3(path)]
            answer = {"path": path, "plugins": plugins}
        except Exception as exc:  # noqa: BLE001 - reported to the parent
            answer = {"path": path, "error": str(exc) or type(exc).__name__}
        answers.write(json.dumps(answer) + "\n")
        answers.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
