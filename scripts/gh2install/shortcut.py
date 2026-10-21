"""Application-menu entry and optional desktop icon for the installed game (freedesktop .desktop files)."""
import os
import shutil
import subprocess

NAME = "gh2-recomp"
DESKTOP_FILE = "reGH2.desktop"
LEGACY_DESKTOP_FILES = ("Guitar Hero II.desktop", "Recomp Hero 2.desktop")  # names used before the project was named


def _data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def _env():
    from .pipeline import clean_env
    return clean_env()


def desktop_dir():
    try:
        out = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True, env=_env()).stdout.strip()
        if out and os.path.isdir(out):
            return out
    except OSError:
        pass
    d = os.path.expanduser("~/Desktop")
    return d if os.path.isdir(d) else None


def menu_entry_path():
    return os.path.join(_data_home(), "applications", NAME + ".desktop")


def _quote(path):
    return '"' + path.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$").replace("`", "\\`") + '"'


def _entry(appimage, icon):
    return "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Version=1.0",
        "Name=reGH2",
        "GenericName=Guitar Hero II native PC port",
        "Comment=Unofficial native Linux port of Guitar Hero II (Vulkan), built from your own copy",
        "Keywords=guitar;hero;rhythm;music;game;",
        "Exec=%s launch" % _quote(appimage),
        "Icon=%s" % icon,
        "Terminal=false",
        "Categories=Game;",
        "StartupNotify=false",
        "Actions=Launcher;",
        "",
        "[Desktop Action Launcher]",
        "Name=Open the installer / launcher",
        "Exec=%s" % _quote(appimage),
        "",
    ])


def create(appimage, icon_source=None, desktop_icon=True, log=print):
    """Writes the menu entry (and the desktop icon). `appimage` is the path the shortcut starts."""
    icon = NAME
    if icon_source and os.path.exists(icon_source):
        dest = os.path.join(_data_home(), "icons", "hicolor", "256x256", "apps", NAME + ".png")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(icon_source, dest)
        icon = dest  # absolute path: works even before the icon cache is refreshed
    text = _entry(appimage, icon)
    path = menu_entry_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)
    made = [path]
    desk = desktop_dir() if desktop_icon else None
    for legacy in LEGACY_DESKTOP_FILES:  # drop the icon made by earlier versions under the old name
        old = os.path.join(desktop_dir() or "", legacy)
        if desktop_dir() and os.path.exists(old):
            os.remove(old)
            desk = desk or desktop_dir()
    if desk:
        dpath = os.path.join(desk, DESKTOP_FILE)
        with open(dpath, "w") as f:
            f.write(text)
        os.chmod(dpath, 0o755)
        subprocess.run(["gio", "set", dpath, "metadata::trusted", "true"], capture_output=True, env=_env())  # GNOME asks otherwise
        made.append(dpath)
    for cmd in (["update-desktop-database", os.path.dirname(path)],):
        try:
            subprocess.run(cmd, capture_output=True, env=_env())
        except OSError:
            pass
    log("Shortcut created: " + ", ".join(made))
    return made


def remove(log=print):
    desk = desktop_dir()
    for p in (menu_entry_path(), os.path.join(desk, DESKTOP_FILE) if desk else None,
              os.path.join(_data_home(), "icons", "hicolor", "256x256", "apps", NAME + ".png")):
        if p and os.path.exists(p):
            os.remove(p)
            log("Removed " + p)


def refresh(appimage, icon_source=None):
    """Keeps the shortcuts pointing at the AppImage's current location (it may have been moved)."""
    path = menu_entry_path()
    if not os.path.exists(path) or not appimage:
        return
    if _quote(appimage) + " launch" in open(path).read():
        return
    desk = desktop_dir()
    create(appimage, icon_source, desktop_icon=bool(desk and os.path.exists(os.path.join(desk, DESKTOP_FILE))),
           log=lambda *_: None)
