"""Checks what the host must provide that the AppImage cannot carry: a Vulkan driver for the user's GPU.

The X11 / Wayland client libraries and the Vulkan loader travel inside the AppImage (AppRun uses them only when the
host lacks them). A Vulkan *driver* depends on the GPU, so it is detected here and, with the user's consent,
installed through the distro's package manager.
"""
import glob
import os
import shutil
import subprocess

ICD_DIRS = ("/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
            os.path.expanduser("~/.local/share/vulkan/icd.d"))
GPU_VENDORS = {"0x1002": "amd", "0x8086": "intel", "0x10de": "nvidia"}


def os_release():
    info = {}
    try:
        with open("/etc/os-release") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip().split("=", 1)
                    info[k] = v.strip('"')
    except OSError:
        pass
    return info


def distro_family():
    info = os_release()
    ids = [info.get("ID", "")] + info.get("ID_LIKE", "").split()
    for i in ids:
        if i in ("debian", "ubuntu"):
            return "debian"
        if i in ("fedora", "rhel", "centos"):
            return "fedora"
        if i in ("arch", "archlinux"):
            return "arch"
        if i in ("suse", "opensuse", "sles"):
            return "suse"
    return None


def gpu_vendors():
    found = []
    for path in sorted(glob.glob("/sys/class/drm/card[0-9]*/device/vendor")):
        try:
            v = GPU_VENDORS.get(open(path).read().strip())
        except OSError:
            continue
        if v and v not in found:
            found.append(v)
    return found


def installed_icds():
    out = []
    for d in ICD_DIRS:
        out += glob.glob(os.path.join(d, "*.json"))
    return out


# distro family -> vendor -> packages
PACKAGES = {
    "debian": {"amd": ["mesa-vulkan-drivers"], "intel": ["mesa-vulkan-drivers"]},
    "fedora": {"amd": ["mesa-vulkan-drivers"], "intel": ["mesa-vulkan-drivers"]},
    "arch": {"amd": ["vulkan-radeon"], "intel": ["vulkan-intel"], "nvidia": ["nvidia-utils"]},
    "suse": {"amd": ["libvulkan_radeon"], "intel": ["libvulkan_intel"]},
}
INSTALL_CMD = {
    "debian": ["apt-get", "install", "-y"],
    "fedora": ["dnf", "install", "-y"],
    "arch": ["pacman", "-S", "--needed", "--noconfirm"],
    "suse": ["zypper", "--non-interactive", "install"],
}


def check():
    """Returns None when a Vulkan driver is present, else a dict: message, command (list or None), vendors."""
    if installed_icds():
        return None
    vendors = gpu_vendors()
    family = distro_family()
    pkgs = []
    for v in vendors:
        for p in PACKAGES.get(family, {}).get(v, []):
            if p not in pkgs:
                pkgs.append(p)
    cmd = INSTALL_CMD[family] + pkgs if family and pkgs else None
    msg = "No Vulkan driver was found on this system (the game needs one for your graphics card)."
    if "nvidia" in vendors and "nvidia" not in PACKAGES.get(family, {}):
        msg += " For NVIDIA, install the proprietary NVIDIA driver from your distribution."
    if cmd:
        msg += "\nTo fix it, run as administrator:\n  " + " ".join(cmd)
    elif "nvidia" not in vendors:
        msg += " Install the Vulkan driver package of your GPU (Mesa: mesa-vulkan-drivers / vulkan-radeon / vulkan-intel)."
    return {"message": msg, "command": cmd, "vendors": vendors}


def _clean_env():
    from .pipeline import clean_env
    return clean_env()


def run_privileged(cmd, log=print):
    """Runs a package-manager command as root through pkexec (graphical prompt) or sudo. Returns True on success."""
    for helper in ("pkexec", "sudo"):
        if shutil.which(helper):
            log("$ %s %s" % (helper, " ".join(cmd)))
            r = subprocess.run([helper] + cmd, env=_clean_env())
            return r.returncode == 0
    log("neither pkexec nor sudo is available; run the command above as root")
    return False
