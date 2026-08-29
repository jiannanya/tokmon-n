#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>

class SkCanvas;
class SkSurface;

namespace tokmon::desk {

class SkiaDevice {
public:
  virtual ~SkiaDevice() = default;

  SkiaDevice(const SkiaDevice&) = delete;
  SkiaDevice& operator=(const SkiaDevice&) = delete;

  virtual bool resize(int physical_width, int physical_height,
                      std::string& error) = 0;
  virtual SkCanvas* begin_frame() = 0;
  virtual bool end_frame(std::string& error) = 0;
  virtual bool save_png(const std::filesystem::path& path,
                        std::string& error) = 0;

  [[nodiscard]] virtual SkSurface* surface() const = 0;
  [[nodiscard]] virtual int physical_width() const noexcept = 0;
  [[nodiscard]] virtual int physical_height() const noexcept = 0;
  [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
  [[nodiscard]] virtual bool hardware_accelerated() const noexcept = 0;

  void set_ui_scale(float scale) noexcept;
  [[nodiscard]] float ui_scale() const noexcept { return ui_scale_; }
  [[nodiscard]] int logical_width() const noexcept;
  [[nodiscard]] int logical_height() const noexcept;

  static std::unique_ptr<SkiaDevice>
  create(SDL_Window* window, int physical_width, int physical_height,
         bool allow_software_renderer, std::string& error);

protected:
  SkiaDevice() = default;
  void prepare_canvas(SkCanvas* canvas) const;

private:
  float ui_scale_{1.f};
};

} // namespace tokmon::desk
