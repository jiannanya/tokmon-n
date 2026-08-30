#pragma once

#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <functional>
#include <string>

namespace Rml { class Context; }

namespace tokmon::desk {

class SdlPlatform final : public Rml::SystemInterface {
public:
  SdlPlatform();
  ~SdlPlatform() override;

  bool initialize(const char* title, int width, int height, std::string& error);
  void shutdown();
  bool wait_for_event(int timeout_ms);
  bool pump_event(Rml::Context& context, bool& quit, bool& resized);

  [[nodiscard]] SDL_Window* window() const noexcept { return window_; }
  [[nodiscard]] int pixel_width() const noexcept;
  [[nodiscard]] int pixel_height() const noexcept;
  [[nodiscard]] float display_scale() const noexcept;
  [[nodiscard]] int default_ui_scale_percent() const noexcept;
  [[nodiscard]] int default_content_scale_percent() const noexcept;
  void set_ui_scale(float scale) noexcept;
  void size_window_for_ui_scale(int logical_width, int logical_height,
                                float window_scale);
  [[nodiscard]] float ui_scale() const noexcept { return ui_scale_; }
  [[nodiscard]] float input_coordinate_scale() const noexcept;

  void minimize();
  void toggle_maximize();
  void begin_window_drag();
  void set_raw_event_handler(
      std::function<bool(const SDL_Event&)> handler);

  double GetElapsedTime() override;
  void SetMouseCursor(const Rml::String& name) override;
  void SetClipboardText(const Rml::String& text) override;
  void GetClipboardText(Rml::String& text) override;
  void ActivateKeyboard(Rml::Vector2f position, float line_height) override;
  void DeactivateKeyboard() override;

private:
  SDL_Window* window_{nullptr};
  SDL_Cursor* cursor_arrow_{nullptr};
  SDL_Cursor* cursor_pointer_{nullptr};
  SDL_Cursor* cursor_text_{nullptr};
  SDL_Cursor* cursor_move_{nullptr};
  std::chrono::steady_clock::time_point start_;
  float ui_scale_{1.f};
  std::function<bool(const SDL_Event&)> raw_event_handler_;
};

} // namespace tokmon::desk
