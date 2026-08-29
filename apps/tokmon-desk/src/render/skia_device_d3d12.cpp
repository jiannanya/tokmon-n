#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "render/skia_device.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImageInfo.h>
#include <core/SkPixmap.h>
#include <core/SkStream.h>
#include <core/SkSurface.h>
#include <encode/SkPngEncoder.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkSurfaceGanesh.h>
#include <gpu/ganesh/d3d/GrD3DBackendContext.h>
#include <gpu/ganesh/d3d/GrD3DBackendSurface.h>
#include <gpu/ganesh/d3d/GrD3DDirectContext.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tokmon::desk {
namespace {

using Microsoft::WRL::ComPtr;

std::string hr_message(const char* operation, const HRESULT result) {
  char buffer[96]{};
  std::snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lx)",
                operation, static_cast<unsigned long>(result));
  return buffer;
}

std::string path_utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class SkiaDeviceD3D12 final : public SkiaDevice {
public:
  ~SkiaDeviceD3D12() override { destroy(); }

  bool initialize(SDL_Window* window, int width, int height,
                  std::string& error) {
    window_ = window;
    hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
    if (!hwnd_) {
      error = "SDL did not expose a Win32 HWND";
      return false;
    }

    UINT factory_flags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    HRESULT result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
    if (FAILED(result)) {
      error = hr_message("CreateDXGIFactory2", result);
      return false;
    }

    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> candidate;
      result = factory_->EnumAdapterByGpuPreference(
          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
          IID_PPV_ARGS(&candidate));
      if (result == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(result))
        continue;
      DXGI_ADAPTER_DESC1 description{};
      candidate->GetDesc1(&description);
      if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        continue;
      if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&device_)))) {
        adapter_ = candidate;
        break;
      }
    }
    if (!device_) {
      error = "no hardware D3D12 adapter is available";
      return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = device_->CreateCommandQueue(&queue_description,
                                         IID_PPV_ARGS(&queue_));
    if (FAILED(result)) {
      error = hr_message("ID3D12Device::CreateCommandQueue", result);
      return false;
    }

    GrD3DBackendContext backend_context;
    backend_context.fAdapter.retain(adapter_.Get());
    backend_context.fDevice.retain(device_.Get());
    backend_context.fQueue.retain(queue_.Get());
    context_ = GrDirectContexts::MakeD3D(backend_context);
    if (!context_) {
      error = "Skia could not create a Ganesh D3D12 context";
      return false;
    }

    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    DXGI_SWAP_CHAIN_DESC1 swap_description{};
    swap_description.BufferCount = kFrameCount;
    swap_description.Width = static_cast<UINT>(width_);
    swap_description.Height = static_cast<UINT>(height_);
    swap_description.Format = kSwapchainFormat;
    swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_description.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapchain;
    result = factory_->CreateSwapChainForHwnd(
        queue_.Get(), hwnd_, &swap_description, nullptr, nullptr, &swapchain);
    if (FAILED(result)) {
      error = hr_message("IDXGIFactory::CreateSwapChainForHwnd", result);
      return false;
    }
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    result = swapchain.As(&swapchain_);
    if (FAILED(result)) {
      error = hr_message("IDXGISwapChain1::QueryInterface", result);
      return false;
    }

    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    fence_values_.fill(0);
    result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&fence_));
    if (FAILED(result)) {
      error = hr_message("ID3D12Device::CreateFence", result);
      return false;
    }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      error = "CreateEventW failed for the D3D12 frame fence";
      return false;
    }
    return setup_surfaces(error);
  }

  bool resize(int width, int height, std::string& error) override {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == width_ && height == height_ && surfaces_[0])
      return true;
    if (!context_ || !swapchain_)
      return false;

    context_->flushAndSubmit(GrSyncCpu::kYes);
    wait_for_all_frames();
    for (auto& surface : surfaces_)
      surface.reset();
    for (auto& buffer : buffers_)
      buffer.Reset();

    const HRESULT result = swapchain_->ResizeBuffers(
        kFrameCount, static_cast<UINT>(width), static_cast<UINT>(height),
        kSwapchainFormat, 0);
    if (FAILED(result)) {
      error = hr_message("IDXGISwapChain::ResizeBuffers", result);
      return false;
    }
    width_ = width;
    height_ = height;
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    return setup_surfaces(error);
  }

  SkCanvas* begin_frame() override {
    if (!swapchain_)
      return nullptr;
    frame_index_ = swapchain_->GetCurrentBackBufferIndex();
    wait_for_frame(frame_index_);
    auto* canvas = surfaces_[frame_index_]
                       ? surfaces_[frame_index_]->getCanvas()
                       : nullptr;
    prepare_canvas(canvas);
    return canvas;
  }

  bool end_frame(std::string& error) override {
    auto* current_surface = surfaces_[frame_index_].get();
    if (!context_ || !current_surface)
      return false;
    GrFlushInfo flush_info;
    context_->flush(current_surface,
                    SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
    context_->submit();
    HRESULT result = swapchain_->Present(1, 0);
    if (FAILED(result)) {
      error = hr_message("IDXGISwapChain::Present", result);
      return false;
    }
    const auto signal_value = next_fence_value_++;
    result = queue_->Signal(fence_.Get(), signal_value);
    if (FAILED(result)) {
      error = hr_message("ID3D12CommandQueue::Signal", result);
      return false;
    }
    fence_values_[frame_index_] = signal_value;
    return true;
  }

  bool save_png(const std::filesystem::path& path,
                std::string& error) override {
    auto* current_surface = surfaces_[frame_index_].get();
    if (!current_surface) {
      error = "D3D12 backbuffer surface is unavailable";
      return false;
    }
    context_->flushAndSubmit(GrSyncCpu::kYes);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width_) * height_);
    const auto info = SkImageInfo::Make(width_, height_, kRGBA_8888_SkColorType,
                                        kPremul_SkAlphaType);
    if (!current_surface->readPixels(info, pixels.data(),
                                     static_cast<std::size_t>(width_) * 4u,
                                     0, 0)) {
      error = "Skia could not read the D3D12 backbuffer";
      return false;
    }
    const SkPixmap pixmap(info, pixels.data(),
                          static_cast<std::size_t>(width_) * 4u);
    SkFILEWStream stream(path_utf8(path).c_str());
    if (!stream.isValid() || !SkPngEncoder::Encode(&stream, pixmap, {})) {
      error = "could not write GPU screenshot PNG";
      return false;
    }
    return true;
  }

  SkSurface* surface() const override {
    return surfaces_[frame_index_].get();
  }
  int physical_width() const noexcept override { return width_; }
  int physical_height() const noexcept override { return height_; }
  const char* backend_name() const noexcept override {
    return "Skia Ganesh D3D12";
  }
  bool hardware_accelerated() const noexcept override { return true; }

