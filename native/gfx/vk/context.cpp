#include "gh2vk/context.h"


#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gh2vk {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr, "VULKAN %s: %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "warning", data->pMessage);
  return VK_FALSE;
}

bool Fail(std::string* error, const char* what, VkResult r = VK_SUCCESS) {
  if (error) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s%s%d", what, r == VK_SUCCESS ? "" : ": VkResult ", r == VK_SUCCESS ? 0 : static_cast<int>(r));
    *error = r == VK_SUCCESS ? what : buf;
  }
  return false;
}

}  // namespace

Context::~Context() {
  if (device_) vkDeviceWaitIdle(device_);
  DestroySwapchain();
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (allocator_) vmaDestroyAllocator(allocator_);
  if (device_) vkDestroyDevice(device_, nullptr);
  if (messenger_) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance_, messenger_, nullptr);
  }
  if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool Context::Initialize(const WindowInfo& window, std::string* error) {
  window_ = window;
  const bool validation = [] { const char* e = std::getenv("GH2_VK_VALIDATION"); return e && *e && *e != '0'; }();

  // ---- instance ----
  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
  if (window.kind == WindowInfo::Kind::kWayland) extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
  else if (window.kind == WindowInfo::Kind::kXlib) extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
  else return Fail(error, "no window to present to");
  std::vector<const char*> layers;
  if (validation) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    layers.push_back("VK_LAYER_KHRONOS_validation");
  }
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "gh2";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  ici.ppEnabledExtensionNames = extensions.data();
  ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
  if (r != VK_SUCCESS) return Fail(error, "vkCreateInstance", r);
  if (validation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    mci.pfnUserCallback = DebugCallback;
    if (create) create(instance_, &mci, nullptr, &messenger_);
  }

  // ---- surface ----
  if (window.kind == WindowInfo::Kind::kWayland) {
    VkWaylandSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    sci.display = static_cast<wl_display*>(window.display);
    sci.surface = reinterpret_cast<wl_surface*>(window.surface);
    r = vkCreateWaylandSurfaceKHR(instance_, &sci, nullptr, &surface_);
  } else {
    VkXlibSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    sci.dpy = static_cast<Display*>(window.display);
    sci.window = static_cast<Window>(window.surface);
    r = vkCreateXlibSurfaceKHR(instance_, &sci, nullptr, &surface_);
  }
  if (r != VK_SUCCESS) return Fail(error, "creating the window surface", r);

  // ---- physical device: one that can present to the window, discrete GPUs first (GH2_VK_DEVICE picks by index or name part) ----
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  const char* wanted = std::getenv("GH2_VK_DEVICE");
  int best_score = -1;
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(devices[i], &props);
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qcount, qprops.data());
    int family = -1;
    for (uint32_t q = 0; q < qcount; ++q) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, surface_, &present);
      if (present && (qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qprops[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qprops[q].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
        family = static_cast<int>(q);
        break;
      }
    }
    if (family < 0) continue;
    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50 : 10;
    if (wanted && *wanted) {
      if (std::string(props.deviceName).find(wanted) != std::string::npos || std::to_string(i) == wanted) score += 1000;
    }
    if (score > best_score) {
      best_score = score;
      physical_ = devices[i];
      family_ = static_cast<uint32_t>(family);
    }
  }
  if (!physical_) return Fail(error, "no Vulkan device can present to the window");
  vkGetPhysicalDeviceProperties(physical_, &properties_);
  vkGetPhysicalDeviceMemoryProperties(physical_, &memory_properties_);
  device_name_ = properties_.deviceName;

  // ---- device ----
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.pNext = &f13;
  f12.bufferDeviceAddress = VK_TRUE;
  f12.runtimeDescriptorArray = VK_TRUE;
  f12.descriptorIndexing = VK_TRUE;
  f12.descriptorBindingPartiallyBound = VK_TRUE;
  f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
  f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  f12.scalarBlockLayout = VK_TRUE;
  f12.timelineSemaphore = VK_TRUE;
  f12.hostQueryReset = VK_TRUE;
  f12.samplerMirrorClampToEdge = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  f2.features.shaderInt64 = VK_TRUE;
  f2.features.samplerAnisotropy = VK_TRUE;
  f2.features.textureCompressionBC = VK_TRUE;
  f2.features.independentBlend = VK_TRUE;
  f2.features.geometryShader = VK_TRUE;
  f2.features.depthClamp = VK_TRUE;
  f2.features.depthBiasClamp = VK_TRUE;
  f2.features.fillModeNonSolid = VK_TRUE;
  f2.features.fragmentStoresAndAtomics = VK_TRUE;
  f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = device_extensions;
  r = vkCreateDevice(physical_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS) return Fail(error, "vkCreateDevice", r);
  vkGetDeviceQueue(device_, family_, 0, &queue_);
  set_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetInstanceProcAddr(instance_, "vkSetDebugUtilsObjectNameEXT"));

  VmaAllocatorCreateInfo aci = {};
  aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  aci.physicalDevice = physical_;
  aci.device = device_;
  aci.instance = instance_;
  aci.vulkanApiVersion = VK_API_VERSION_1_3;
  VmaVulkanFunctions funcs = {};
  funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  aci.pVulkanFunctions = &funcs;
  r = vmaCreateAllocator(&aci, &allocator_);
  if (r != VK_SUCCESS) return Fail(error, "vmaCreateAllocator", r);
  return CreateSwapchain() || Fail(error, "creating the swapchain");
}

