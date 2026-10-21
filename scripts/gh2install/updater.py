"""Looks on GitHub for a newer release of reGH2 and, only when the user asks, replaces the AppImage with it.

Nothing is sent but one HTTPS GET to the GitHub API. The download is checked against the SHA256SUMS.txt of the same release.
GH2_NO_UPDATE_CHECK=1 turns the check off; GH2_UPDATE_API points it at another server (tests).
"""
import hashlib
import json
import os
import re
import shutil
import urllib.request

from . import __version__

REPO = "jillsandwich1998/reGH2"
APPIMAGE_NAME = "reGH2-x86_64.AppImage"
SUMS_NAME = "SHA256SUMS.txt"


class UpdateError(Exception):
    pass


def api_url():
    return os.environ.get("GH2_UPDATE_API") or "https://api.github.com/repos/%s/releases?per_page=30" % REPO


def parse_version(text):
    m = re.match(r"v?(\d+)\.(\d+)\.(\d+)", text or "")
    return tuple(int(x) for x in m.groups()) if m else None


def _get(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "reGH2/" + __version__, "Accept": "application/vnd.github+json"})
    return urllib.request.urlopen(req, timeout=timeout)


def _trusted(url):
    return url.startswith("https://") or bool(os.environ.get("GH2_UPDATE_API"))


def latest(timeout=6):
    """The newest published release (pre-releases included) that carries the AppImage, as a dict, or None. Raises UpdateError
    when GitHub cannot be reached."""
    try:
        with _get(api_url(), timeout) as r:
            releases = json.load(r)
    except Exception as e:  # noqa: BLE001 - no network, rate limit, bad JSON: all mean "cannot tell"
        raise UpdateError("cannot reach GitHub: %s" % e)
    best = None
    for rel in releases if isinstance(releases, list) else []:
        ver = parse_version(rel.get("tag_name"))
        if rel.get("draft") or not ver:
            continue
        assets = {a.get("name"): a.get("browser_download_url") for a in rel.get("assets", [])}
        if APPIMAGE_NAME not in assets or SUMS_NAME not in assets:
            continue
        if not best or ver > best["tuple"]:
            best = {"tuple": ver, "version": ".".join(map(str, ver)), "tag": rel["tag_name"], "notes": rel.get("body") or "",
                    "page": rel.get("html_url", ""), "appimage": assets[APPIMAGE_NAME], "sums": assets[SUMS_NAME]}
    return best


def is_newer(info, current=None):
    return bool(info) and info["tuple"] > (parse_version(current or __version__) or (0, 0, 0))


def check_enabled(config_path=None):
    if os.environ.get("GH2_NO_UPDATE_CHECK"):
        return False
    try:
        return bool(json.load(open(config_path or config_file())).get("check_updates", True))
    except (OSError, ValueError):
        return True


def config_file():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "gh2-recomp", "launcher.json")


def set_check_enabled(on, config_path=None):
    path = config_path or config_file()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump({"check_updates": bool(on)}, open(path, "w"))


def target_appimage():
    """The AppImage that is running (replaced by an update), or None when not run from one."""
    p = os.environ.get("APPIMAGE")
    return p if p and os.path.isfile(p) else None


def download(info, dest_dir, progress=None, timeout=30):
    """Downloads the release's AppImage into dest_dir as a hidden temporary file, checks its sha256 against the release's
    SHA256SUMS.txt and returns the path. progress(fraction) is optional."""
    for u in (info["appimage"], info["sums"]):
        if not _trusted(u):
            raise UpdateError("refusing a download address that is not https: " + u)
    try:
        with _get(info["sums"], timeout) as r:
            sums = r.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        raise UpdateError("cannot download the checksum file: %s" % e)
    want = None
    for line in sums.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].lstrip("*") == APPIMAGE_NAME:
            want = parts[0].lower()
    if not want:
        raise UpdateError("the release has no checksum for " + APPIMAGE_NAME)
    tmp = os.path.join(dest_dir, ".%s.part" % APPIMAGE_NAME)
    h = hashlib.sha256()
    try:
        with _get(info["appimage"], timeout) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            while True:
                block = r.read(1 << 20)
                if not block:
                    break
                f.write(block)
                h.update(block)
                done += len(block)
                if progress and total:
                    progress(done / total)
    except Exception as e:  # noqa: BLE001
        _rm(tmp)
        raise UpdateError("download failed: %s" % e)
    if h.hexdigest() != want:
        _rm(tmp)
        raise UpdateError("the downloaded file does not match its checksum; nothing was changed")
    os.chmod(tmp, 0o755)
    return tmp


def _rm(path):
    try:
        os.remove(path)
    except OSError:
        pass


def apply(info, progress=None, log=print):
    """Downloads the new AppImage and puts it where the running one is (an atomic rename, so a running game or launcher is not
    disturbed). If that folder is not writable, or this is not an AppImage, the file goes to ~/Downloads instead.
    Returns (path, replaced)."""
    target = target_appimage()
    if target and os.access(os.path.dirname(target), os.W_OK):
        tmp = download(info, os.path.dirname(target), progress)
        os.replace(tmp, target)
        log("Updated %s to %s" % (target, info["version"]))
        return target, True
    folder = os.path.join(os.path.expanduser("~"), "Downloads")
    os.makedirs(folder, exist_ok=True)
    tmp = download(info, folder, progress)
    final = os.path.join(folder, "reGH2-%s-x86_64.AppImage" % info["version"])
    shutil.move(tmp, final)
    log("Saved the new version as %s (the running AppImage could not be replaced)" % final)
    return final, False