private:
  static constexpr UINT kFrameCount = 2;
  static constexpr DXGI_FORMAT kSwapchainFormat =
      DXGI_FORMAT_R8G8B8A8_UNORM;

  bool setup_surfaces(std::string& error) {
    GrD3DTextureResourceInfo info(
        nullptr, nullptr, D3D12_RESOURCE_STATE_PRESENT, kSwapchainFormat, 1, 1,
        0, skgpu::Protected::kNo);
    for (UINT index = 0; index < kFrameCount; ++index) {
      HRESULT result = swapchain_->GetBuffer(index, IID_PPV_ARGS(&buffers_[index]));
      if (FAILED(result)) {
        error = hr_message("IDXGISwapChain::GetBuffer", result);
        return false;
      }
      info.fResource.retain(buffers_[index].Get());
      const auto backend_target = GrBackendRenderTargets::MakeD3D(
          width_, height_, info);
      surfaces_[index] = SkSurfaces::WrapBackendRenderTarget(
          context_.get(), backend_target, kTopLeft_GrSurfaceOrigin,
          kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
      if (!surfaces_[index]) {
        error = "Skia could not wrap a D3D12 swapchain backbuffer";
        return false;
      }
    }
    return true;
  }

  void wait_for_frame(const UINT index) {
    const auto required = fence_values_[index];
    if (!required || fence_->GetCompletedValue() >= required)
      return;
    fence_->SetEventOnCompletion(required, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }

  void wait_for_all_frames() {
    for (UINT index = 0; index < kFrameCount; ++index)
      wait_for_frame(index);
  }

  void destroy() {
    if (context_)
      context_->flushAndSubmit(GrSyncCpu::kYes);
    if (fence_ && fence_event_)
      wait_for_all_frames();
    for (auto& surface : surfaces_)
      surface.reset();
    for (auto& buffer : buffers_)
      buffer.Reset();
    if (context_) {
      context_->abandonContext();
      context_.reset();
    }
    swapchain_.Reset();
    fence_.Reset();
    queue_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    if (fence_event_)
      CloseHandle(fence_event_);
    fence_event_ = nullptr;
  }

  SDL_Window* window_{nullptr};
  HWND hwnd_{nullptr};
  int width_{0};
  int height_{0};
  UINT frame_index_{0};
  std::uint64_t next_fence_value_{1};
  HANDLE fence_event_{nullptr};
  ComPtr<IDXGIFactory6> factory_;
  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12Fence> fence_;
  std::array<std::uint64_t, kFrameCount> fence_values_{};
  std::array<ComPtr<ID3D12Resource>, kFrameCount> buffers_;
  std::array<sk_sp<SkSurface>, kFrameCount> surfaces_;
  sk_sp<GrDirectContext> context_;
};

} // namespace

std::unique_ptr<SkiaDevice> create_skia_device_d3d12(
    SDL_Window* window, const int width, const int height, std::string& error) {
  auto device = std::make_unique<SkiaDeviceD3D12>();
  if (!device->initialize(window, width, height, error))
    return {};
  return device;
}

} // namespace tokmon::desk

#endif
