import argparse
import os
import sys

from . import __version__
from . import deps, shortcut
from .pipeline import Installer, InstallError, KNOWN_XEX


def doctor(ask):
    problem = deps.check()
    if not problem:
        print("Vulkan driver found.")
        return 0
    print(problem["message"], flush=True)
    if ask and problem["command"]:
        if input("Install it now (asks for your password)? [y/N] ").strip().lower() == "y":
            return 0 if deps.run_privileged(problem["command"]) else 1
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="gh2-recomp", description="reGH2 (Guitar Hero II native port): install from your own copy")
    ap.add_argument("--prefix", help="install folder (default: ~/.local/share/gh2-recomp)")
    ap.add_argument("--sdk", help="folder of the prebuilt ReXGlue SDK (default: ./sdk or $GH2_SDK)")
    ap.add_argument("--cxx", help="C++ compiler (clang++ 20 or newer; default: $GH2_CXX or clang++)")
    ap.add_argument("--toolchain-file", help="CMake toolchain file (bundled compiler + sysroot)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("install", help="build the game from your disc image / game folder / default.xex")
    p.add_argument("source", help="Guitar Hero II: .iso image, extracted game folder, or default.xex")
    p.add_argument("--no-desktop-icon", action="store_true", help="do not put an icon on the desktop (the app menu entry is always made)")
    p.add_argument("--rebuild", action="store_true", help="rebuild even if this game is already installed and up to date")
    p.add_argument("--force", action="store_true", help="continue even if default.xex is an unknown version")
    sub.add_parser("status", help="show what is installed")
    sub.add_parser("launch", help="start the game (extra options are passed to it, e.g. --startup_mode=menu)")
    p = sub.add_parser("shortcut", help="create or remove the app menu entry and desktop icon")
    p.add_argument("--remove", action="store_true")
    p = sub.add_parser("uninstall", help="remove the installed game and its shortcuts (saves and DLC are kept)")
    p.add_argument("--delete-saves", action="store_true", help="also delete saves, installed DLC and achievements (cannot be undone)")
    p.add_argument("--dry-run", action="store_true", help="only show what would be removed")
    p.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    p = sub.add_parser("saves", help="show where the save game is, or copy it into a zip")
    p.add_argument("--export", metavar="FOLDER", help="write a zip with the save game and achievements into FOLDER")
    sub.add_parser("report", help="save a problem report (system, settings, logs) to attach to a GitHub issue")
    sub.add_parser("doctor", help="check the system for a Vulkan driver and offer to install it")
    sub.add_parser("version")
    a, extra = ap.parse_known_args(argv)  # for `launch`, unknown options go to the game
    inst = Installer(a.prefix, a.sdk, a.cxx, a.toolchain_file, force=getattr(a, "force", False), desktop_icon=not getattr(a, "no_desktop_icon", False),
                     progress=lambda f, t: print("[%3d%%] %s" % (f * 100, t), flush=True) if t else None)
    try:
        if a.cmd == "install":
            inst.install(a.source, rebuild=a.rebuild)
        elif a.cmd == "status":
            print(inst.status() or "not installed")
        elif a.cmd == "shortcut":
            if a.remove:
                shortcut.remove()
            else:
                inst.make_shortcut()
        elif a.cmd == "uninstall":
            if not a.dry_run and not a.yes:
                if not sys.stdin.isatty():
                    raise InstallError("not a terminal: add --yes to confirm, or --dry-run to look first")
                inst.uninstall(a.delete_saves, dry_run=True)
                if input("Remove these? [y/N] ").strip().lower() != "y":
                    print("nothing removed")
                    return 1
            inst.uninstall(a.delete_saves, dry_run=a.dry_run)
        elif a.cmd == "saves":
            print("Save game folder:", inst.user_data_dir())
            if a.export:
                print("Exported:", inst.export_saves(os.path.abspath(a.export)))
        elif a.cmd == "report":
            print(inst.write_report())
        elif a.cmd == "doctor":
            return doctor(ask=True)
        elif a.cmd == "launch":
            doctor(ask=sys.stdin.isatty())
            inst.launch(extra)
        else:
            print("gh2-recomp", __version__, "- supported default.xex:", ", ".join(KNOWN_XEX))
    except InstallError as e:
        print("error:", e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
