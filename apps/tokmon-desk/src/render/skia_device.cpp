#include "render/skia_device.hpp"

#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkImage.h>
#include <core/SkImageInfo.h>
#include <core/SkPixmap.h>
#include <core/SkStream.h>
#include <core/SkSurface.h>
#include <encode/SkPngEncoder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tokmon::desk {

#if defined(_WIN32)
std::unique_ptr<SkiaDevice> create_skia_device_d3d12(
    SDL_Window*, int, int, std::string&);
#endif
#if defined(__APPLE__)
std::unique_ptr<SkiaDevice> create_skia_device_metal(
    SDL_Window*, int, int, std::string&);
#endif
#if defined(__linux__)
std::unique_ptr<SkiaDevice> create_skia_device_vulkan(
    SDL_Window*, int, int, std::string&);
#endif

namespace {

std::string path_utf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class RasterDiagnosticDevice final : public SkiaDevice {
public:
  RasterDiagnosticDevice(SDL_Window* window, int width, int height,
                         std::string& error)
      : window_(window) {
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) {
      error = SDL_GetError();
      return;
    }
    SDL_SetRenderVSync(renderer_, 1);
    resize(width, height, error);
  }

  ~RasterDiagnosticDevice() override {
    surface_.reset();
    if (texture_)
      SDL_DestroyTexture(texture_);
    if (renderer_)
      SDL_DestroyRenderer(renderer_);
  }

  bool valid() const noexcept { return renderer_ && texture_ && surface_; }

  bool resize(int width, int height, std::string& error) override {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == width_ && height == height_ && surface_)
      return true;
    if (texture_) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width_) * height_, 0u);
    const auto info = SkImageInfo::Make(width_, height_, kRGBA_8888_SkColorType,
                                        kPremul_SkAlphaType);
    surface_ = SkSurfaces::WrapPixels(
        info, pixels_.data(), static_cast<std::size_t>(width_) * 4u);
    if (!surface_) {
      error = "Skia could not create the diagnostic raster surface";
      return false;
    }
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING, width_, height_);
    if (!texture_) {
      error = SDL_GetError();
      return false;
    }
    return true;
  }

  SkCanvas* begin_frame() override {
    auto* canvas = surface_ ? surface_->getCanvas() : nullptr;
    prepare_canvas(canvas);
    return canvas;
  }

  bool end_frame(std::string& error) override {
    if (!surface_ || !texture_)
      return false;
    if (!SDL_UpdateTexture(texture_, nullptr, pixels_.data(), width_ * 4) ||
        !SDL_RenderClear(renderer_) ||
        !SDL_RenderTexture(renderer_, texture_, nullptr, nullptr) ||
        !SDL_RenderPresent(renderer_)) {
      error = SDL_GetError();
      return false;
    }
    return true;
  }

  bool save_png(const std::filesystem::path& path,
                std::string& error) override {
    SkPixmap pixmap;
    if (!surface_ || !surface_->peekPixels(&pixmap)) {
      error = "Skia raster pixels are unavailable";
      return false;
    }
    SkFILEWStream stream(path_utf8(path).c_str());
    if (!stream.isValid() || !SkPngEncoder::Encode(&stream, pixmap, {})) {
      error = "could not write PNG";
      return false;
    }
    return true;
  }

  SkSurface* surface() const override { return surface_.get(); }
  int physical_width() const noexcept override { return width_; }
  int physical_height() const noexcept override { return height_; }
  const char* backend_name() const noexcept override {
    return "Skia raster diagnostic";
  }
  bool hardware_accelerated() const noexcept override { return false; }

private:
  SDL_Window* window_{nullptr};
  SDL_Renderer* renderer_{nullptr};
  SDL_Texture* texture_{nullptr};
  int width_{0};
  int height_{0};
  std::vector<std::uint32_t> pixels_;
  sk_sp<SkSurface> surface_;
};

} // namespace

void SkiaDevice::set_ui_scale(const float scale) noexcept {
  ui_scale_ = std::clamp(scale, 0.7f, 2.f);
}

int SkiaDevice::logical_width() const noexcept {
  return std::max(1, static_cast<int>(std::lround(physical_width() / ui_scale_)));
}

int SkiaDevice::logical_height() const noexcept {
  return std::max(1, static_cast<int>(std::lround(physical_height() / ui_scale_)));
}

void SkiaDevice::prepare_canvas(SkCanvas* canvas) const {
  if (!canvas)
    return;
  canvas->restoreToCount(1);
  canvas->resetMatrix();
  canvas->clear(SkColorSetRGB(245, 245, 244));
  canvas->scale(ui_scale_, ui_scale_);
}

std::unique_ptr<SkiaDevice> SkiaDevice::create(
    SDL_Window* window, const int width, const int height,
    const bool allow_software_renderer, std::string& error) {
  std::unique_ptr<SkiaDevice> result;
#if defined(_WIN32)
  result = create_skia_device_d3d12(window, width, height, error);
#elif defined(__APPLE__)
  result = create_skia_device_metal(window, width, height, error);
#elif defined(__linux__)
  result = create_skia_device_vulkan(window, width, height, error);
#else
  error = "tokmon-desk has no Skia GPU backend for this platform";
#endif
  if (result || !allow_software_renderer)
    return result;

  std::string raster_error;
  auto raster = std::make_unique<RasterDiagnosticDevice>(
      window, width, height, raster_error);
  if (!raster->valid()) {
    error += "; raster diagnostic fallback failed: " + raster_error;
    return {};
  }
  error.clear();
  return raster;
}

} // namespace tokmon::desk
