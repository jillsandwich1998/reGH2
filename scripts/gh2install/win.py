"""Windows installer pipeline: the user's own copy of Guitar Hero II -> codegen -> native build -> a runnable gh2.exe.

The package (or a development checkout) is laid out like the repository: <root>\\scripts, \\app, \\native, \\sdk-patches, plus the folders the installer ships with
(\\host, \\llvm-mingw, \\ninja, \\sdk\\win-amd64, \\python, \\shader-tools). What is not in the package is looked for on the machine, so a development checkout works too.

Two ways to build:
* module (the player's): the package carries the host prebuilt (\\host: gh2.exe, the runtime and renderer, the DLLs it loads; made on our machine by `make-host`).
  Only the recompiled game code is compiled here, as the game module gh2game.dll, with llvm-mingw (clang, lld, libc++, mingw-w64: all redistributable), so nothing
  from Microsoft is needed (native/tools/build_game_module.py, native/rt/module).
* full (a development checkout without a prebuilt host): CMake + clang + Visual Studio's C++ tools and Windows SDK build everything, the host included.

It subclasses gh2install.pipeline.Installer, which does the parts that are the same on every system (checking default.xex, `rexglue init` / `codegen`, the spin-hint
patch, the install record); this module replaces what is different on Windows: where the tools are, the build environment, the files that make an installed game, how
the game is started and how it is removed.
"""
import ctypes
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

from . import __version__
from . import pipeline as P
from .pipeline import InstallError

ROOT = P.repo_root()   # the package root has the layout of the repository


# ---- places -----------------------------------------------------------------------------------------------------------------
def local_appdata():
    return os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")


def default_prefix():
    """Where the built game lives: the build project (generated C++, objects) and app\\ (gh2.exe and what it loads)."""
    return os.path.join(local_appdata(), "reGH2", "game")


def user_data_dir():
    """Saves, installed DLC, achievements and the shader cache: the folder the game itself uses (native/platform/os: DataDir)."""
    return os.path.join(local_appdata(), "gh2")


def bundled(*parts):
    """A folder or file the package ships (None in a development checkout that has not got it)."""
    p = os.path.join(ROOT, *parts)
    return p if os.path.exists(p) else None


def _program_files(*names):
    for var in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"):
        base = os.environ.get(var)
        for n in names:
            if base and os.path.isdir(os.path.join(base, n)):
                yield os.path.join(base, n)


# ---- the tools ----------------------------------------------------------------------------------------------------------------
def llvm_bin():
    for cand in (bundled("llvm", "bin"), *[os.path.join(p, "bin") for p in _program_files("LLVM")]):
        if cand and os.path.exists(os.path.join(cand, "clang.exe")):
            return cand
    which = shutil.which("clang")
    return os.path.dirname(which) if which else None


def llvm_mingw_dir():
    """llvm-mingw (clang, lld, libc++, mingw-w64), which builds the game module gh2game.dll: the package's, else GH2_LLVM_MINGW."""
    for cand in (bundled("llvm-mingw"), os.environ.get("GH2_LLVM_MINGW")):
        if cand and os.path.exists(os.path.join(cand, "bin", "clang++.exe")):
            return cand
    return None


HOST_META = "host.json"          # what the prebuilt host was built from (version, contract, the xex, the hashes of the boundary lists)
HOST_OVERRIDES = "overrides.txt"  # the game functions its hooks replace: the game module is generated from this list
HOST_NOT_APP = (HOST_META, HOST_OVERRIDES)


def host_dir():
    """The prebuilt host: the package's host\\, else GH2_HOST_DIR (a folder made by `gh2win.py make-host`); None means a full build."""
    for cand in (bundled("host"), os.environ.get("GH2_HOST_DIR")):
        if cand and all(os.path.exists(os.path.join(cand, f)) for f in ("gh2.exe", HOST_META, HOST_OVERRIDES)):
            return cand
    return None


