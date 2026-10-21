"""Install pipeline: user's game image -> codegen -> native build. Everything proprietary is generated on the user's
machine from the user's own files; nothing of the game ships with this project."""
import hashlib
import filecmp
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

from . import __version__, shortcut
from .xiso import Xiso, XisoError

# SHA-1 of the default.xex this project's hints were made for. Anything else may need different function hints.
KNOWN_XEX = {
    "b90b864487e6291761b207c8d0064e9a0779bdee": "Guitar Hero II (Xbox 360, title 415607E7)",
}


class InstallError(Exception):
    pass


def repo_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def default_prefix():
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "gh2-recomp")


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_xex(folder):
    for name in os.listdir(folder):
        if name.lower() == "default.xex":
            return os.path.join(folder, name)
    return None


def find_game(folder, max_depth=3):
    """Looks for the game inside a folder the user picked. Returns the extracted game folder (holding default.xex) or a
    disc image (.iso with default.xex at its root), or None. The folder itself wins, then disc images, then sub-folders."""
    folder = os.path.abspath(folder)
    if find_xex(folder):
        return folder
    isos, dirs = [], []
    for root, subdirs, files in os.walk(folder):
        depth = root[len(folder):].count(os.sep)
        if depth >= max_depth:
            subdirs[:] = []
        subdirs.sort()
        for f in sorted(files):
            if f.lower().endswith(".iso"):
                isos.append(os.path.join(root, f))
        if find_xex(root) and root != folder:
            dirs.append(root)
    for iso in isos:
        try:
            image = Xiso(iso)
        except (XisoError, OSError):
            continue
        try:
            if image.has_root_file("default.xex"):
                return iso
        finally:
            image.close()
    return dirs[0] if dirs else None


def mem_available_gb():
    try:
        for line in open("/proc/meminfo"):
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) / (1024 * 1024)
    except OSError:
        pass
    return 8.0


def job_count():
    # clang on the generated sources needs roughly 1.5 GB per job.
    return max(1, min(os.cpu_count() or 2, int(mem_available_gb() / 1.5)))


def replace_file(src, dst):
    """Copies src over dst without touching a file a running game may be using: nothing happens when the contents are
    already the same, otherwise the new file is written aside and renamed over the old one (the running process keeps
    the old file; overwriting it in place would crash it)."""
    if os.path.exists(dst) and os.path.getsize(src) == os.path.getsize(dst) and filecmp.cmp(src, dst, shallow=False):
        return
    tmp = dst + ".new"
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)


def clean_env():
    """Environment for programs outside the AppImage (file manager, package manager, the game): the bundled libraries,
    Python and toolchain must not leak into them, or a system program can fail to start (e.g. Dolphin with our older
    libstdc++)."""
    e = dict(os.environ)
    appdir = e.pop("APPDIR", None)
    e.pop("APPIMAGE", None)
    for k in ("PYTHONHOME", "PYTHONPATH", "TCL_LIBRARY", "TK_LIBRARY", "GH2_SDK", "GH2_CXX", "GH2_TOOLCHAIN_FILE",
              "ARGV0", "OWD"):
        e.pop(k, None)

    def outside(paths):
        return [p for p in paths.split(":") if p and not (appdir and p.startswith(appdir)) and "fallback-libs" not in p]

    e["PATH"] = ":".join(outside(e.get("PATH", "")))
    ld = ":".join(outside(e.get("LD_LIBRARY_PATH", "")))
    if ld:
        e["LD_LIBRARY_PATH"] = ld
    else:
        e.pop("LD_LIBRARY_PATH", None)
    return e


def host_libs():
    """Sonames the host's dynamic linker already knows (x86-64)."""
    for cmd in ("/sbin/ldconfig", "ldconfig"):
        try:
            out = subprocess.run([cmd, "-p"], capture_output=True, text=True).stdout
        except OSError:
            continue
        return {l.split()[0] for l in out.splitlines() if "x86-64" in l and l.strip()}
    return set()


