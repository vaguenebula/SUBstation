"""Running work from other threads on the main thread: the MCP server's thread,
analysis workers and LLM calls never touch the Project themselves. A call is
queued onto the Qt main thread and answered through a
concurrent.futures.Future, with a timeout; so calls run one at a time, between
UI events, exactly like the user's edits."""

from __future__ import annotations

from collections.abc import Callable
from concurrent.futures import CancelledError, Future
from concurrent.futures import TimeoutError as FutureTimeout
from typing import Any

from PySide6.QtCore import QObject, Qt, QThread, Signal

from ..ops.errors import BUSY, OpError

DEFAULT_TIMEOUT = 30.0  # seconds a caller waits for the main thread


class MainThreadDispatcher(QObject):
    _queued = Signal(object)

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._queued.connect(self._run, Qt.ConnectionType.QueuedConnection)

    def on_main_thread(self) -> bool:
        return QThread.currentThread() == self.thread()

    def submit(self, work: Callable[[], Any]) -> Future:
        """`work()` on the main thread (now, if this is it); its result in the Future."""
        future: Future = Future()
        if self.on_main_thread():
            self._run((work, future))
        else:
            self._queued.emit((work, future))
        return future

    def call(self, work: Callable[[], Any], timeout: float = DEFAULT_TIMEOUT) -> Any:
        """`work()` on the main thread, waiting up to `timeout` seconds for its
        result (or its exception). Not started in time: it never runs, and the
        error is 'busy'."""
        future = self.submit(work)
        try:
            return future.result(timeout)
        except FutureTimeout:
            if future.cancel():
                raise OpError(BUSY, f"The program didn't answer within {timeout:g} s") from None
            return future.result(timeout)  # it started just now: let it finish
        except CancelledError:
            raise OpError(BUSY, "The call was cancelled") from None

    def _run(self, job) -> None:
        work, future = job
        if not future.set_running_or_notify_cancel():
            return  # timed out before it began
        try:
            result = work()
        except BaseException as exc:  # noqa: BLE001 (the caller gets it)
            future.set_exception(exc)
        else:
            future.set_result(result)