uint64_t Context::DeviceMemoryInUse() const {
  if (!allocator_) return 0;
  VmaTotalStatistics stats;
  vmaCalculateStatistics(allocator_, &stats);
  return stats.total.statistics.allocationBytes;
}

void Context::SetName(VkObjectType type, uint64_t handle, const char* name) const {
  if (!set_name_) return;
  VkDebugUtilsObjectNameInfoEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
  info.objectType = type;
  info.objectHandle = handle;
  info.pObjectName = name;
  set_name_(device_, &info);
}

bool Context::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer* out) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo aci = {};
  if (host_visible) {
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  } else {
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  }
  VmaAllocationInfo info = {};
  if (vmaCreateBuffer(allocator_, &bci, &aci, &out->buffer, &out->allocation, &info) != VK_SUCCESS) return false;
  out->size = size;
  out->mapped = static_cast<uint8_t*>(info.pMappedData);
  out->address = 0;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo bda = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bda.buffer = out->buffer;
    out->address = vkGetBufferDeviceAddress(device_, &bda);
  }
  return true;
}

void Context::DestroyBuffer(Buffer* buffer) {
  if (buffer->buffer) vmaDestroyBuffer(allocator_, buffer->buffer, buffer->allocation);
  *buffer = Buffer{};
}

bool Context::CreateImage(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageViewType view_type,
                          VkImageAspectFlags aspect, Image* out, VkSampleCountFlagBits samples) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = flags;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {width, height, 1};
  ici.mipLevels = levels;
  ici.arrayLayers = layers;
  ici.samples = samples;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &ici, &aci, &out->image, &out->allocation, nullptr) != VK_SUCCESS) return false;
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = out->image;
  vci.viewType = view_type;
  vci.format = format;
  vci.subresourceRange = {aspect, 0, levels, 0, layers};
  if (vkCreateImageView(device_, &vci, nullptr, &out->view) != VK_SUCCESS) {
    vmaDestroyImage(allocator_, out->image, out->allocation);
    *out = Image{};
    return false;
  }
  out->format = format;
  out->width = width;
  out->height = height;
  out->levels = levels;
  out->layers = layers;
  out->samples = samples;
  return true;
}

void Context::DestroyImage(Image* image) {
  if (image->view) vkDestroyImageView(device_, image->view, nullptr);
  if (image->image) vmaDestroyImage(allocator_, image->image, image->allocation);
  *image = Image{};
}

