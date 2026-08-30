#if defined(__APPLE__)

#include "render/skia_device.hpp"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImageInfo.h>
#include <core/SkPixmap.h>
#include <core/SkStream.h>
#include <core/SkSurface.h>
#include <encode/SkPngEncoder.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#include <gpu/ganesh/mtl/SkSurfaceMetal.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tokmon::desk {
namespace {

std::string path_utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class SkiaDeviceMetal final : public SkiaDevice {
 public:
  ~SkiaDeviceMetal() override { destroy(); }

  void destroy() {
    surface_.reset();
    if (context_) {
      context_->flushAndSubmit(GrSyncCpu::kYes);
      context_->abandonContext();
      context_.reset();
    }
    drawable_ = nullptr;
    layer_ = nil;
    queue_ = nil;
    device_ = nil;
  }

  bool initialize(SDL_Window* window, int width, int height,
                  std::string& error) {
    window_ = window;
    auto* cocoa_window = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        nullptr);
    if (!cocoa_window) {
      error = "SDL did not expose an NSWindow";
      return false;
    }
    device_ = MTLCreateSystemDefaultDevice();
    queue_ = [device_ newCommandQueue];
    if (!device_ || !queue_) {
      error = "no hardware Metal device/queue is available";
      return false;
    }
    layer_ = [CAMetalLayer layer];
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = NO;
    layer_.opaque = YES;
    cocoa_window.contentView.wantsLayer = YES;
    cocoa_window.contentView.layer = layer_;

    GrMtlBackendContext backend;
    backend.fDevice.retain((__bridge GrMTLHandle)device_);
    backend.fQueue.retain((__bridge GrMTLHandle)queue_);
    context_ = GrDirectContexts::MakeMetal(backend);
    if (!context_) {
      error = "Skia could not create a Ganesh Metal context";
      return false;
    }
    return resize(width, height, error);
  }

  bool resize(int width, int height, std::string&) override {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    layer_.drawableSize = CGSizeMake(width_, height_);
    surface_.reset();
    drawable_ = nullptr;
    return true;
  }

  bool recover(std::string& error) override {
    auto* const window = window_;
    const int width = width_;
    const int height = height_;
    destroy();
    error.clear();
    return initialize(window, width, height, error);
  }

  SkCanvas* begin_frame() override {
    surface_.reset();
    drawable_ = nullptr;
    GrMTLHandle drawable = nullptr;
    surface_ = SkSurfaces::WrapCAMetalLayer(
        context_.get(), (__bridge GrMTLHandle)layer_,
        kTopLeft_GrSurfaceOrigin, 1, kBGRA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr, &drawable);
    drawable_ = (__bridge id<CAMetalDrawable>)drawable;
    auto* canvas = surface_ ? surface_->getCanvas() : nullptr;
    prepare_canvas(canvas);
    return canvas;
  }

  bool end_frame(std::string& error) override {
    if (!surface_ || !drawable_) {
      error = "Metal drawable is unavailable";
      return false;
    }
    context_->flushAndSubmit(GrSyncCpu::kNo);
    [drawable_ present];
    return true;
  }

  bool save_png(const std::filesystem::path& path,
                std::string& error) override {
    if (!surface_) {
      error = "Metal surface is unavailable";
      return false;
    }
    context_->flushAndSubmit(GrSyncCpu::kYes);
    const auto info = SkImageInfo::Make(width_, height_, kRGBA_8888_SkColorType,
                                        kPremul_SkAlphaType);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width_) * height_);
    if (!surface_->readPixels(info, pixels.data(),
                              static_cast<std::size_t>(width_) * 4u, 0, 0)) {
      error = "Skia could not read the Metal drawable";
      return false;
    }
    const SkPixmap pixmap(info, pixels.data(),
                          static_cast<std::size_t>(width_) * 4u);
    SkFILEWStream stream(path_utf8(path).c_str());
    if (!stream.isValid() || !SkPngEncoder::Encode(&stream, pixmap, {})) {
      error = "could not write Metal screenshot PNG";
      return false;
    }
    return true;
  }

  SkSurface* surface() const override { return surface_.get(); }
  int physical_width() const noexcept override { return width_; }
  int physical_height() const noexcept override { return height_; }
  const char* backend_name() const noexcept override {
    return "Skia Ganesh Metal";
  }
  bool hardware_accelerated() const noexcept override { return true; }

 private:
  SDL_Window* window_{nullptr};
  int width_{0};
  int height_{0};
  id<MTLDevice> device_{nil};
  id<MTLCommandQueue> queue_{nil};
  CAMetalLayer* layer_{nil};
  id<CAMetalDrawable> drawable_{nil};
  sk_sp<GrDirectContext> context_;
  sk_sp<SkSurface> surface_;
};

} // namespace

std::unique_ptr<SkiaDevice> create_skia_device_metal(
    SDL_Window* window, const int width, const int height, std::string& error) {
  auto device = std::make_unique<SkiaDeviceMetal>();
  if (!device->initialize(window, width, height, error))
    return {};
  return device;
}

} // namespace tokmon::desk

#endif
