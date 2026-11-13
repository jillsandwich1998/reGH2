"""What the Windows package cannot carry, checked: a Vulkan driver for the user's GPU (it comes with the graphics driver: vulkan-1.dll is in System32 when
there is one).

Nothing else is needed from the machine. The game code is compiled on the user's PC from their own default.xex, but only as the game module gh2game.dll, with
llvm-mingw, which the package carries (clang, lld, libc++ and the mingw-w64 headers and libraries are all redistributable); the rest of the game comes prebuilt.
No Microsoft build tool is downloaded or installed.

Same shape as gh2install.deps on Linux: check() returns None or {"message", "command"}.
"""
import ctypes
import sys

from . import win


# ---- Vulkan -----------------------------------------------------------------------------------------------------------------------
def vulkan_devices():
    """The number of Vulkan devices the installed driver reports, or None when there is no Vulkan loader / it fails."""
    try:
        vk = ctypes.WinDLL("vulkan-1.dll")
    except OSError:
        return None

    class InstanceInfo(ctypes.Structure):   # VkInstanceCreateInfo, nothing enabled
        _fields_ = [("sType", ctypes.c_int32), ("pNext", ctypes.c_void_p), ("flags", ctypes.c_uint32), ("pApplicationInfo", ctypes.c_void_p),
                    ("enabledLayerCount", ctypes.c_uint32), ("ppEnabledLayerNames", ctypes.c_void_p),
                    ("enabledExtensionCount", ctypes.c_uint32), ("ppEnabledExtensionNames", ctypes.c_void_p)]
    info = InstanceInfo(sType=1)   # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    instance = ctypes.c_void_p()
    try:
        if vk.vkCreateInstance(ctypes.byref(info), None, ctypes.byref(instance)) != 0:
            return 0
        count = ctypes.c_uint32(0)
        vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
        vk.vkDestroyInstance(instance, None)
        return count.value
    except (OSError, AttributeError):
        return None


def check():
    """None when a Vulkan driver is present, else {"message", "command": None} (a driver comes from the GPU maker, there is nothing to run)."""
    n = vulkan_devices()
    if n:
        return None
    if n is None:
        why = "Windows has no Vulkan loader (vulkan-1.dll)."
    else:
        why = "Vulkan is installed but reports no graphics card."
    return {"message": why + "\nreGH2 draws with Vulkan: install the latest graphics driver from your GPU's maker (AMD, NVIDIA or Intel) and try again.", "command": None}


def doctor(log=print):
    """Prints the state of what the game needs and of the tools that build it; returns the process exit code."""
    bad = 0
    problem = check()
    log("Vulkan: %s" % ("ok (%d device(s))" % vulkan_devices() if not problem else problem["message"].split("\n")[0]))
    bad += bool(problem)
    host = win.host_dir()
    log("Prebuilt game host: %s" % (host or "none (a development checkout: the full build needs CMake, LLVM and Visual Studio's C++ tools)"))
    tools = (("llvm-mingw", win.llvm_mingw_dir()), ("Ninja", win.ninja_dir())) if host else \
            (("LLVM", win.llvm_bin()), ("llvm-mingw", win.llvm_mingw_dir()), ("CMake", win.cmake_bin()), ("Ninja", win.ninja_dir(win.find_msvc())),
             ("Visual Studio C++ tools", (win.find_msvc() or {}).get("path")), ("Vulkan headers", win.vulkan_dev()[0]))
    for name, found in tools:
        log("%s: %s" % (name, found or "missing"))
        bad += not found
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(doctor())
