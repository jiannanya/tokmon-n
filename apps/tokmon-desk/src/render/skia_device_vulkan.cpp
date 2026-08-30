#if defined(__linux__)

#include "render/skia_device.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImageInfo.h>
#include <core/SkPixmap.h>
#include <core/SkStream.h>
#include <core/SkSurface.h>
#include <encode/SkPngEncoder.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <gpu/ganesh/vk/GrVkBackendSurface.h>
#include <gpu/ganesh/vk/GrVkDirectContext.h>
#include <gpu/ganesh/vk/GrVkTypes.h>
#include <gpu/vk/VulkanBackendContext.h>
#include <gpu/vk/VulkanMemoryAllocator.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tokmon::desk {
namespace {

std::string path_utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string vk_error(const char* operation, const VkResult result) {
  return std::string(operation) + " failed (VkResult " +
         std::to_string(static_cast<int>(result)) + ")";
}

class DedicatedVulkanAllocator final : public skgpu::VulkanMemoryAllocator {
 public:
  DedicatedVulkanAllocator(VkPhysicalDevice physical_device, VkDevice device)
      : device_(device) {
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties_);
  }

  VkResult allocateImageMemory(VkImage image, uint32_t flags,
                               skgpu::VulkanBackendMemory* memory) override {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image, &requirements);
    return allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, flags,
                    image, VK_NULL_HANDLE, memory);
  }

  VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage,
                                uint32_t flags,
                                skgpu::VulkanBackendMemory* memory) override {
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    VkMemoryPropertyFlags desired = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (usage != BufferUsage::kGpuOnly)
      desired = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    return allocate(requirements, desired, flags, VK_NULL_HANDLE, buffer,
                    memory);
  }

  void getAllocInfo(const skgpu::VulkanBackendMemory& memory,
                    skgpu::VulkanAlloc* info) const override {
    const auto* allocation = from(memory);
    *info = {};
    if (!allocation)
      return;
    info->fMemory = allocation->memory;
    info->fOffset = 0;
    info->fSize = allocation->size;
    info->fBackendMemory = memory;
    if (allocation->host_visible)
      info->fFlags |= skgpu::VulkanAlloc::kMappable_Flag;
    if (!allocation->coherent)
      info->fFlags |= skgpu::VulkanAlloc::kNoncoherent_Flag;
  }

  void* mapMemory(const skgpu::VulkanBackendMemory& memory) override {
    auto* allocation = from(memory);
    if (!allocation || !allocation->host_visible)
      return nullptr;
    if (!allocation->mapped && vkMapMemory(device_, allocation->memory, 0,
                                            allocation->size, 0,
                                            &allocation->mapped) != VK_SUCCESS)
      return nullptr;
    return allocation->mapped;
  }

  void unmapMemory(const skgpu::VulkanBackendMemory& memory) override {
    auto* allocation = from(memory);
    if (allocation && allocation->mapped) {
      vkUnmapMemory(device_, allocation->memory);
      allocation->mapped = nullptr;
    }
  }

  VkResult flushMemory(const skgpu::VulkanBackendMemory& memory,
                       VkDeviceSize offset, VkDeviceSize size) override {
    const auto* allocation = from(memory);
    if (!allocation || allocation->coherent)
      return VK_SUCCESS;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = allocation->memory;
    range.offset = offset;
    range.size = size;
    return vkFlushMappedMemoryRanges(device_, 1, &range);
  }

  VkResult invalidateMemory(const skgpu::VulkanBackendMemory& memory,
                            VkDeviceSize offset, VkDeviceSize size) override {
    const auto* allocation = from(memory);
    if (!allocation || allocation->coherent)
      return VK_SUCCESS;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = allocation->memory;
    range.offset = offset;
    range.size = size;
    return vkInvalidateMappedMemoryRanges(device_, 1, &range);
  }

  void freeMemory(const skgpu::VulkanBackendMemory& memory) override {
    auto* allocation = from(memory);
    if (!allocation)
      return;
    if (allocation->mapped)
      vkUnmapMemory(device_, allocation->memory);
    vkFreeMemory(device_, allocation->memory, nullptr);
    allocated_ -= allocation->size;
    delete allocation;
  }

  std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override {
    return {allocated_, allocated_};
  }

 private:
  struct Allocation {
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{0};
    void* mapped{nullptr};
    bool host_visible{false};
    bool coherent{false};
  };

  static Allocation* from(const skgpu::VulkanBackendMemory memory) {
    return reinterpret_cast<Allocation*>(memory);
  }

  VkResult allocate(const VkMemoryRequirements& requirements,
                    VkMemoryPropertyFlags desired, uint32_t,
                    VkImage image, VkBuffer buffer,
                    skgpu::VulkanBackendMemory* output) {
    uint32_t selected = std::numeric_limits<uint32_t>::max();
    for (uint32_t index = 0; index < properties_.memoryTypeCount; ++index) {
      if (!(requirements.memoryTypeBits & (1u << index)))
        continue;
      const auto flags = properties_.memoryTypes[index].propertyFlags;
      if ((flags & desired) == desired) {
        selected = index;
        break;
      }
      if (selected == std::numeric_limits<uint32_t>::max())
        selected = index;
    }
    if (selected == std::numeric_limits<uint32_t>::max())
      return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = selected;
    auto* allocation = new Allocation;
    auto result = vkAllocateMemory(device_, &info, nullptr, &allocation->memory);
    if (result != VK_SUCCESS) {
      delete allocation;
      return result;
    }
    result = image ? vkBindImageMemory(device_, image, allocation->memory, 0)
                   : vkBindBufferMemory(device_, buffer, allocation->memory, 0);
    if (result != VK_SUCCESS) {
      vkFreeMemory(device_, allocation->memory, nullptr);
      delete allocation;
      return result;
    }
    allocation->size = requirements.size;
    const auto flags = properties_.memoryTypes[selected].propertyFlags;
    allocation->host_visible = flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    allocation->coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    allocated_ += allocation->size;
    *output = reinterpret_cast<skgpu::VulkanBackendMemory>(allocation);
    return VK_SUCCESS;
  }

  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDeviceMemoryProperties properties_{};
  std::uint64_t allocated_{0};
};

