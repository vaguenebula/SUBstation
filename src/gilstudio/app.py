"""Application entry point: `python -m gilstudio [project.gilproj]`."""

from __future__ import annotations

import sys

from PySide6.QtCore import QCoreApplication, QTimer
from PySide6.QtWidgets import QApplication, QMessageBox

from . import APP_NAME, engine_mismatch, theme
from . import _engine as ge


def _set_windows_app_id() -> None:
    # Groups the taskbar button under our icon instead of python.exe's.
    if sys.platform == "win32":
        import ctypes

        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("GILStudio.DAW")


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv if argv is None else argv
    _set_windows_app_id()
    QCoreApplication.setOrganizationName(APP_NAME)
    QCoreApplication.setApplicationName(APP_NAME)
    app = QApplication(argv)
    theme.apply(app)
    if (mismatch := engine_mismatch()) is not None:
        print(mismatch, file=sys.stderr)
        QMessageBox.critical(None, APP_NAME, mismatch)
        return 1

    from .ui.main_window import MainWindow  # after QApplication exists

    engine = ge.Engine()
    window = MainWindow(engine)
    window.show()
    QTimer.singleShot(0, window.start_audio)
    if len(argv) > 1:
        QTimer.singleShot(0, lambda: window.open_project(argv[1]))
    code = app.exec()
    engine.close_device()
    window.bridge.shutdown()
    return code