def read_host_meta(folder):
    try:
        with open(os.path.join(folder, HOST_META), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def module_tools():
    """native/tools as an importable package directory (gen_module_stubs: the list hashes)."""
    tools = os.path.join(ROOT, "native", "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import gen_module_stubs  # noqa: E402
    return gen_module_stubs


def contract_version():
    with open(os.path.join(ROOT, "native", "rt", "module", "include", "gh2module", "game_module.h"), encoding="utf-8") as f:
        m = re.search(r"#define GH2GAME_ABI_VERSION (\d+)u", f.read())
    return int(m.group(1)) if m else None


def cmake_bin():
    for cand in (bundled("cmake", "bin"), *[os.path.join(p, "bin") for p in _program_files("CMake")]):
        if cand and os.path.exists(os.path.join(cand, "cmake.exe")):
            return cand
    which = shutil.which("cmake")
    return os.path.dirname(which) if which else None


def ninja_dir(msvc=None):
    cand = bundled("ninja")
    if cand and os.path.exists(os.path.join(cand, "ninja.exe")):
        return cand
    which = shutil.which("ninja")
    if which:
        return os.path.dirname(which)
    if msvc:   # Visual Studio carries one next to its CMake
        hits = glob.glob(os.path.join(msvc["path"], "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe"))
        if hits:
            return os.path.dirname(hits[0])
    return None


def vulkan_dev():
    """(include dir, vulkan-1.lib) for compiling against Vulkan: the package's copy, else the LunarG SDK."""
    inc, lib = bundled("vulkan", "Include"), bundled("vulkan", "Lib", "vulkan-1.lib")
    if inc and lib:
        return inc, lib
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk and os.path.isdir(r"C:\VulkanSDK"):
        versions = sorted(os.listdir(r"C:\VulkanSDK"), reverse=True)
        sdk = os.path.join(r"C:\VulkanSDK", versions[0]) if versions else None
    if sdk and os.path.exists(os.path.join(sdk, "Lib", "vulkan-1.lib")):
        return os.path.join(sdk, "Include"), os.path.join(sdk, "Lib", "vulkan-1.lib")
    return None, None


def zlib_source():
    """The zlib sources the package carries (the build would otherwise download them), or None."""
    hits = sorted(glob.glob(os.path.join(ROOT, "deps", "zlib-*")))
    return hits[-1] if hits and os.path.exists(os.path.join(hits[-1], "CMakeLists.txt")) else None


def python_exe():
    return bundled("python", "python.exe") or sys.executable


def shader_tools_dir():
    """native/tools of the package's shader-tools bundle (XenosRecomp binary beside it), else of this checkout."""
    return bundled("shader-tools", "native", "tools") or os.path.join(ROOT, "native", "tools")


# ---- Visual Studio Build Tools (the C runtime and the Windows SDK) ------------------------------------------------------------------
def vswhere():
    p = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Microsoft Visual Studio", "Installer", "vswhere.exe")
    return p if os.path.exists(p) else None


def _vswhere_json(*extra):
    exe = vswhere()
    if not exe:
        return []
    try:
        out = subprocess.run([exe, "-products", "*", "-format", "json", "-utf8", *extra], capture_output=True, text=True, encoding="utf-8").stdout
        return json.loads(out or "[]")
    except (OSError, ValueError):
        return []


def find_msvc():
    """The newest Visual Studio with the x64 C++ tools: {"path", "vcvars", "redist"} (redist = folder with the C++ runtime DLLs, or None)
    and whether a Windows SDK exists next to it; None when there is no such installation."""
    for inst in _vswhere_json("-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"):
        path = inst.get("installationPath")
        vcvars = os.path.join(path or "", "VC", "Auxiliary", "Build", "vcvars64.bat")
        if not os.path.exists(vcvars):
            continue
        redist = sorted(glob.glob(os.path.join(path, "VC", "Redist", "MSVC", "*", "x64", "Microsoft.VC*.CRT")))
        sdk_root = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Include")
        has_sdk = bool(glob.glob(os.path.join(sdk_root, "10.*", "um", "windows.h")))
        return {"path": path, "vcvars": vcvars, "redist": redist[-1] if redist else None, "has_sdk": has_sdk}
    return None


def msvc_environment(msvc):
    """The environment of an x64 Visual Studio command prompt (INCLUDE, LIB, PATH of the C++ tools and the Windows SDK)."""
    env = dict(os.environ)
    installer = os.path.dirname(vswhere() or "")
    if installer:
        env["PATH"] = env.get("PATH", "") + os.pathsep + installer   # vcvars looks for vswhere on PATH
    out = subprocess.run('cmd /c ""%s" >nul 2>&1 && set"' % msvc["vcvars"], capture_output=True, text=True, env=env, shell=True,
                         encoding="utf-8", errors="replace").stdout
    for line in out.splitlines():
        if "=" in line and not line.startswith("="):
            k, v = line.split("=", 1)
            env[k] = v
    return env


def format_elapsed(seconds):
    seconds = int(max(0, seconds))
    return "%d:%02d" % (seconds // 60, seconds % 60)


# ---- memory / jobs ---------------------------------------------------------------------------------------------------------------
def mem_available_gb():
    class MemoryStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    status = MemoryStatus()
    status.dwLength = ctypes.sizeof(status)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return status.ullAvailPhys / (1 << 30)
    return 8.0


P.mem_available_gb = mem_available_gb   # job_count() asks the module-level name


def rmtree_force(path):
    """shutil.rmtree that also removes read-only files (git objects) and survives a file that is briefly locked."""
    def onerror(func, p, _exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except OSError:
            pass
    shutil.rmtree(path, onerror=onerror)


def _native_ignore(directory, names):
    """What the build project does not need from native/: caches, the golden traces, and the XenosRecomp clone and its build (the game runs the shader tools bundle)."""
    skip = {"__pycache__", "golden"}
    if os.path.basename(directory) == "third_party":
        skip |= {"XenosRecomp", "build"}
    return [n for n in names if n in skip]


# ---- the installer ---------------------------------------------------------------------------------------------------------------
class WinInstaller(P.Installer):
    EXPORT_SKIP = ("cache", "shaders", "shader_build", "0000000000000000")

    def __init__(self, prefix=None, sdk=None, full=False, **kw):
        kw.pop("cxx", None)
        kw.pop("toolchain_file", None)
        super().__init__(prefix=prefix or os.environ.get("GH2_PREFIX") or default_prefix(), sdk=sdk or os.environ.get("GH2_SDK") or "", **kw)
        self.host = None if full else host_dir()   # a prebuilt host: only the game module is compiled here (see the module docstring)
        self._env = None
        self._msvc = None
        self._last_frac = 0.0
        shown = self.progress

        def progress(frac, text):   # (remembers where the bar is, for the elapsed-time ticker of the quiet steps)
            self._last_frac = frac
            shown(frac, text)
        self.progress = progress

    # ---- tools and environment ----------------------------------------------------------------------------------
    def msvc(self):
        if self._msvc is None:
            self._msvc = find_msvc() or {}
        return self._msvc or None

    def sdk_prefix(self):
        p = os.path.join(self.sdk, "win-amd64")
        return p if os.path.isdir(p) else self.sdk

    def sdk_bin(self, name):
        cand = os.path.join(self.sdk_prefix(), "bin", name + ".exe")
        if os.path.exists(cand):
            return cand
        raise InstallError("the ReXGlue SDK was not found at %s (no bin\\%s.exe): build it with scripts\\windows\\build-sdk.ps1 or pass --sdk" % (self.sdk, name))

    def simde_dir(self):
        """The folder that holds simde/ (header-only SIMD emulation the generated code uses): the SDK's include folder."""
        inc = os.path.join(self.sdk_prefix(), "include")
        return inc if os.path.exists(os.path.join(inc, "simde", "x86", "sse4.1.h")) else None

    def check_toolchain(self):
        """Raises InstallError saying what is missing; returns the Visual Studio description (a full build) or {} (a module build)."""
        if self.host:
            self.sdk_bin("rexglue")
            for what, found in (("llvm-mingw (the compiler of the game module)", llvm_mingw_dir()), ("Ninja", ninja_dir()), ("simde (in the SDK's include folder)", self.simde_dir())):
                if not found:
                    raise InstallError("%s was not found: reinstall reGH2 (a development checkout needs GH2_LLVM_MINGW / the tools installed)." % what)
            meta = read_host_meta(self.host)
            if meta.get("contract") != contract_version():
                raise InstallError("the prebuilt game host in %s speaks contract %s, these sources %s: reinstall reGH2" % (self.host, meta.get("contract"), contract_version()))
            return {}
        msvc = self.msvc()
        if not msvc:
            raise InstallError("Visual Studio Build Tools (the C++ tools) are not installed. The launcher can install them for you; "
                               "by hand: winget install Microsoft.VisualStudio.2022.BuildTools, workload 'Desktop development with C++'.")
        if not msvc["has_sdk"]:
            raise InstallError("the Windows SDK is missing from the Visual Studio Build Tools installation (component 'Windows 11 SDK'). The launcher can add it.")
        for what, found in (("LLVM (clang)", llvm_bin()), ("CMake", cmake_bin()), ("Ninja", ninja_dir(msvc)), ("the Vulkan headers and vulkan-1.lib", vulkan_dev()[0])):
            if not found:
                raise InstallError("%s was not found: reinstall reGH2 (a development checkout needs it installed)." % what)
        return msvc

    def env(self):
        if self._env is None and self.host:
            # no Visual Studio environment: llvm-mingw brings its own headers and libraries. The host folder is on PATH for the DLLs rexglue.exe loads
            # (the SDK's runtime and the C++ runtime DLLs the package carries).
            self.check_toolchain()
            e = {k: v for k, v in os.environ.items() if k.upper() not in ("INCLUDE", "LIB", "LIBPATH", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH")}
            paths = [os.path.join(llvm_mingw_dir(), "bin"), ninja_dir(), os.path.join(self.sdk_prefix(), "bin"), self.host]
            e["PATH"] = os.pathsep.join([p for p in paths if p] + [e.get("PATH", "")])
            e["PYTHONUTF8"] = "1"
            self._env = e
        if self._env is None:
            msvc = self.check_toolchain()
            e = msvc_environment(msvc)
            llvm = llvm_bin()
            self.ensure_cxx(llvm)
            paths = [llvm, cmake_bin(), ninja_dir(msvc), os.path.join(self.sdk_prefix(), "bin")]
            e["PATH"] = os.pathsep.join([p for p in paths if p] + [e.get("PATH", "")])
            e["PYTHONUTF8"] = "1"
            self._env = e
        return dict(self._env)

    # LLVM tools that are one program under several names (clang++ is clang; the archive tools are llvm-ar; strip is objcopy): the package carries each program once
    LLVM_ALIASES = {"clang++.exe": "clang.exe", "llvm-lib.exe": "llvm-ar.exe", "llvm-ranlib.exe": "llvm-ar.exe", "llvm-dlltool.exe": "llvm-ar.exe",
                    "llvm-strip.exe": "llvm-objcopy.exe"}

    # steps that print little while they work: the progress line shows how long they have been running
    TICKED_STEPS = (("codegen", "Translating the game's code (PowerPC to C++)"), ("init", "Preparing the project"), ("-S", "Configuring the build"))

    def run(self, cmd, cwd=None, on_line=None):
        """Installer.run with the program looked up on the toolchain's PATH (Windows searches the parent's PATH for the executable, not the env= it is given),
        every line it prints sent to the log (the launcher shows them as they come) and, for the quiet steps, a ticking elapsed time on the progress line."""
        env = self.env()
        found = shutil.which(cmd[0], path=env.get("PATH"))
        label = next((text for arg, text in self.TICKED_STEPS if arg in cmd[1:3] or (arg == "-S" and "-S" in cmd)), None)
        state = {"frac": getattr(self, "_last_frac", 0.0), "done": False}
        if label:
            import threading
            t0 = time.time()

            def tick():
                while not state["done"]:
                    self.progress(state["frac"], "%s  -  %s" % (label, format_elapsed(time.time() - t0)))
                    time.sleep(1.0)
            threading.Thread(target=tick, daemon=True).start()

        def both(line):
            self.log(line)
            if on_line:
                on_line(line)
        try:
            return super().run([found or cmd[0], *cmd[1:]], cwd=cwd, on_line=both)
        finally:
            state["done"] = True

    @staticmethod
    def ensure_cxx(llvm):
        """Makes the other names of the LLVM programs the package carries once (as hard links, else copies). A development machine's LLVM already has them."""
        for alias, target in WinInstaller.LLVM_ALIASES.items():
            dst, src = os.path.join(llvm, alias), os.path.join(llvm, target)
            if not os.path.exists(dst) and os.path.exists(src):
                try:
                    os.link(src, dst)
                except OSError:
                    shutil.copy2(src, dst)

    def build_info(self):
        def first_line(cmd):
            try:
                env = self.env()
                exe = shutil.which(cmd[0], path=env.get("PATH")) or cmd[0]
                out = subprocess.run([exe, *cmd[1:]], capture_output=True, text=True, env=env).stdout.strip().splitlines()
                return out[0] if out else "?"
            except (OSError, InstallError):
                return "?"
        if self.host:
            host = read_host_meta(self.host)
            return {"mode": "module", "compiler": "llvm-mingw clang++", "compiler_version": first_line(["clang++", "--version"]),
                    "bundled_toolchain": bool(bundled("llvm-mingw")), "host": {k: host.get(k) for k in ("version", "built", "contract", "overrides")},
                    "options": "game module gh2game.dll: -O2 -DNDEBUG, llvm-mingw (clang + lld, libc++), prebuilt host", "parallel_jobs": getattr(self, "last_jobs", None)}
        msvc = self.msvc() or {}
        return {"mode": "full", "compiler": "clang++", "compiler_version": first_line(["clang++", "--version"]), "bundled_toolchain": bool(bundled("llvm")),
                "cmake": first_line(["cmake", "--version"]), "visual_studio": msvc.get("path", "?"),
                "options": "Release, -O2 -DNDEBUG, clang + lld-link, Vulkan", "parallel_jobs": getattr(self, "last_jobs", None)}

    # ---- the fingerprint of what gets compiled (without the clones and build trees of a development checkout) --------
    def fingerprint(self):
        import hashlib
        h = hashlib.sha1()
        for root in (self.app_dir, os.path.join(ROOT, "sdk-patches"), os.path.join(ROOT, "native")):
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = sorted(d for d in dirnames if d not in _native_ignore(dirpath, dirnames) and d != "__pycache__")
                for name in sorted(filenames):
                    if name.endswith(".pyc"):
                        continue
                    path = os.path.join(dirpath, name)
                    h.update(os.path.relpath(path, root).encode())
                    with open(path, "rb") as f:
                        h.update(f.read().replace(b"\r\n", b"\n"))   # (a CRLF checkout and an LF package are the same sources)
        if self.host:   # a new prebuilt host (another list of overrides, contract) needs the module rebuilt
            for name in HOST_NOT_APP:
                with open(os.path.join(self.host, name), "rb") as f:
                    h.update(f.read().replace(b"\r\n", b"\n"))
        return h.hexdigest()

    # ---- steps -----------------------------------------------------------------------------------------------------
    def copy_sources(self, build_dir):
        """src/, hints/, native/ and the CMake file into the build project (what the Linux installer's prepare_project does)."""
        for item in ("src", "hints"):
            dst = os.path.join(build_dir, item)
            if os.path.isdir(dst):
                rmtree_force(dst)
            shutil.copytree(os.path.join(self.app_dir, item), dst)
        dst = os.path.join(build_dir, "native")
        if os.path.isdir(dst):
            rmtree_force(dst)
        shutil.copytree(os.path.join(ROOT, "native"), dst, ignore=_native_ignore)
        shutil.copy(os.path.join(self.app_dir, "CMakeLists.txt"), os.path.join(build_dir, "CMakeLists.txt"))

    def prepare_project(self, build_dir, game_dir, xex):
        self.progress(0.33, "Preparing the project")
        if os.path.isdir(build_dir):
            rmtree_force(build_dir)
        os.makedirs(build_dir)
        # (forward slashes: the paths go into a TOML string, where a backslash would start an escape)
        game_dir, xex = game_dir.replace("\\", "/"), xex.replace("\\", "/")
        self.run([self.sdk_bin("rexglue"), "init", "--project-name", "gh2", "--xex-path", xex, "--project-root", build_dir])
        self.copy_sources(build_dir)
        with open(os.path.join(self.app_dir, "gh2_manifest.toml.in"), encoding="utf-8") as f:
            text = f.read().replace("@GAME_ROOT@", game_dir).replace("@XEX_PATH@", xex)
        with open(os.path.join(build_dir, "gh2_manifest.toml"), "w", encoding="utf-8") as f:
            f.write(text)

    def build(self, build_dir):
        out = os.path.join(build_dir, "out")
        inc, lib = vulkan_dev()
        fwd = lambda p: p.replace("\\", "/")  # noqa: E731
        cfg = ["cmake", "-S", build_dir, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
               "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
               "-DCMAKE_PREFIX_PATH=" + fwd(self.sdk_prefix()), "-DVulkan_INCLUDE_DIR=" + fwd(inc), "-DVulkan_LIBRARY=" + fwd(lib)]
        mingw = llvm_mingw_dir()
        if mingw:   # the game code becomes gh2game.dll (GH2_GAME_MODULE, the default on Windows)
            cfg += ["-DGH2_LLVM_MINGW=" + fwd(mingw)]
        zlib = zlib_source()
        if zlib:   # the package carries zlib: nothing is downloaded while installing
            cfg += ["-DFETCHCONTENT_SOURCE_DIR_GH2_ZLIB=" + fwd(zlib), "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
        self.progress(0.42, "Configuring the build")
        self.run(cfg)
        jobs = P.job_count()
        self.last_jobs = jobs
        self.log("building with %d parallel jobs" % jobs)
        keep_going = ["--", "-k", "0"] if os.environ.get("GH2_KEEP_GOING") else []   # ninja: go on after an error, list them all
        self.run_ninja_steps(["cmake", "--build", out, "-j", str(jobs)] + keep_going)
        return out

    def build_module(self, build_dir):
        """The module build: only the generated game code, as gh2game.dll, with llvm-mingw (native/tools/build_game_module.py). Returns the DLL's folder."""
        out = os.path.join(build_dir, "module", "out")
        jobs = P.job_count()
        self.last_jobs = jobs
        self.log("building the game module with %d parallel jobs (llvm-mingw)" % jobs)
        self.progress(0.42, "Preparing the build")
        cmd = [python_exe(), "-I", os.path.join(ROOT, "native", "tools", "build_game_module.py"),
               "--generated", os.path.join(build_dir, "generated", "default"), "--native", os.path.join(ROOT, "native"), "--simde", self.simde_dir(),
               "--overrides", os.path.join(self.host, HOST_OVERRIDES), "--toolchain", llvm_mingw_dir(), "--ninja", os.path.join(ninja_dir(), "ninja.exe"),
               "--build", os.path.join(build_dir, "module"), "--out", os.path.join(out, "gh2game.dll"), "-j", str(jobs)]
        self.run_ninja_steps(cmd)
        if not os.path.exists(os.path.join(out, "gh2game.dll")):
            raise InstallError("the game module was not built (see the log)")
        return out

    def run_ninja_steps(self, cmd):
        """Runs a build that prints Ninja's "[n/N] ..." lines; the progress line shows the step, the file, the elapsed time and an estimate of what is left."""
        step = re.compile(r"^\[(\d+)/(\d+)\]\s*(?:Building \S+ (?:object|precompiled header) |Linking .*?(?:executable|library) )?(\S*)")
        t0 = time.time()
        now = {"a": 0, "b": 0, "what": "", "at": t0, "done": False}   # the last step ninja reported, and when

        def show():
            a, b = now["a"], now["b"]
            elapsed = time.time() - t0
            if not b:
                self.progress(0.45, "Compiling  -  %s elapsed" % format_elapsed(elapsed))
                return
            # remaining: from the rate so far, minus the time spent on the current step already (a guess only once a few files are done)
            left = max(0.0, (now["at"] - t0) / a * (b - a) - (time.time() - now["at"])) if a >= 5 else None
            self.progress(0.45 + 0.5 * a / b, "Compiling %d/%d%s  -  %s elapsed%s" % (
                a, b, ("  " + now["what"]) if now["what"] else "", format_elapsed(elapsed), ", about %s left" % format_elapsed(left) if left is not None else ""))

        def on_line(line):
            m = step.match(line)
            if m:
                what = re.sub(r"\.(obj|o|pch)$", "", os.path.basename(m.group(3))) if m.group(3) else ""
                now.update(a=int(m.group(1)), b=int(m.group(2)), what=what, at=time.time())
                show()

        def tick():   # between ninja's lines the elapsed time still counts
            while not now["done"]:
                time.sleep(1.0)
                if not now["done"]:
                    show()
        import threading
        threading.Thread(target=tick, daemon=True).start()
        try:
            self.run(cmd, on_line=on_line)
        finally:
            now["done"] = True

    def ensure_runtime(self):
        """What gh2.exe loads, next to it (the Windows loader looks in the executable's folder first): the SDK's DLLs and the C++ runtime
        DLLs of Visual Studio (app-local deployment, which Microsoft's licence allows)."""
        app = os.path.join(self.prefix, "app")
        os.makedirs(app, exist_ok=True)
        if self.host:   # the prebuilt host carries everything: gh2.exe, the SDK's DLLs and the C++ runtime DLLs
            for name in os.listdir(self.host):
                if name not in HOST_NOT_APP and os.path.isfile(os.path.join(self.host, name)):
                    P.replace_file(os.path.join(self.host, name), os.path.join(app, name))
            return
        bindir = os.path.join(self.sdk_prefix(), "bin")
        if os.path.isdir(bindir):
            for name in os.listdir(bindir):
                if name.lower().endswith(".dll"):
                    P.replace_file(os.path.join(bindir, name), os.path.join(app, name))
        if not os.path.exists(os.path.join(app, "rexruntime.dll")):
            raise InstallError("rexruntime.dll not found (SDK at %s)" % self.sdk)
        redist = (self.msvc() or {}).get("redist")
        if redist:
            for name in os.listdir(redist):
                if name.lower().endswith(".dll"):
                    P.replace_file(os.path.join(redist, name), os.path.join(app, name))

    def install_state(self, source):
        meta = self.status()
        if not meta or not os.path.exists(os.path.join(self.prefix, "app", "gh2.exe")):
            return "none"
        if self.host and not os.path.exists(os.path.join(self.prefix, "app", "gh2game.dll")):
            return "outdated"   # (built before the game module existed)
        if self.source_xex_sha1(source) != meta.get("xex_sha1"):
            return "other"
        return "current" if meta.get("fingerprint") == self.fingerprint() else "outdated"

    def install(self, source, rebuild=False, codegen_only=False):
        t0 = time.time()
        os.makedirs(self.prefix, exist_ok=True)
        if not codegen_only and not rebuild and self.install_state(source) == "current":
            self.log("Already installed and up to date: nothing to rebuild (use --rebuild to force it).")
            meta = self.status()
            gd = meta.get("game_dir", "")
            if not (os.path.isdir(gd) and P.find_xex(gd)):
                self.progress(0.1, "Restoring the game files")   # the folder it was installed from is gone: no rebuild needed
                meta["game_dir"] = self.locate_game(source)
                with open(os.path.join(self.prefix, "install.json"), "w", encoding="utf-8") as f:
                    json.dump(meta, f, indent=1)
            self.ensure_runtime()
            self.progress(1.0, "Already installed")
            return meta
        self.progress(0.01, "Checking the build tools")
        self.check_toolchain()
        self.progress(0.02, "Looking for the game")
        game_dir = self.locate_game(source)
        xex = P.find_xex(game_dir)
        digest = self.check_xex(xex)
        if self.host:
            made_for = read_host_meta(self.host).get("xex_sha1")
            if made_for and made_for != digest:   # the hooks name game functions by address: they fit the default.xex they were made with
                raise InstallError("the prebuilt game host fits default.xex %s, this one is %s: another default.xex needs a full build from the sources "
                                   "(a development checkout without GH2_HOST_DIR)" % (made_for, digest))
        build_dir = os.path.join(self.prefix, "build")
        if rebuild or codegen_only or not os.path.isdir(os.path.join(build_dir, "generated", "default")):
            self.prepare_project(build_dir, game_dir, xex)
            self.codegen(build_dir)
        else:   # an interrupted build: keep the generated sources (the slow part), refresh our own sources
            self.log("reusing %s (use --rebuild to regenerate)" % os.path.join(build_dir, "generated"))
            self.copy_sources(build_dir)
        if codegen_only:
            self.log("codegen done: %s" % os.path.join(build_dir, "generated", "default"))
            return None
        out = self.build_module(build_dir) if self.host else self.build(build_dir)
        app = os.path.join(self.prefix, "app")
        os.makedirs(app, exist_ok=True)
        for f in ("gh2.exe", "gh2.pdb", "gh2game.dll", "gh2game.pdb", "rexgpu-xenos.dll"):   # (the PDB: symbol names for crash dumps and scripts\\windows\\sample-threads.ps1)
            src = os.path.join(out, f)
            if os.path.exists(src):
                P.replace_file(src, os.path.join(app, f))
        self.ensure_runtime()
        meta = {"version": __version__, "game_dir": game_dir, "xex_sha1": digest, "sdk": self.sdk, "fingerprint": self.fingerprint(),
                "installed": time.strftime("%Y-%m-%d %H:%M:%S"), "seconds": round(time.time() - t0), "build": self.build_info()}
        with open(os.path.join(self.prefix, "install.json"), "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
        self.progress(1.0, "Done")
        self.log("Installed in %d s." % meta["seconds"])
        return meta

    def stage_host(self, dest):
        """Copies the host of a full build (gh2.exe and everything it loads, except the game module) into `dest`, with the override list and host.json:
        the folder the package ships as host\\ (packaging/windows/make-installer.ps1). Needs a finished full build in this prefix."""
        if self.host:
            raise InstallError("stage_host needs a full build (WinInstaller(full=True))")
        out = os.path.join(self.prefix, "build", "out")
        overrides = os.path.join(out, "native", "module", HOST_OVERRIDES)
        if not (os.path.exists(os.path.join(out, "gh2.exe")) and os.path.exists(overrides)):
            raise InstallError("no full build with the game module in %s: run `gh2win.py build` first" % out)
        meta = self.status() or {}
        if meta.get("fingerprint") != self.fingerprint():   # the host must be built from exactly these sources (its hooks define the module's boundary)
            raise InstallError("the full build in %s is older than these sources: run `gh2win.py build --full` first" % self.prefix)
        if os.path.isdir(dest):
            rmtree_force(dest)
        os.makedirs(dest)
        for f in ("gh2.exe", "gh2.pdb", "rexgpu-xenos.dll"):
            if os.path.exists(os.path.join(out, f)):
                shutil.copy2(os.path.join(out, f), dest)
        bindir = os.path.join(self.sdk_prefix(), "bin")
        dlls = [os.path.join(bindir, n) for n in os.listdir(bindir) if n.lower().endswith(".dll")] if os.path.isdir(bindir) else []
        redist = (self.msvc() or {}).get("redist")   # the C++ runtime DLLs (Microsoft's licence allows shipping them next to the program that uses them)
        dlls += [os.path.join(redist, n) for n in os.listdir(redist) if n.lower().endswith(".dll")] if redist else []
        for p in dlls:
            shutil.copy2(p, dest)
        shutil.copy2(overrides, os.path.join(dest, HOST_OVERRIDES))
        stubs = module_tools()
        names = stubs.read_list(overrides)
        imports = stubs.read_list(os.path.join(ROOT, "native", "rt", "abi", "contract", "imports.txt"))
        host = {"version": __version__, "built": time.strftime("%Y-%m-%d %H:%M:%S"), "contract": contract_version(), "xex_sha1": meta.get("xex_sha1"),
                "imports": len(imports), "imports_hash": "%016X" % stubs.fnv1a(imports), "overrides": len(names), "overrides_hash": "%016X" % stubs.fnv1a(names),
                "crt_dlls": bool(redist)}
        with open(os.path.join(dest, HOST_META), "w", encoding="utf-8") as f:
            json.dump(host, f, indent=1)
        self.log("host staged in %s: %d files, %d overrides, contract %s" % (dest, len(os.listdir(dest)), len(names), host["contract"]))
        return dest

    def make_shortcut(self):
        """The Start menu entries and the optional desktop icon come from Setup.exe (a program folder without them is a portable copy)."""

    # ---- running ------------------------------------------------------------------------------------------------------
    def game_env(self):
        """Environment of the game process: ours plus what the first-start shader build needs (this package's Python and shader tools)."""
        e = dict(os.environ)
        meta = self.status() or {}
        e["REX_GAME_DATA_ROOT"] = meta.get("game_dir", "")
        e["REX_GPU_PLUGIN"] = "xenos"
        e["GH2_NATIVE_TOOLS"] = shader_tools_dir()
        e["GH2_PYTHON"] = python_exe()
        e["PATH"] = os.path.join(self.prefix, "app") + os.pathsep + e.get("PATH", "")
        return e

    def game_exe(self):
        return os.path.join(self.prefix, "app", "gh2.exe")

    def start_game(self, extra_args=(), wait=False):
        """Starts the game from its own folder (logs and gh2.toml live there). Returns the exit code when `wait`, else the Popen."""
        meta = self.status()
        if not meta:
            raise InstallError("the game is not installed yet")
        self.ensure_runtime()
        app = os.path.join(self.prefix, "app")
        flags = 0 if wait else subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        p = subprocess.Popen([self.game_exe(), *extra_args], cwd=app, env=self.game_env(), creationflags=flags, close_fds=True)
        return p.wait() if wait else p

    def launch(self, extra_args=()):
        return self.start_game(extra_args, wait=True)

    # ---- uninstall ----------------------------------------------------------------------------------------------------
    def user_data_dir(self):
        return user_data_dir()

    def running_game_pids(self):
        try:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gh2.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW).stdout
        except OSError:
            return []
        return [int(m.group(1)) for m in re.finditer(r'"gh2\.exe","(\d+)"', out)]

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
        game_dir = (self.status() or {}).get("game_dir")
        if game_dir and os.path.isdir(game_dir) and not os.path.abspath(game_dir).lower().startswith(os.path.abspath(self.prefix).lower() + os.sep):
            keep.append("your own game folder %s is never touched" % game_dir)
        data = self.user_data_dir()
        if os.path.isdir(data):
            if delete_saves:
                items.append((data, size(data)))
            else:
                keep.append("your saves, DLC and achievements in %s are kept" % data)
        return items, keep

    def uninstall(self, delete_saves=False, dry_run=False):
        """Removes the built game. Saves and DLC stay unless `delete_saves`. (The program itself is removed by its uninstaller.)"""
        pids = self.running_game_pids()
        if pids:
            raise InstallError("the game is running (process %s): close it first" % ", ".join(map(str, pids)))
        items, keep = self.uninstall_plan(delete_saves)
        for path, nbytes in items:
            self.log("%s %s (%.1f MB)" % ("would remove" if dry_run else "removing", path, nbytes / 1e6))
        for note in keep:
            self.log("kept: " + note)
        if not dry_run:
            for path, _ in items:
                rmtree_force(path)
        return items, keep

    # ---- the problem report ------------------------------------------------------------------------------------------------
    def system_report_lines(self, first):
        import platform
        import winreg

        def reg(sub, name):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, sub) as k:
                    return str(winreg.QueryValueEx(k, name)[0])
            except OSError:
                return "?"
        nt = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        ms = MemoryStatus()
        ms.dwLength = ctypes.sizeof(ms)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms))
        return ["OS: %s %s" % (reg(nt, "ProductName"), reg(nt, "DisplayVersion")),
                "Kernel: Windows build %s %s" % (reg(nt, "CurrentBuildNumber"), platform.machine()),
                "Session: Windows desktop",
                "CPU: %s (%s threads)" % (reg(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString").strip(), os.cpu_count()),
                "RAM: total %d kB, available %d kB" % (ms.ullTotalPhys // 1024, ms.ullAvailPhys // 1024)]