class SkiaDeviceVulkan final : public SkiaDevice {
 public:
  ~SkiaDeviceVulkan() override { destroy(); }

  bool initialize(SDL_Window* window, int width, int height,
                  std::string& error) {
    window_ = window;
    Uint32 extension_count = 0;
    const auto extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!extensions || extension_count == 0) {
      error = SDL_GetError();
      return false;
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "tokmon-desk";
    application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application.pEngineName = "Skia Ganesh";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = extension_count;
    instance_info.ppEnabledExtensionNames = extensions;
    auto result = vkCreateInstance(&instance_info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
      error = vk_error("vkCreateInstance", result);
      return false;
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_handle_)) {
      error = SDL_GetError();
      return false;
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    for (const auto candidate : devices) {
      uint32_t queue_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count,
                                                queues.data());
      for (uint32_t index = 0; index < queue_count; ++index)
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            SDL_Vulkan_GetPresentationSupport(instance_, candidate, index)) {
          physical_device_ = candidate;
          queue_family_ = index;
          break;
        }
      if (physical_device_)
        break;
    }
    if (!physical_device_) {
      error = "no Vulkan 1.1 graphics/present queue is available";
      return false;
    }
    const float priority = 1.f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
      error = vk_error("vkCreateDevice", result);
      return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    allocator_ = sk_make_sp<DedicatedVulkanAllocator>(physical_device_, device_);
    skgpu::VulkanBackendContext backend;
    backend.fInstance = instance_;
    backend.fPhysicalDevice = physical_device_;
    backend.fDevice = device_;
    backend.fQueue = queue_;
    backend.fGraphicsQueueIndex = queue_family_;
    backend.fMaxAPIVersion = VK_API_VERSION_1_1;
    backend.fMemoryAllocator = allocator_;
    backend.fGetProc = [](const char* name, VkInstance instance, VkDevice device) {
      return device ? vkGetDeviceProcAddr(device, name)
                    : vkGetInstanceProcAddr(instance, name);
    };
    context_ = GrDirectContexts::MakeVulkan(backend);
    if (!context_) {
      error = "Skia could not create a Ganesh Vulkan context";
      return false;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(device_, &fence_info, nullptr, &acquire_fence_);
    if (result != VK_SUCCESS) {
      error = vk_error("vkCreateFence", result);
      return false;
    }
    return setup_swapchain(width, height, error);
  }

  bool resize(int width, int height, std::string& error) override {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == width_ && height == height_ && swapchain_)
      return true;
    return setup_swapchain(width, height, error);
  }

  bool recover(std::string& error) override {
    auto* const window = window_;
    const int width = width_;
    const int height = height_;
    destroy(true);
    error.clear();
    return initialize(window, width, height, error);
  }

  SkCanvas* begin_frame() override {
    if (!swapchain_)
      return nullptr;
    vkResetFences(device_, 1, &acquire_fence_);
    auto result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                        VK_NULL_HANDLE, acquire_fence_,
                                        &image_index_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
      return nullptr;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
      return nullptr;
    vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, UINT64_MAX);
    auto* canvas = surfaces_[image_index_]
        ? surfaces_[image_index_]->getCanvas() : nullptr;
    prepare_canvas(canvas);
    return canvas;
  }

  bool end_frame(std::string& error) override {
    if (image_index_ >= surfaces_.size() || !surfaces_[image_index_]) {
      error = "Vulkan swapchain surface is unavailable";
      return false;
    }
    GrFlushInfo flush_info;
    context_->flush(surfaces_[image_index_].get(),
                    SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
    context_->submit(GrSyncCpu::kYes);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index_;
    const auto result = vkQueuePresentKHR(queue_, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      error = vk_error("vkQueuePresentKHR", result);
      return false;
    }
    return true;
  }

  bool save_png(const std::filesystem::path& path,
                std::string& error) override {
    if (image_index_ >= surfaces_.size() || !surfaces_[image_index_]) {
      error = "Vulkan surface is unavailable";
      return false;
    }
    context_->flushAndSubmit(GrSyncCpu::kYes);
    const auto info = SkImageInfo::Make(width_, height_, kRGBA_8888_SkColorType,
                                        kPremul_SkAlphaType);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width_) * height_);
    if (!surfaces_[image_index_]->readPixels(
            info, pixels.data(), static_cast<std::size_t>(width_) * 4u, 0, 0)) {
      error = "Skia could not read the Vulkan backbuffer";
      return false;
    }
    const SkPixmap pixmap(info, pixels.data(),
                          static_cast<std::size_t>(width_) * 4u);
    SkFILEWStream stream(path_utf8(path).c_str());
    if (!stream.isValid() || !SkPngEncoder::Encode(&stream, pixmap, {})) {
      error = "could not write Vulkan screenshot PNG";
      return false;
    }
    return true;
  }

  SkSurface* surface() const override {
    return image_index_ < surfaces_.size() ? surfaces_[image_index_].get()
                                           : nullptr;
  }
  int physical_width() const noexcept override { return width_; }
  int physical_height() const noexcept override { return height_; }
  const char* backend_name() const noexcept override {
    return "Skia Ganesh Vulkan";
  }
  bool hardware_accelerated() const noexcept override { return true; }

 private:
  bool setup_swapchain(int requested_width, int requested_height,
                       std::string& error) {
    if (!device_ || !surface_handle_)
      return false;
    vkDeviceWaitIdle(device_);
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_handle_,
                                              &capabilities);
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_handle_,
                                        &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_handle_,
                                        &format_count, formats.data());
    if (formats.empty()) {
      error = "Vulkan surface has no supported format";
      return false;
    }
    auto chosen = formats.front();
    for (const auto& format : formats)
      if (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
          format.format == VK_FORMAT_B8G8R8A8_SRGB) {
        chosen = format;
        break;
      }
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<uint32_t>::max()) {
      extent.width = std::clamp(static_cast<uint32_t>(requested_width),
                                capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width);
      extent.height = std::clamp(static_cast<uint32_t>(requested_height),
                                 capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height);
    }
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
      image_count = capabilities.maxImageCount;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_handle_;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = capabilities.supportedCompositeAlpha &
                                  VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    auto result = vkCreateSwapchainKHR(device_, &info, nullptr, &replacement);
    if (result != VK_SUCCESS) {
      error = vk_error("vkCreateSwapchainKHR", result);
      return false;
    }
    surfaces_.clear();
    images_.clear();
    if (swapchain_)
      vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = replacement;
    width_ = static_cast<int>(extent.width);
    height_ = static_cast<int>(extent.height);
    format_ = chosen.format;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images_.data());
    surfaces_.reserve(images_.size());
    for (const auto image : images_) {
      GrVkImageInfo image_info;
      image_info.fImage = image;
      image_info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      image_info.fFormat = format_;
      image_info.fImageUsageFlags = info.imageUsage;
      image_info.fSampleCount = 1;
      image_info.fLevelCount = 1;
      image_info.fCurrentQueueFamily = queue_family_;
      const auto target = GrBackendRenderTargets::MakeVk(width_, height_,
                                                         image_info);
      auto surface = SkSurfaces::WrapBackendRenderTarget(
          context_.get(), target, kTopLeft_GrSurfaceOrigin,
          kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
      if (!surface) {
        error = "Skia could not wrap a Vulkan swapchain image";
        return false;
      }
      surfaces_.push_back(std::move(surface));
    }
    image_index_ = 0;
    return true;
  }

  void destroy(const bool force = false) {
    if (device_ && !force)
      vkDeviceWaitIdle(device_);
    surfaces_.clear();
    context_.reset();
    allocator_.reset();
    if (acquire_fence_)
      vkDestroyFence(device_, acquire_fence_, nullptr);
    if (swapchain_)
      vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (device_)
      vkDestroyDevice(device_, nullptr);
    if (surface_handle_)
      SDL_Vulkan_DestroySurface(instance_, surface_handle_, nullptr);
    if (instance_)
      vkDestroyInstance(instance_, nullptr);
    acquire_fence_ = VK_NULL_HANDLE;
    swapchain_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    surface_handle_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    images_.clear();
  }

  SDL_Window* window_{nullptr};
  int width_{0};
  int height_{0};
  VkInstance instance_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_handle_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue queue_{VK_NULL_HANDLE};
  uint32_t queue_family_{0};
  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  VkFence acquire_fence_{VK_NULL_HANDLE};
  uint32_t image_index_{0};
  std::vector<VkImage> images_;
  std::vector<sk_sp<SkSurface>> surfaces_;
  sk_sp<DedicatedVulkanAllocator> allocator_;
  sk_sp<GrDirectContext> context_;
};

} // namespace

std::unique_ptr<SkiaDevice> create_skia_device_vulkan(
    SDL_Window* window, const int width, const int height, std::string& error) {
  auto device = std::make_unique<SkiaDeviceVulkan>();
  if (!device->initialize(window, width, height, error))
    return {};
  return device;
}

} // namespace tokmon::desk

#endif
