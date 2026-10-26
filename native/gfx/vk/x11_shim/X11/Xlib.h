// Stand-in for <X11/Xlib.h>, used only when the machine has no X11 development files (the player's build with the AppImage's toolchain). <vulkan/vulkan.h> with
// VK_USE_PLATFORM_XLIB_KHR needs just these three names to declare vkCreateXlibSurfaceKHR; the renderer hands over the handles SDL gives it and never calls Xlib itself.
#pragma once
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;
typedef unsigned long VisualID;
