"""The few things the launcher does differently on Windows and on Linux (the rest of it is the same Tk program)."""
import os
import subprocess
import sys

IS_WIN = sys.platform == "win32"
UI_FONT = "Segoe UI" if IS_WIN else "Sans"
MONO_FONT = "Consolas" if IS_WIN else "Monospace"
HERE = os.path.dirname(os.path.abspath(__file__))


def enable_dpi_awareness():
    """Windows: draw sharp text on scaled displays (Tk would otherwise be stretched by the system)."""
    if not IS_WIN:
        return
    import ctypes
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def icon_path():
    p = os.path.join(HERE, "reGH2.ico")
    return p if IS_WIN and os.path.exists(p) else None


def open_path(path):
    """Opens a folder or an address with the system's default program. Returns False when this system has no such thing (Linux callers try their own openers)."""
    if not IS_WIN:
        return False
    try:
        os.startfile(path)   # noqa: S606 - a folder of ours or the project's page
        return True
    except OSError:
        return False


def detached_flags():
    """Popen keyword arguments that let a started program outlive the launcher."""
    if IS_WIN:
        return {"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP, "close_fds": True}
    return {"start_new_session": True}