// ----------------------------------------------------------------------------------------------------------------------------------- swapchain

bool Context::CreateSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) return false;
  uint32_t w = window_.width ? window_.width->load() : 0, h = window_.height ? window_.height->load() : 0;
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    w = caps.currentExtent.width;
    h = caps.currentExtent.height;
  }
  w = std::clamp(w ? w : 1280u, caps.minImageExtent.width, caps.maxImageExtent.width);
  h = std::clamp(h ? h : 720u, caps.minImageExtent.height, caps.maxImageExtent.height);
  if (w == 0 || h == 0) return false;   // minimised

  uint32_t fcount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fcount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fcount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fcount, formats.data());
  VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
  for (const auto& f : formats)
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;   // the game's output is already display-encoded: no sRGB conversion

  uint32_t pcount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pcount, nullptr);
  std::vector<VkPresentModeKHR> modes(pcount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pcount, modes.data());
  const char* want = std::getenv("GH2_VK_PRESENT");   // fifo | mailbox | immediate: the testing override of the player's option
  int request = present_request_.load();
  if (want && std::string(want) == "fifo") request = 0;
  else if (want && std::string(want) == "mailbox") request = 1;
  else if (want && std::string(want) == "immediate") request = 2;
  auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   // always available
  present_active_ = 0;
  if (request >= 2 && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR, present_active_ = 2;
  else if (request >= 1 && has(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR, present_active_ = 1;   // the game paces itself at 60 Hz: never block it on the display's refresh
  present_applied_request_ = request;

  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = {w, h};
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);   // TRANSFER_SRC: the debugging dump of the presented picture
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;
  VkSwapchainKHR created = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(device_, &sci, nullptr, &created) != VK_SUCCESS) return false;
  vkDeviceWaitIdle(device_);
  for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
  for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
  views_.clear();
  rendered_.clear();
  images_.clear();
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = created;
  swapchain_format_ = chosen.format;
  extent_ = {w, h};
  uint32_t icount = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &icount, nullptr);
  images_.resize(icount);
  vkGetSwapchainImagesKHR(device_, swapchain_, &icount, images_.data());
  for (VkImage image : images_) {
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = chosen.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    vkCreateImageView(device_, &vci, nullptr, &view);
    views_.push_back(view);
    VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore;
    vkCreateSemaphore(device_, &semi, nullptr, &semaphore);
    rendered_.push_back(semaphore);
  }
  std::fprintf(stderr, "VULKAN swapchain %ux%u, %u images, format %d, present mode %d on %s\n", w, h, icount, static_cast<int>(chosen.format), static_cast<int>(mode), device_name_.c_str());
  return true;
}

void Context::DestroySwapchain() {
  for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
  for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
  views_.clear();
  rendered_.clear();
  images_.clear();
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
}

bool Context::AcquireNext(VkSemaphore signal, Frame* out) {
  const uint32_t wanted_w = window_.width ? window_.width->load() : 0, wanted_h = window_.height ? window_.height->load() : 0;
  const bool present_changed = present_request_.load() != present_applied_request_ && !std::getenv("GH2_VK_PRESENT");
  if (!swapchain_ || present_changed || (wanted_w && wanted_h && (wanted_w != extent_.width || wanted_h != extent_.height))) {
    if (!CreateSwapchain()) return false;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    uint32_t index = 0;
    const VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE, &index);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
      out->image = images_[index];
      out->view = views_[index];
      out->index = index;
      return true;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      if (!CreateSwapchain()) return false;
      continue;
    }
    return false;
  }
  return false;
}

bool Context::Present(const Frame& frame, VkSemaphore wait) {
  VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  info.waitSemaphoreCount = 1;
  info.pWaitSemaphores = &wait;
  info.swapchainCount = 1;
  info.pSwapchains = &swapchain_;
  info.pImageIndices = &frame.index;
  const VkResult r = vkQueuePresentKHR(queue_, &info);
  return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

}  // namespace gh2vk
