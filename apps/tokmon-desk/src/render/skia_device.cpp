#include "render/skia_device.hpp"

#include <core/SkCanvas.h>
#include <core/SkBlurTypes.h>
#include <core/SkColor.h>
#include <core/SkImage.h>
#include <core/SkImageInfo.h>
#include <core/SkMaskFilter.h>
#include <core/SkPaint.h>
#include <core/SkPixmap.h>
#include <core/SkRect.h>
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
    pixels_.assign(static_cast<std::size_t>(width_) *
                       static_cast<std::size_t>(height_),
                   0u);
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

  bool recover(std::string& error) override {
    const int width = width_;
    const int height = height_;
    surface_.reset();
    if (texture_) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    return resize(width, height, error);
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

void SkiaDevice::set_frame_density(const float density) noexcept {
  frame_density_ = std::clamp(density, 0.7f, 4.f);
}

int SkiaDevice::logical_width() const noexcept {
  return std::max(1, static_cast<int>(std::lround(
      static_cast<float>(physical_width()) / ui_scale_)));
}

int SkiaDevice::logical_height() const noexcept {
  return std::max(1, static_cast<int>(std::lround(
      static_cast<float>(physical_height()) / ui_scale_)));
}

void SkiaDevice::prepare_canvas(SkCanvas* canvas) const {
  if (!canvas)
    return;
  canvas->restoreToCount(1);
  canvas->resetMatrix();
  // The old frameless Slint window leaves its inset transparent.  The native
  // compositor in the frozen Windows golden resolves that gutter against a
  // white desktop before the inner app surface casts its shadow.
  canvas->clear(SK_ColorWHITE);
  // RmlUi clips decorators to the document viewport, while the old Slint shell
  // paints its native-window shadow outside the scaled application surface.
  // Draw that one compositor-level primitive before the Rml document.  The
  // opaque body covers the shadow's interior during normal rendering.
  const float inset = 6.4f * frame_density_;
  const float radius = 4.f * frame_density_;
  SkPaint frame_shadow;
  frame_shadow.setAntiAlias(true);
  frame_shadow.setColor(SkColorSetARGB(56, 28, 25, 23));
  frame_shadow.setMaskFilter(SkMaskFilter::MakeBlur(
      kOuter_SkBlurStyle, 3.2f * frame_density_, false));
  canvas->drawRoundRect(
      SkRect::MakeLTRB(inset, inset,
                       static_cast<float>(physical_width()) - inset,
                       static_cast<float>(physical_height()) - inset),
      radius, radius, frame_shadow);
  // Windows adds a crisp inner edge to the native shadow backing before the
  // scaled application layer begins.  It is separate from the RCSS body's
  // hairline and is visible in the frozen capture at the outer edge only.
  const float backing_inset = 4.8f * frame_density_;
  SkPaint backing_edge;
  backing_edge.setAntiAlias(true);
  backing_edge.setStyle(SkPaint::kStroke_Style);
  backing_edge.setStrokeWidth(2.f);
  backing_edge.setColor(SkColorSetARGB(52, 28, 25, 23));
  canvas->drawRoundRect(
      SkRect::MakeLTRB(backing_inset, backing_inset,
                       static_cast<float>(physical_width()) - backing_inset,
                       static_cast<float>(physical_height()) - backing_inset),
      radius + 2.f, radius + 2.f, backing_edge);
  // RmlUi is given the physical framebuffer dimensions and resolves all
  // legacy design units through its dp ratio. Keep the final Skia canvas at a
  // one-to-one device-pixel transform so glyph/SVG atlases are never enlarged
  // after rasterization.
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
