"""Command line of the Windows installer (the launcher and Setup.exe use the same code: scripts\\gh2install\\win.py).

    gh2win.py install <folder|.iso|default.xex>   codegen + compile the game from your own copy (first time: several minutes)
    gh2win.py launch [-- game options]            start the game
    gh2win.py status | doctor | report | saves [--export DIR] | uninstall [--delete-saves] [--yes] | update [--apply] | version
    gh2win.py build --game <folder>               the development spelling of install; --prefix / --sdk point at another build folder / SDK
                                                  (with a prebuilt host, GH2_HOST_DIR or the package's host folder, only the game module is compiled;
                                                  --full builds everything with CMake and Visual Studio's C++ tools)
    gh2win.py make-host --out <folder>            after a full build: stage the prebuilt host (gh2.exe and what it loads) the package ships
    gh2win.py codegen-only --game <folder>        stop after the C++ is generated
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

from gh2install import __version__, deps_win, updater, win  # noqa: E402
from gh2install.pipeline import InstallError, KNOWN_XEX  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gh2win", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["install", "build", "codegen-only", "make-host", "launch", "status", "doctor", "report", "saves", "uninstall", "update", "version"])
    ap.add_argument("source", nargs="?", help="install: the .iso, the extracted game folder or default.xex")
    ap.add_argument("--game", help="the game folder (development spelling of the install source)")
    ap.add_argument("--sdk", default=os.environ.get("GH2_SDK"), help="the ReXGlue SDK (holds win-amd64); default: the package's, else GH2_SDK")
    ap.add_argument("--prefix", default=None, help="where the build project and the app go (default: %%LOCALAPPDATA%%\\reGH2\\game)")
    ap.add_argument("--full", action="store_true", help="build: compile the host too (CMake + Visual Studio's C++ tools), not only the game module")
    ap.add_argument("--out", help="make-host: the folder to stage the prebuilt host in")
    ap.add_argument("--rebuild", action="store_true", help="regenerate and recompile even if this game is already installed and up to date")
    ap.add_argument("--force", action="store_true", help="accept a default.xex other than the supported one")
    ap.add_argument("--export", metavar="DIR", help="saves: write a zip with the save game into DIR")
    ap.add_argument("--delete-saves", action="store_true", help="uninstall: also delete saves, installed DLC and achievements")
    ap.add_argument("--yes", action="store_true", help="uninstall: do not ask")
    ap.add_argument("--apply", action="store_true", help="update: download and run the new Setup.exe")
    argv = list(sys.argv[1:] if argv is None else argv)
    game_args = []
    if "--" in argv:   # everything after -- goes to the game
        cut = argv.index("--")
        argv, game_args = argv[:cut], argv[cut + 1:]
    a = ap.parse_args(argv)

    def progress(frac, text):
        if text:
            print("[%3d%%] %s" % (frac * 100, text), flush=True)

    inst = win.WinInstaller(prefix=a.prefix, sdk=a.sdk or "", force=a.force, progress=progress, full=a.full or a.command == "make-host")
    try:
        if a.command in ("install", "build", "codegen-only"):
            source = a.source or a.game or os.path.join(win.ROOT, "Game")
            inst.install(source, rebuild=a.rebuild, codegen_only=a.command == "codegen-only")
        elif a.command == "make-host":
            if not a.out:
                ap.error("make-host needs --out <folder>")
            inst.stage_host(os.path.abspath(a.out))
        elif a.command == "launch":
            problem = deps_win.check()
            if problem:
                print(problem["message"], file=sys.stderr)
            return inst.launch(game_args)
        elif a.command == "status":
            print(inst.status() or "not installed")
        elif a.command == "doctor":
            return deps_win.doctor()
        elif a.command == "report":
            print(inst.write_report())
        elif a.command == "saves":
            print("Save game folder:", inst.user_data_dir())
            if a.export:
                print("Exported:", inst.export_saves(os.path.abspath(a.export)))
        elif a.command == "uninstall":
            inst.uninstall(a.delete_saves, dry_run=True)
            if not a.yes and input("Remove these? [y/N] ").strip().lower() != "y":
                print("nothing removed")
                return 1
            inst.uninstall(a.delete_saves)
        elif a.command == "update":
            info = updater.latest()
            if not updater.is_newer(info):
                print("reGH2 %s is the newest version." % __version__)
            else:
                print("reGH2 %s is available (this is %s): %s" % (info["version"], __version__, info["page"]))
                if a.apply:
                    print(updater.apply(info, progress=lambda f: print("\r[%3d%%] downloading" % (f * 100), end="", flush=True)))
        else:
            print("reGH2", __version__, "- supported default.xex:", ", ".join(KNOWN_XEX))
    except (InstallError, updater.UpdateError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