class Installer:
    def __init__(self, prefix=None, sdk=None, cxx=None, toolchain_file=None, log=print, progress=None, force=False,
                 desktop_icon=True):
        self.prefix = os.path.abspath(prefix or default_prefix())
        self.sdk = os.path.abspath(sdk or os.environ.get("GH2_SDK") or os.path.join(repo_root(), "sdk"))
        self.cxx = cxx or os.environ.get("GH2_CXX") or "clang++"
        self.toolchain_file = toolchain_file or os.environ.get("GH2_TOOLCHAIN_FILE")
        self.log = log
        self.progress = progress or (lambda frac, text: None)
        self.force = force
        self.desktop_icon = desktop_icon
        self.app_dir = os.path.join(repo_root(), "app")

    # ---- helpers ------------------------------------------------------------------------------------------------
    def env(self):
        e = dict(os.environ)
        libdir = os.path.join(self.sdk, "linux-amd64", "lib")
        if not os.path.isdir(libdir):
            libdir = os.path.join(self.sdk, "lib")
        e["LD_LIBRARY_PATH"] = libdir + (":" + e["LD_LIBRARY_PATH"] if e.get("LD_LIBRARY_PATH") else "")
        return e

    def ensure_runtime(self):
        """Copies what the built game loads at run time next to it (librexruntime.so, the C++ runtime and the fallback
        system libraries shipped in the AppImage), so the game keeps running after the AppImage (and its mount) is gone."""
        app = os.path.join(self.prefix, "app")
        lib = os.path.join(app, "lib")
        os.makedirs(lib, exist_ok=True)
        rt = os.path.join(self.sdk_prefix(), "lib", "librexruntime.so")
        if os.path.exists(rt):
            replace_file(rt, os.path.join(lib, "librexruntime.so"))
        appdir = os.environ.get("APPDIR")
        if appdir:
            for f in glob.glob(os.path.join(appdir, "usr", "lib", "*.so*")):
                replace_file(f, os.path.join(lib, os.path.basename(f)))
            fb = os.path.join(app, "fallback")
            os.makedirs(fb, exist_ok=True)
            for f in glob.glob(os.path.join(appdir, "usr", "fallback", "*")):
                replace_file(f, os.path.join(fb, os.path.basename(f)))
        if not os.path.exists(os.path.join(lib, "librexruntime.so")):
            raise InstallError("librexruntime.so not found (SDK at %s)" % self.sdk)

    def make_shortcut(self):
        """Menu entry + desktop icon; only when running from an AppImage (the shortcut starts that file)."""
        appimage = os.environ.get("APPIMAGE")
        if not appimage:
            return
        appdir = os.environ.get("APPDIR", "")
        try:
            shortcut.create(appimage, os.path.join(appdir, "gh2-recomp.png"), self.desktop_icon, self.log)
        except OSError as e:
            self.log("Could not create the shortcut: %s" % e)

    # ---- uninstall ----------------------------------------------------------------------------------------------
    def user_data_dir(self):
        """Saves, installed DLC, achievements and the shader cache. Shared by every build of the game, kept by default."""
        return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "gh2")

    def export_saves(self, dest_dir):
        """Zips the save game (the player's profile) and the achievements into dest_dir. The shader cache and the installed
        DLC (a big folder that can be installed again) are left out. Returns the path of the zip."""
        import zipfile
        root = self.user_data_dir()
        skip = ("cache", "0000000000000000")
        files = []
        if os.path.isdir(root):
            for entry in sorted(os.listdir(root)):
                if entry in skip:
                    continue
                for dp, _, names in os.walk(os.path.join(root, entry)):
                    files += [os.path.join(dp, n) for n in sorted(names)]
        if not files:
            raise InstallError("no save game found in %s (play and save in the game first)" % root)
        os.makedirs(dest_dir, exist_ok=True)
        path = os.path.join(dest_dir, time.strftime("reGH2-saves-%Y%m%d-%H%M%S.zip"))
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            for f in files:
                z.write(f, os.path.join("gh2", os.path.relpath(f, root)))
        return path

    def running_game_pids(self):
        app = os.path.join(self.prefix, "app", "gh2")
        pids = []
        for d in os.listdir("/proc"):
            if d.isdigit():
                try:
                    exe = os.readlink("/proc/%s/exe" % d)
                except OSError:
                    continue
                if exe in (app, app + " (deleted)"):
                    pids.append(int(d))
        return pids

    def uninstall_plan(self, delete_saves=False):
        """What uninstall would remove: [(path, size in bytes)], plus notes about what stays."""
        def size(path):
            total = 0
            for dp, _, files in os.walk(path):
                for f in files:
                    try:
                        total += os.lstat(os.path.join(dp, f)).st_size
                    except OSError:
                        pass
            return total
        items, keep = [], []
        if os.path.isdir(self.prefix):
            if not (os.path.exists(os.path.join(self.prefix, "install.json")) or os.path.isdir(os.path.join(self.prefix, "app"))):
                raise InstallError("%s does not look like an installation of this program; nothing was removed" % self.prefix)
            items.append((self.prefix, size(self.prefix)))
        cache = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "gh2-recomp")
        if os.path.isdir(cache):
            items.append((cache, size(cache)))
        meta = self.status() or {}
        game_dir = meta.get("game_dir")
        if game_dir and os.path.isdir(game_dir) and not os.path.abspath(game_dir).startswith(self.prefix + os.sep):
            keep.append("your own game folder %s is never touched" % game_dir)
        data = self.user_data_dir()
        if os.path.isdir(data):
            if delete_saves:
                items.append((data, size(data)))
            else:
                keep.append("your saves, DLC and achievements in %s are kept" % data)
        return items, keep

    def uninstall(self, delete_saves=False, dry_run=False):
        """Removes the installed game, its shortcuts and cache. Saves and DLC stay unless `delete_saves`."""
        pids = self.running_game_pids()
        if pids:
            raise InstallError("the game is running (process %s): close it first" % ", ".join(map(str, pids)))
        items, keep = self.uninstall_plan(delete_saves)
        for path, nbytes in items:
            self.log("%s %s (%.1f MB)" % ("would remove" if dry_run else "removing", path, nbytes / 1e6))
        for note in keep:
            self.log("kept: " + note)
        if dry_run:
            return items, keep
        for path, _ in items:
            shutil.rmtree(path, ignore_errors=True)
        shortcut.remove(log=self.log)
        return items, keep

    def write_report(self):
        """Problem report from outside the game (for when it will not start or has crashed): install record, system, the
        game's settings and the tails of its logs. The home folder is replaced by "~" so it can go into a public issue."""
        import platform
        logs = os.path.join(self.prefix, "app", "logs")
        os.makedirs(logs, exist_ok=True)

        def tail(path, n):
            try:
                with open(path, errors="replace") as f:
                    return "".join(f.readlines()[-n:])
            except OSError:
                return "(not found)\n"

        def first(path, key):
            try:
                for line in open(path):
                    if line.startswith(key):
                        return line.split(":", 1)[1].strip()
            except OSError:
                pass
            return "?"

        files = sorted(glob.glob(os.path.join(logs, "gh2_*.log")), key=os.path.getmtime, reverse=True)
        meta = self.status()
        out = ["reGH2 problem report (made by the launcher)", "Created: " + time.strftime("%Y-%m-%d %H:%M:%S"),
               "Attach this file to a GitHub issue and say what you were doing when the problem happened.", "",
               "===== Install =====", json.dumps(meta, indent=1) if meta else "not installed", "",
               "===== System =====", "OS: " + platform.freedesktop_os_release().get("PRETTY_NAME", "?") if hasattr(platform, "freedesktop_os_release") else "OS: ?",
               "Kernel: " + platform.release() + " " + platform.machine(),
               "Session: %s, desktop %s" % (os.environ.get("XDG_SESSION_TYPE", "-"), os.environ.get("XDG_CURRENT_DESKTOP", "-")),
               "CPU: %s (%s threads)" % (first("/proc/cpuinfo", "model name"), os.cpu_count()),
               "RAM: total %s, available %s" % (first("/proc/meminfo", "MemTotal"), first("/proc/meminfo", "MemAvailable")), "",
               "===== Game settings (app/gh2.toml) =====", tail(os.path.join(self.prefix, "app", "gh2.toml"), 60), "",
               "===== Slowdown log (last 40 lines) =====", tail(os.path.join(logs, "perf.log"), 40)]
        for label, f, n in (("Latest log", files[0] if files else None, 400), ("Previous log", files[1] if len(files) > 1 else None, 150)):
            out += ["===== %s (%s, last %d lines) =====" % (label, os.path.basename(f) if f else "none", n), tail(f, n) if f else "no log file", ""]
        text = "\n".join(out)
        home = os.path.expanduser("~")
        if home and home != "/":
            text = text.replace(home, "~")
        path = os.path.join(logs, time.strftime("report-%Y%m%d-%H%M%S.txt"))
        with open(path, "w") as f:
            f.write(text)
        return path

    def game_env(self):
        """Environment for the game process: its own libraries only, nothing inherited from the AppImage."""
        app = os.path.join(self.prefix, "app")
        e = clean_env()
        libs = [os.path.join(app, "lib")]
        fb = os.path.join(app, "fallback")
        if os.path.isdir(fb):
            have = host_libs()
            run_libs = os.path.join(self.prefix, "run-libs")
            os.makedirs(run_libs, exist_ok=True)
            wanted = {name for name in os.listdir(fb) if name not in have}
            for name in os.listdir(run_libs):  # only touch what changed: another instance may be using these links
                if name not in wanted:
                    os.unlink(os.path.join(run_libs, name))
            for name in wanted:
                link, target = os.path.join(run_libs, name), os.path.join(fb, name)
                if not (os.path.islink(link) and os.readlink(link) == target):
                    tmp = link + ".new"
                    if os.path.lexists(tmp):
                        os.unlink(tmp)
                    os.symlink(target, tmp)
                    os.replace(tmp, link)
            libs.append(run_libs)
        e["LD_LIBRARY_PATH"] = ":".join(libs + ([e["LD_LIBRARY_PATH"]] if e.get("LD_LIBRARY_PATH") else []))
        e["REX_GAME_DATA_ROOT"] = (self.status() or {}).get("game_dir", "")
        e["REX_GPU_PLUGIN"] = "xenos"
        return e

    def sdk_bin(self, name):
        for cand in (os.path.join(self.sdk, "linux-amd64", "bin", name), os.path.join(self.sdk, "bin", name)):
            if os.path.exists(cand):
                return cand
        raise InstallError("SDK not found at %s (no bin/%s)" % (self.sdk, name))

    def sdk_prefix(self):
        p = os.path.join(self.sdk, "linux-amd64")
        return p if os.path.isdir(p) else self.sdk

    def run(self, cmd, cwd=None, on_line=None):
        self.log("$ " + " ".join(cmd))
        p = subprocess.Popen(cmd, cwd=cwd, env=self.env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             errors="replace")
        tail = []
        for line in p.stdout:
            line = line.rstrip()
            tail.append(line)
            tail = tail[-40:]
            if on_line:
                on_line(line)
        rc = p.wait()
        if rc != 0:
            raise InstallError("command failed (%d): %s\n%s" % (rc, " ".join(cmd), "\n".join(tail[-15:])))
        return tail

    # ---- steps --------------------------------------------------------------------------------------------------
    def locate_game(self, source):
        """Returns the folder holding default.xex, extracting a disc image if needed."""
        source = os.path.abspath(source)
        if os.path.isdir(source):
            found = find_game(source)
            if not found:
                raise InstallError("no Guitar Hero II found in %s (looked for default.xex and for .iso disc images)" % source)
            if os.path.isdir(found):
                return found
            self.log("Found disc image %s" % found)
            source = found
        if source.lower().endswith(".xex"):
            return os.path.dirname(source)
        game_dir = os.path.join(self.prefix, "game")
        self.log("Reading disc image %s" % source)
        try:
            iso = Xiso(source)
        except XisoError as e:
            raise InstallError("%s: %s" % (source, e))
        os.makedirs(game_dir, exist_ok=True)

        def prog(done, total, name):
            self.progress(0.02 + 0.28 * done / max(1, total), "Extracting %s" % name)

        iso.extract(game_dir, prog)
        iso.close()
        if not find_xex(game_dir):
            raise InstallError("the image has no default.xex")
        return game_dir

    def fingerprint(self):
        """Identifies what gets compiled (our sources, hints, CMake and the SDK patches): a different one means the
        installed game is out of date and a rebuild is worthwhile."""
        h = hashlib.sha1()
        for root in (self.app_dir, os.path.join(repo_root(), "sdk-patches")):
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames.sort()
                for name in sorted(filenames):
                    path = os.path.join(dirpath, name)
                    h.update(os.path.relpath(path, root).encode())
                    with open(path, "rb") as f:
                        h.update(f.read())
        return h.hexdigest()

    def source_xex_sha1(self, source):
        """SHA-1 of the default.xex a source (folder, disc image or .xex) holds, without extracting a whole disc."""
        source = os.path.abspath(source)
        if os.path.isdir(source):
            source = find_game(source)
            if source is None:
                return None
            if os.path.isdir(source):
                return sha1_of(find_xex(source))
        if source.lower().endswith(".xex"):
            return sha1_of(source)
        try:
            iso = Xiso(source)
        except (XisoError, OSError):
            return None
        try:
            data = iso.read_file("default.xex")
        finally:
            iso.close()
        return hashlib.sha1(data).hexdigest() if data else None

    def install_state(self, source):
        """'none' (nothing installed), 'other' (installed from a different default.xex), 'outdated' (our code changed
        since) or 'current' (the same game and the same code: nothing to rebuild)."""
        meta = self.status()
        if not meta or not os.path.exists(os.path.join(self.prefix, "app", "gh2")):
            return "none"
        if self.source_xex_sha1(source) != meta.get("xex_sha1"):
            return "other"
        return "current" if meta.get("fingerprint") == self.fingerprint() else "outdated"

    def check_xex(self, xex):
        self.progress(0.31, "Checking the game executable")
        digest = sha1_of(xex)
        self.log("default.xex SHA-1 %s" % digest)
        if digest not in KNOWN_XEX:
            msg = ("This default.xex (%s) is not the version this project was made for (%s). "
                   "The recompiler may need other function hints." % (digest, ", ".join(KNOWN_XEX)))
            if not self.force:
                raise InstallError(msg + " Use --force to try anyway.")
            self.log("WARNING: " + msg)
        return digest

    def prepare_project(self, build_dir, game_dir, xex):
        self.progress(0.33, "Preparing the project")
        if os.path.isdir(build_dir):
            shutil.rmtree(build_dir)
        os.makedirs(build_dir)
        self.run([self.sdk_bin("rexglue"), "init", "--project-name", "gh2", "--xex-path", xex, "--project-root", build_dir])
        for item in ("src", "hints"):
            dst = os.path.join(build_dir, item)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            shutil.copytree(os.path.join(self.app_dir, item), dst)
        shutil.copy(os.path.join(self.app_dir, "CMakeLists.txt"), os.path.join(build_dir, "CMakeLists.txt"))
        text = open(os.path.join(self.app_dir, "gh2_manifest.toml.in")).read()
        text = text.replace("@GAME_ROOT@", game_dir).replace("@XEX_PATH@", xex)
        open(os.path.join(build_dir, "gh2_manifest.toml"), "w").write(text)

    def codegen(self, build_dir):
        self.progress(0.36, "Recompiling the game (PowerPC to C++)")
        tail = self.run([self.sdk_bin("rexglue"), "codegen", "gh2_manifest.toml"], cwd=build_dir)
        if not any("Codegen summary" in l for l in tail):
            raise InstallError("codegen did not finish: " + "\n".join(tail[-8:]))
        # The recompiler turns the PowerPC SMT yield hint (db16cyc) into a comment; the spin-waits of the game then burn a
        # core. Replace it by a call into the host's wait helper (src/gh2_host.cpp).
        gen = os.path.join(build_dir, "generated", "default")
        patched = 0
        pat = re.compile(r"^([ \t]*)// db16cyc.*$", re.M)
        for name in os.listdir(gen):
            if name.startswith("gh2_recomp.") and name.endswith(".cpp"):
                p = os.path.join(gen, name)
                s = open(p, errors="surrogateescape").read()
                n = pat.sub(r"\1{ void gh2_spin_hint(); gh2_spin_hint(); }  // db16cyc", s)
                if n != s:
                    open(p, "w", errors="surrogateescape").write(n)
                    patched += 1
        self.log("spin hints patched in %d files" % patched)

    def build(self, build_dir):
        out = os.path.join(build_dir, "out")
        cfg = ["cmake", "-S", build_dir, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
               "-DCMAKE_CXX_COMPILER=" + self.cxx, "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
               "-DCMAKE_PREFIX_PATH=" + self.sdk_prefix()]
        if self.toolchain_file:
            cfg.append("-DCMAKE_TOOLCHAIN_FILE=" + self.toolchain_file)
        self.progress(0.42, "Configuring the build")
        self.run(cfg)
        jobs = job_count()
        self.log("building with %d parallel jobs" % jobs)
        step = re.compile(r"^\[(\d+)/(\d+)\]")

        def on_line(line):
            m = step.match(line)
            if m:
                a, b = int(m.group(1)), int(m.group(2))
                self.progress(0.45 + 0.5 * a / max(1, b), "Compiling %d/%d" % (a, b))

        self.run(["cmake", "--build", out, "-j", str(jobs)], on_line=on_line)
        return out

    def install(self, source, rebuild=False):
        t0 = time.time()
        os.makedirs(self.prefix, exist_ok=True)
        if not rebuild and self.install_state(source) == "current":
            self.log("Already installed and up to date: nothing to rebuild (use --rebuild to force it).")
            meta = self.status()
            gd = meta.get("game_dir", "")
            if not (os.path.isdir(gd) and find_xex(gd)):
                self.progress(0.1, "Restoring the game files")  # the folder it was installed from is gone: no rebuild needed
                meta["game_dir"] = self.locate_game(source)
                json.dump(meta, open(os.path.join(self.prefix, "install.json"), "w"), indent=1)
            self.ensure_runtime()
            self.make_shortcut()
            self.progress(1.0, "Already installed")
            return meta
        self.progress(0.01, "Looking for the game")
        game_dir = self.locate_game(source)
        xex = find_xex(game_dir)
        digest = self.check_xex(xex)
        build_dir = os.path.join(self.prefix, "build")
        self.prepare_project(build_dir, game_dir, xex)
        self.codegen(build_dir)
        out = self.build(build_dir)
        app = os.path.join(self.prefix, "app")
        os.makedirs(app, exist_ok=True)
        for f in ("gh2", "librexgpu-xenos.so"):
            replace_file(os.path.join(out, f), os.path.join(app, f))
        self.ensure_runtime()
        meta = {"version": __version__, "game_dir": game_dir, "xex_sha1": digest, "sdk": self.sdk,
                "fingerprint": self.fingerprint(), "installed": time.strftime("%Y-%m-%d %H:%M:%S"), "seconds": round(time.time() - t0)}
        json.dump(meta, open(os.path.join(self.prefix, "install.json"), "w"), indent=1)
        self.make_shortcut()
        self.progress(1.0, "Done")
        self.log("Installed in %d s. Launch with: gh2-recomp launch" % meta["seconds"])
        return meta

    # ---- run ----------------------------------------------------------------------------------------------------
    def status(self):
        p = os.path.join(self.prefix, "install.json")
        return json.load(open(p)) if os.path.exists(p) else None

    def launch(self, extra_args=()):
        meta = self.status()
        if not meta:
            raise InstallError("the game is not installed yet")
        app = os.path.join(self.prefix, "app")
        if not os.path.exists(os.path.join(app, "lib", "librexruntime.so")):
            self.ensure_runtime()  # installed by an older version
        env = self.game_env()
        os.execve(os.path.join(app, "gh2"), [os.path.join(app, "gh2"), *extra_args], env)
