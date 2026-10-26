// gh2vk::Context - the Vulkan device, its memory and the swapchain for the game's window (Phase 3).
//
// Bring-up for the native renderer: one instance, one device with the features the translated shaders need (buffer device address, descriptor indexing, 64-bit integers, dynamic
// rendering, synchronization2, BC textures), one graphics queue, VMA for memory, and a swapchain on the window the application hands over (Wayland or Xcb surface handles; this
// library never sees the SDK). GH2_VK_VALIDATION=1 turns on the Khronos validation layer and routes its messages to stderr; GH2_VK_DEVICE=<index or name part> picks the GPU.
// Plain C++17 + Vulkan.
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace gh2vk {

// The window the renderer presents to: raw handles of the platform surface and the live size in pixels (updated by the application).
struct WindowInfo {
  enum class Kind : uint32_t { kNone, kWayland, kXlib } kind = Kind::kNone;
  void* display = nullptr;       // wl_display* / Display*
  uint64_t surface = 0;          // wl_surface* / Window
  const std::atomic<uint32_t>* width = nullptr;
  const std::atomic<uint32_t>* height = nullptr;
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  uint8_t* mapped = nullptr;          // host-visible buffers are mapped for good
  VkDeviceSize size = 0;
  VkDeviceAddress address = 0;        // buffers created with the device-address usage
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, levels = 1, layers = 1;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

class Context {
 public:
  ~Context();
  bool Initialize(const WindowInfo& window, std::string* error);

  // ---- memory ----
  bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer* out);
  void DestroyBuffer(Buffer* buffer);
  // A 2D (or array / cube with `flags`) image with a full view. `view_type` must fit layers.
  bool CreateImage(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageViewType view_type,
                   VkImageAspectFlags aspect, Image* out, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
  void DestroyImage(Image* image);

  // ---- swapchain ----
  struct Frame {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t index = 0;
  };
  // Acquires the next presentable image (recreating the swapchain when the window changed size). Returns false if there is nothing to draw to right now.
  bool AcquireNext(VkSemaphore signal, Frame* out);
  bool Present(const Frame& frame, VkSemaphore wait);
  // One "rendering finished" semaphore per swapchain image: the presentation engine may still hold the previous one, so it cannot be tied to a frame slot.
  VkSemaphore RenderedSemaphore(uint32_t image_index) const { return rendered_[image_index]; }
  VkExtent2D swapchain_extent() const { return extent_; }
  // How pictures reach the screen: 0 FIFO (vertical sync: no tearing, the picture waits for the refresh), 1 mailbox (no tearing, the newest picture wins: lower latency; falls back to FIFO),
  // 2 immediate (no waiting: tearing, lowest latency; falls back to mailbox, then FIFO). Any thread; the swapchain is rebuilt at the next frame. GH2_VK_PRESENT=fifo|mailbox|immediate overrides.
  void SetPresentMode(int mode) { present_request_ = mode; }
  int present_mode() const { return present_active_; }   // the one in use (0, 1, 2)
  VkFormat swapchain_format() const { return swapchain_format_; }

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical_device() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return family_; }
  VmaAllocator allocator() const { return allocator_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  const VkPhysicalDeviceMemoryProperties& memory_properties() const { return memory_properties_; }
  const std::string& device_name() const { return device_name_; }
  // memory in use, from VMA (bytes)
  uint64_t DeviceMemoryInUse() const;

  // Debug names show up in the validation layer and in capture tools.
  void SetName(VkObjectType type, uint64_t handle, const char* name) const;

 private:
  bool CreateSwapchain();
  void DestroySwapchain();

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  VmaAllocator allocator_ = nullptr;
  VkPhysicalDeviceProperties properties_ = {};
  VkPhysicalDeviceMemoryProperties memory_properties_ = {};
  std::string device_name_;
  WindowInfo window_;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_ = {0, 0};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkSemaphore> rendered_;
  PFN_vkSetDebugUtilsObjectNameEXT set_name_ = nullptr;
  std::atomic<int> present_request_{1};
  int present_applied_request_ = -1;   // the request the swapchain was built for
  int present_active_ = 0;
};

}  // namespace gh2vk
