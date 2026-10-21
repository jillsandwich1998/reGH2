#!/usr/bin/env python3
"""Graphical launcher / installer for reGH2, the Guitar Hero II native port (Tk). All game files come from the user's own copy."""
import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from gh2install import __version__, deps, shortcut, updater  # noqa: E402
from gh2install.pipeline import InstallError, Installer, clean_env, default_prefix, find_game  # noqa: E402

import native_dialog  # noqa: E402
from palette import BG, DEEP_RED, DIM, GOLD, ORANGE, PANEL, RED, TEXT, TROUGH  # noqa: E402


class Launcher(tk.Tk):
    def __init__(self, installer_args):
        super().__init__()
        self.title("reGH2")
        self.configure(bg=BG)
        self.geometry("760x680")
        self.minsize(640, 560)
        self.installer_args = installer_args
        self.source = tk.StringVar()
        self.busy = False
        self._style()
        self._build()
        self.refresh()
        self.update_info = None
        if updater.check_enabled():
            self.after(400, self.check_updates)

    # ---- look ---------------------------------------------------------------------------------------------------
    def _style(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        s.configure("TFrame", background=BG)
        s.configure("Panel.TFrame", background=PANEL)
        s.configure("TLabel", background=BG, foreground=TEXT, font=("Sans", 10))
        s.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        s.configure("Dim.TLabel", background=PANEL, foreground=DIM)
        s.configure("Title.TLabel", background=RED, foreground=TEXT, font=("Sans", 16, "bold"), padding=(16, 10))
        s.configure("Head.TLabel", background=PANEL, foreground=GOLD, font=("Sans", 11, "bold"))
        s.configure("TButton", background=DEEP_RED, foreground=TEXT, borderwidth=0, padding=(14, 8), font=("Sans", 10))
        s.map("TButton", background=[("active", ORANGE), ("disabled", TROUGH)], foreground=[("disabled", DIM)])
        s.configure("Play.TButton", background=RED, font=("Sans", 16, "bold"), padding=(30, 14))
        s.map("Play.TButton", background=[("active", ORANGE), ("disabled", TROUGH)])
        s.configure("Horizontal.TProgressbar", troughcolor=TROUGH, background=ORANGE, bordercolor=PANEL, lightcolor=ORANGE,
                    darkcolor=ORANGE)

    def _build(self):
        ttk.Label(self, text="reGH2  -  Guitar Hero II native port", style="Title.TLabel").pack(fill="x")
        body = ttk.Frame(self, padding=16)
        body.pack(fill="both", expand=True)

        self.update_panel = ttk.Frame(body, style="Panel.TFrame", padding=(16, 10))  # shown only when a newer version exists
        self.update_text = ttk.Label(self.update_panel, text="", style="Head.TLabel")
        self.update_text.pack(side="left")
        self.update_btn = ttk.Button(self.update_panel, text="Update now", command=self.apply_update)
        self.update_btn.pack(side="right")
        ttk.Button(self.update_panel, text="What's new", command=lambda: self.open_folder(self.update_info["page"])).pack(
            side="right", padx=8)

        self.state_panel = ttk.Frame(body, style="Panel.TFrame", padding=16)
        self.state_panel.pack(fill="x")
        self.state_head = ttk.Label(self.state_panel, text="", style="Head.TLabel")
        self.state_head.pack(anchor="w")
        self.state_info = ttk.Label(self.state_panel, text="", style="Dim.TLabel", wraplength=680, justify="left")
        self.state_info.pack(anchor="w", pady=(4, 10))
        row = ttk.Frame(self.state_panel, style="Panel.TFrame")
        row.pack(fill="x")
        self.play_btn = ttk.Button(row, text="PLAY", style="Play.TButton", command=self.play)
        self.play_btn.pack(side="left")
        self.folder_btn = ttk.Button(row, text="Open data folder", command=self.open_data)
        self.folder_btn.pack(side="left", padx=12)
        self.report_btn = ttk.Button(row, text="Save problem report", command=self.report)
        self.report_btn.pack(side="left")
        row2 = ttk.Frame(self.state_panel, style="Panel.TFrame")
        row2.pack(fill="x", pady=(10, 0))
        ttk.Button(row2, text="Open saves folder", command=self.open_saves).pack(side="left")
        ttk.Button(row2, text="Export saves...", command=self.export_saves).pack(side="left", padx=12)
        self.uninstall_btn = ttk.Button(row2, text="Uninstall...", command=self.uninstall)
        self.uninstall_btn.pack(side="left")
        self.rebuild_btn = ttk.Button(row2, text="Update game", command=self.rebuild_installed)  # shown when this version differs

        self.install_panel = ttk.Frame(body, style="Panel.TFrame", padding=16)
        self.install_panel.pack(fill="x", pady=(14, 0))
        ttk.Label(self.install_panel, text="INSTALL FROM YOUR COPY OF THE GAME", style="Head.TLabel").pack(anchor="w")
        ttk.Label(self.install_panel, style="Dim.TLabel", wraplength=680, justify="left",
                  text="Select the folder that holds your Guitar Hero II (Xbox 360): the disc image (.iso), the extracted game, or the "
                       "folder with default.xex. Any folder works; the game is looked for inside it. It is rebuilt from your "
                       "files on this computer; nothing of it is downloaded or bundled.").pack(
            anchor="w", pady=(4, 10))
        pick = ttk.Frame(self.install_panel, style="Panel.TFrame")
        pick.pack(fill="x")
        self.pick_btn = ttk.Button(pick, text="Select folder...", command=self.pick_game)
        self.pick_btn.pack(side="left")
        self.found_text = tk.StringVar(value="No folder selected yet.")
        ttk.Label(self.install_panel, textvariable=self.found_text, style="Dim.TLabel", wraplength=680).pack(anchor="w", pady=(8, 8))
        self.desktop_icon = tk.BooleanVar(value=True)
        ttk.Checkbutton(self.install_panel, text="Create a desktop icon (an app menu entry is always created)",
                        variable=self.desktop_icon).pack(anchor="w", pady=(0, 8))
        self.install_btn = ttk.Button(self.install_panel, text="Install", command=self.install)
        self.install_btn.pack(anchor="w")
        self.bar = ttk.Progressbar(self.install_panel, maximum=100)
        self.bar.pack(fill="x", pady=(12, 2))
        self.bar_text = ttk.Label(self.install_panel, text="", style="Dim.TLabel")
        self.bar_text.pack(anchor="w")

        self.log = tk.Text(body, height=8, bg="#120808", fg=DIM, insertbackground=TEXT, relief="flat", font=("Monospace", 9),
                           state="disabled")
        self.log.pack(fill="both", expand=True, pady=(14, 0))
        self.check_var = tk.BooleanVar(value=updater.check_enabled())
        ttk.Checkbutton(body, text="Look for a newer version when this window opens", variable=self.check_var,
                        command=lambda: updater.set_check_enabled(self.check_var.get())).pack(anchor="w", pady=(8, 0))

    # ---- state --------------------------------------------------------------------------------------------------
    def installer(self, **kw):
        return Installer(**self.installer_args, **kw)

    def refresh(self):
        meta = self.installer().status()
        if meta:
            self.state_head.config(text="READY TO PLAY")
            self.state_info.config(text="Installed %s from %s\nSettings are in the game: press F2." %
                                   (meta.get("installed", "?"), meta.get("game_dir", "?")))
            self.play_btn.state(["!disabled"])
            if self.installer().game_outdated():
                self.state_info.config(text=self.state_info.cget("text") + "\nThis version of reGH2 has changes the installed game "
                                       "does not have yet: press Update game (a few minutes, your saves are kept).")
                self.rebuild_btn.pack(side="left", padx=12)
            else:
                self.rebuild_btn.pack_forget()
        else:
            self.rebuild_btn.pack_forget()
            self.state_head.config(text="NOT INSTALLED YET")
            self.state_info.config(text="Choose your copy of the game below and press Install. The first install compiles "
                                        "the game on this computer (a few minutes).")
            self.play_btn.state(["disabled"])

    def write(self, text):
        def do():
            self.log.config(state="normal")
            self.log.insert("end", text + "\n")
            self.log.see("end")
            self.log.config(state="disabled")
        self.after(0, do)

    def set_progress(self, frac, text):
        def do():
            self.bar["value"] = frac * 100
            self.bar_text.config(text=text or "")
        self.after(0, do)

    # ---- actions ------------------------------------------------------------------------------------------------
    def pick_game(self):
        """One button: the user picks a folder; the game (disc image, extracted folder or default.xex) is found inside it."""
        if self.busy:
            return
        start = os.path.dirname(self.source.get()) if self.source.get() else os.path.expanduser("~")

        def ask():
            folder = native_dialog.pick_directory("Select the folder with your Guitar Hero II", start)
            self.after(0, lambda: self.picked(folder))

        self.pick_btn.state(["disabled"])
        threading.Thread(target=ask, daemon=True).start()

    def picked(self, folder):
        self.pick_btn.state(["!disabled"])
        if folder is None:  # no native dialog on this system
            folder = filedialog.askdirectory(title="Select the folder with your Guitar Hero II")
        if not folder:
            return
        found = find_game(folder)
        if not found:
            self.source.set("")
            self.found_text.set("No Guitar Hero II found in %s (looked for a disc image .iso and for default.xex)." % folder)
            self.set_install_state("none")
            return
        self.source.set(found)
        state = self.installer().install_state(found)
        note = {"current": "  -  already installed and up to date, nothing to rebuild.",
                "outdated": "  -  installed, but this version of the port is newer: Update rebuilds it.",
                "other": "  -  a different version of the game than the installed one: Install rebuilds it."}.get(state, "")
        self.found_text.set("Found: %s%s" % (found, note))
        self.set_install_state(state)

    def set_install_state(self, state):
        self.install_state = state
        self.install_btn.config(text={"current": "Reinstall", "outdated": "Update"}.get(state, "Install"))

    def install(self):
        if self.busy:
            return
        src = self.source.get().strip()
        if not src:
            messagebox.showinfo("Install", "Select the folder with your Guitar Hero II first.")
            return
        rebuild = False
        if getattr(self, "install_state", "none") == "current":
            if not messagebox.askyesno("Already installed", "This game is already installed and up to date.\n\n"
                                       "Rebuild it anyway? That takes a few minutes and is only useful if something is broken."):
                return
            rebuild = True
        self.busy = True
        self.install_btn.state(["disabled"])
        self.play_btn.state(["disabled"])

        want_icon = self.desktop_icon.get()

        def work():
            try:
                self.installer(log=self.write, progress=self.set_progress,
                               desktop_icon=want_icon).install(src, rebuild=rebuild)
                self.after(0, lambda: (self.set_install_state("current"),
                                       messagebox.showinfo("Install", "Done. Press PLAY.")))
            except InstallError as e:
                self.write("ERROR: %s" % e)
                self.after(0, lambda: messagebox.showerror("Install failed", str(e)[:1500]))
            except Exception as e:  # noqa: BLE001 - show anything unexpected to the user
                self.write("ERROR: %r" % (e,))
                self.after(0, lambda: messagebox.showerror("Install failed", repr(e)))
            finally:
                def done():
                    self.busy = False
                    self.install_btn.state(["!disabled"])
                    self.refresh()
                self.after(0, done)

        threading.Thread(target=work, daemon=True).start()

    def rebuild_installed(self):
        """Rebuilds the installed game with this version, from the game files it was built from."""
        meta = self.installer().status() or {}
        self.source.set(meta.get("game_dir", ""))
        self.install_state = "outdated"
        self.install()

    # ---- updates ------------------------------------------------------------------------------------------------
    def check_updates(self):
        def work():
            try:
                info = updater.latest()
            except updater.UpdateError as e:
                self.write("Update check skipped: %s" % e)
                return
            if updater.is_newer(info):
                self.after(0, lambda: self.show_update(info))
        threading.Thread(target=work, daemon=True).start()

    def show_update(self, info):
        self.update_info = info
        self.update_text.config(text="reGH2 %s is available (you have %s)" % (info["version"], __version__))
        self.update_panel.pack(fill="x", pady=(0, 14), before=self.state_panel)

    def restart_args(self):
        flags = {"sdk": "--sdk=", "cxx": "--cxx=", "toolchain_file": "--toolchain-file=", "prefix": "--prefix="}
        return [flags[k] + v for k, v in self.installer_args.items() if k in flags and v]

    def apply_update(self):
        if self.busy or not self.update_info:
            return
        info = self.update_info
        if not updater.target_appimage():
            messagebox.showinfo("Update", "This copy is not running from an AppImage, so it cannot replace itself.\n\n"
                                "Download the new version here:\n" + info["page"])
            return
        self.busy = True
        self.update_btn.state(["disabled"])

        def work():
            try:
                path, replaced = updater.apply(info, progress=lambda f: self.set_progress(f, "Downloading reGH2 %s" % info["version"]),
                                               log=self.write)
            except updater.UpdateError as e:
                self.write("ERROR: %s" % e)
                self.after(0, lambda: messagebox.showerror("Update failed", str(e)))
                self.after(0, lambda: (setattr(self, "busy", False), self.update_btn.state(["!disabled"]), self.set_progress(0, "")))
                return
            self.set_progress(1, "Updated")

            def done():
                if replaced:
                    messagebox.showinfo("Updated", "reGH2 %s is installed. The launcher will restart now." % info["version"])
                    subprocess.Popen([path, *self.restart_args()], env=clean_env(), start_new_session=True)
                else:
                    messagebox.showinfo("Updated", "The new version was saved as:\n%s\n\nStart it from there." % path)
                self.destroy()
            self.after(0, done)

        threading.Thread(target=work, daemon=True).start()

    def check_vulkan(self):
        """True when a Vulkan driver is present or the user chose to go on; offers to install a missing driver."""
        problem = deps.check()
        if not problem:
            return True
        if problem["command"]:
            if messagebox.askyesno("Vulkan driver missing", problem["message"].split("\n")[0] +
                                   "\n\nInstall it now? Your system will ask for the administrator password:\n  " +
                                   " ".join(problem["command"])):
                ok = deps.run_privileged(problem["command"], log=self.write)
                if not ok:
                    messagebox.showerror("Vulkan driver", "The installation did not complete. See the log.")
                return ok
            return False
        return messagebox.askokcancel("Vulkan driver missing", problem["message"] + "\n\nTry to start anyway?")

    def play(self):
        inst = self.installer()
        meta = inst.status()
        if not meta or not self.check_vulkan():
            return
        app = os.path.join(inst.prefix, "app")
        try:
            inst.ensure_runtime()  # the game must not depend on this AppImage staying mounted
        except InstallError as e:
            messagebox.showerror("Cannot start", str(e))
            return
        env = inst.game_env()
        subprocess.Popen([os.path.join(app, "gh2")], env=env, cwd=app, start_new_session=True)
        self.after(1500, self.destroy)

    def open_folder(self, folder):
        # programs outside the AppImage must not see its bundled libraries (the file manager would not even start)
        for opener in (["xdg-open"], ["gio", "open"], ["dolphin"], ["nautilus"], ["thunar"]):
            try:
                subprocess.Popen(opener + [folder], env=clean_env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
                return
            except OSError:
                continue
        messagebox.showinfo("Folder", "No file manager could be started. The folder is:\n\n" + folder)

    def open_data(self):
        folder = self.installer().prefix
        if not os.path.isdir(folder):
            messagebox.showinfo("Data folder", "Nothing is installed yet, so there is no data folder.\n\n" + folder)
            return
        self.open_folder(folder)

    def open_saves(self):
        folder = self.installer().user_data_dir()
        if not os.path.isdir(folder):
            messagebox.showinfo("Saves", "There is no save game yet: it is created when you play.\n\n" + folder)
            return
        self.open_folder(folder)

    def export_saves(self):
        """Copies the save game and achievements into a zip in a folder the player picks."""
        inst = self.installer()
        if not os.path.isdir(inst.user_data_dir()):
            messagebox.showinfo("Saves", "There is no save game yet: it is created when you play.")
            return

        def ask():
            folder = native_dialog.pick_directory("Where do you want the saves copied to?", os.path.expanduser("~"))
            self.after(0, lambda: self.exported(folder))

        threading.Thread(target=ask, daemon=True).start()

    def exported(self, folder):
        if folder is None:  # no native dialog on this system
            folder = filedialog.askdirectory(title="Where do you want the saves copied to?")
        if not folder:
            return
        try:
            path = self.installer().export_saves(folder)
        except (InstallError, OSError) as e:
            messagebox.showerror("Saves", str(e))
            return
        self.write("Saves exported: %s" % path)
        messagebox.showinfo("Saves", "Saved a copy of your save game and achievements:\n\n%s" % path)

    def uninstall(self):
        """Removes the installed game and its shortcuts; saves and DLC are kept unless the user also says so."""
        inst = self.installer()
        if not inst.status():
            messagebox.showinfo("Uninstall", "Nothing is installed.")
            return
        try:
            items, keep = inst.uninstall_plan()
        except InstallError as e:
            messagebox.showerror("Uninstall", str(e))
            return
        total = sum(n for _, n in items) / 1e9
        if not messagebox.askyesno("Uninstall", "Remove the installed game and its shortcuts (about %.1f GB)?\n\n%s" %
                                   (total, "\n".join(keep) or "Nothing else is touched.")):
            return
        delete_saves = os.path.isdir(inst.user_data_dir()) and messagebox.askyesno(
            "Saves", "Also delete your saves, installed DLC and achievements?\n\n%s\n\nThis cannot be undone. (\"Export saves...\" makes a copy first.)" % inst.user_data_dir(),
            default="no")
        try:
            inst.uninstall(delete_saves)
        except InstallError as e:
            messagebox.showerror("Uninstall", str(e))
            return
        self.write("Uninstalled.")
        self.refresh()
        messagebox.showinfo("Uninstall", "The game was removed.")

    def report(self):
        """Saves a problem report (system, settings, log tails): for bugs where the game did not start or has crashed."""
        try:
            path = self.installer().write_report()
        except OSError as e:
            messagebox.showerror("Problem report", "Could not save the report: %s" % e)
            return
        self.write("Problem report saved: %s" % path)
        messagebox.showinfo("Problem report", "Saved:\n%s\n\nAttach this file to a GitHub issue and say at what moment of the "
                            "game the problem happens." % path)
        self.open_folder(os.path.dirname(path))


def main():
    args = {}
    for i, a in enumerate(sys.argv[1:]):
        if a.startswith("--sdk="):
            args["sdk"] = a[6:]
        elif a.startswith("--cxx="):
            args["cxx"] = a[6:]
        elif a.startswith("--toolchain-file="):
            args["toolchain_file"] = a[17:]
        elif a.startswith("--prefix="):
            args["prefix"] = a[9:]
    try:  # keep the shortcuts valid if the AppImage was moved; add the menu entry to installs made before it existed
        if Installer(args.get("prefix")).status() and not os.path.exists(shortcut.menu_entry_path()) and os.environ.get("APPIMAGE"):
            shortcut.create(os.environ["APPIMAGE"], os.path.join(os.environ.get("APPDIR", ""), "gh2-recomp.png"),
                            desktop_icon=False, log=lambda *_: None)
        shortcut.refresh(os.environ.get("APPIMAGE"), os.path.join(os.environ.get("APPDIR", ""), "gh2-recomp.png"))
    except OSError:
        pass
    Launcher(args).mainloop()


if __name__ == "__main__":
    main()
