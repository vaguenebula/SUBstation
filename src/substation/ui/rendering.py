"""Renders in the background (exporting, freezing, reversing long clips): while
the engine renders on a thread of its own (RenderJob; or a ReverseJob), a modal
dialog shows how far it got and cancels it. The window goes on meanwhile (it repaints, its meters move), but
takes no edits: the render is of the project as it was when it started.

    with RenderProgress(window, "Export Audio") as progress:
        if progress.wait_for_devices(bridge):
            job = engine.start_export(...)
            progress.follow(job, "Exporting song.wav")
            frames = job.finish()  # None: cancelled
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Protocol, Self

from PySide6.QtCore import QEventLoop, Qt, QTimer
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QLabel,
    QProgressBar,
    QVBoxLayout,
    QWidget,
)

POLL_MS = 30  # how often the dialog looks at the render
STEPS = 1000  # the progress bar's resolution

_active: list[RenderProgress] = []  # the dialog showing, if any


class Job(Protocol):
    """What a dialog follows: the engine's RenderJob, or a ReverseJob (engine_bridge/reversing.py)."""

    @property
    def progress(self) -> float: ...  # 0..1

    @property
    def done(self) -> bool: ...

    def cancel(self) -> None: ...


def active() -> RenderProgress | None:
    """The render dialog showing now (a render is running), if any."""
    return _active[-1] if _active else None


class RenderProgress(QDialog):
    """A render's progress, with Cancel (Esc, or closing it, too). Used as a
    context manager: it shows on entering and goes on leaving."""

    def __init__(self, parent: QWidget | None, title: str):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setWindowModality(Qt.WindowModality.ApplicationModal)
        self.setWindowFlag(Qt.WindowType.WindowContextHelpButtonHint, False)
        self.setMinimumWidth(380)
        self.cancelled = False
        layout = QVBoxLayout(self)
        self.label = QLabel()
        self.bar = QProgressBar()
        self.bar.setRange(0, STEPS)
        self.bar.setTextVisible(False)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        buttons.rejected.connect(self.reject)
        self.cancel_button = buttons.button(QDialogButtonBox.StandardButton.Cancel)
        self.cancel_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # (Space, meant for the transport, doesn't cancel)
        layout.addWidget(self.label)
        layout.addWidget(self.bar)
        layout.addWidget(buttons)

    def __enter__(self) -> Self:
        _active.append(self)
        self.show()
        return self

    def __exit__(self, *_exc) -> None:
        _active.remove(self)
        self.hide()
        if self.parentWidget() is not None:  # its window has the keys again (its shortcuts work)
            self.parentWidget().window().activateWindow()
        self.deleteLater()

    def reject(self) -> None:
        """Cancel (the button, Esc, the close button): the render stops, and the dialog
        goes when its caller is done with it."""
        self.cancelled = True
        self.label.setText("Cancelling…")
        self.cancel_button.setEnabled(False)

    def wait_until(self, ready: Callable[[], bool], label: Callable[[], str]) -> bool:
        """Until ready() (a busy bar meanwhile, `label()` its text): False if cancelled first."""
        self.bar.setRange(0, 0)

        def step() -> bool:
            if not self.cancelled:
                self.label.setText(label())
            return self.cancelled or ready()

        self._spin(step)
        self.bar.setRange(0, STEPS)
        return not self.cancelled

    def wait_for_devices(self, bridge) -> bool:
        """Until the devices are ready to render (a project's plug-ins loading,
        samples): False if cancelled first."""

        def label() -> str:
            waiting = bridge.plugins_pending
            if waiting:
                return f"Loading plug-ins ({waiting} to go)…"
            return "Loading devices…"

        return self.wait_until(bridge.devices_ready, label)

    def follow(self, job: Job, label: str, part: tuple[int, int] = (0, 1)) -> None:
        """Shows a render's progress (as part `i` of `n` of the bar) until it ends
        (cancelled, if the user cancels). Its caller then finishes it."""
        index, count = part
        if not self.cancelled:
            self.label.setText(label)

        def step() -> bool:
            if self.cancelled:
                job.cancel()
            self.bar.setValue(round((index + job.progress) / max(1, count) * STEPS))
            return job.done

        self._spin(step)

    def _spin(self, step: Callable[[], bool]) -> None:
        """The event loop runs until step() (called every POLL_MS) is true."""
        if step():
            return
        loop = QEventLoop(self)
        timer = QTimer(self)
        timer.setInterval(POLL_MS)
        timer.timeout.connect(lambda: step() and loop.quit())
        timer.start()
        loop.exec()
        timer.stop()
        timer.deleteLater()
