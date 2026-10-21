"""Folder picker that uses the system's own file dialog (KDE: kdialog, GNOME and others: zenity), Tk's as a last resort."""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from gh2install.pipeline import clean_env  # noqa: E402


def _commands(title, start):
    start = start if os.path.isdir(start) else os.path.expanduser("~")
    kde = "kde" in os.environ.get("XDG_CURRENT_DESKTOP", "").lower()
    cmds = {
        "kdialog": ["kdialog", "--title", title, "--getexistingdirectory", start],
        "zenity": ["zenity", "--file-selection", "--directory", "--title", title, "--filename", start.rstrip("/") + "/"],
        "yad": ["yad", "--file", "--directory", "--title", title, "--filename", start.rstrip("/") + "/"],
    }
    order = ["kdialog", "zenity", "yad"] if kde else ["zenity", "kdialog", "yad"]
    return [cmds[n] for n in order if shutil.which(n)]


def pick_directory(title, start=""):
    """Returns the chosen folder, "" when the user cancelled, or None when no native dialog is available."""
    for cmd in _commands(title, start or os.path.expanduser("~")):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, env=clean_env())
        except OSError:
            continue
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip().splitlines()[0]
        if r.returncode == 1:  # cancelled
            return ""
    return None
