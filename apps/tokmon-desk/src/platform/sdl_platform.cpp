#include "platform/sdl_platform.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cmath>

namespace tokmon::desk {
namespace {

int modifiers() {
  const auto state = SDL_GetModState();
  int result = 0;
  if (state & SDL_KMOD_CTRL) result |= Rml::Input::KM_CTRL;
  if (state & SDL_KMOD_SHIFT) result |= Rml::Input::KM_SHIFT;
  if (state & SDL_KMOD_ALT) result |= Rml::Input::KM_ALT;
  if (state & SDL_KMOD_GUI) result |= Rml::Input::KM_META;
  if (state & SDL_KMOD_CAPS) result |= Rml::Input::KM_CAPSLOCK;
  if (state & SDL_KMOD_NUM) result |= Rml::Input::KM_NUMLOCK;
  return result;
}

Rml::Input::KeyIdentifier key_identifier(SDL_Keycode key) {
  using namespace Rml::Input;
  if (key >= SDLK_A && key <= SDLK_Z)
    return static_cast<KeyIdentifier>(KI_A + (key - SDLK_A));
  if (key >= SDLK_0 && key <= SDLK_9)
    return static_cast<KeyIdentifier>(KI_0 + (key - SDLK_0));
  switch (key) {
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_TAB: return KI_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return KI_RETURN;
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    case SDLK_END: return KI_END;
    case SDLK_HOME: return KI_HOME;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_UP: return KI_UP;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_INSERT: return KI_INSERT;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_F1: return KI_F1;
    case SDLK_F2: return KI_F2;
    case SDLK_F3: return KI_F3;
    case SDLK_F4: return KI_F4;
    case SDLK_F5: return KI_F5;
    case SDLK_F6: return KI_F6;
    case SDLK_F7: return KI_F7;
    case SDLK_F8: return KI_F8;
    case SDLK_F9: return KI_F9;
    case SDLK_F10: return KI_F10;
    case SDLK_F11: return KI_F11;
    case SDLK_F12: return KI_F12;
    default: return KI_UNKNOWN;
  }
}

int mouse_button(Uint8 value) {
  if (value == SDL_BUTTON_LEFT) return 0;
  if (value == SDL_BUTTON_RIGHT) return 1;
  if (value == SDL_BUTTON_MIDDLE) return 2;
  return 3;
}

} // namespace

SdlPlatform::SdlPlatform() : start_(std::chrono::steady_clock::now()) {}
SdlPlatform::~SdlPlatform() { shutdown(); }

bool SdlPlatform::initialize(const char* title, int width, int height,
                             std::string& error) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    error = SDL_GetError();
    return false;
  }
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                          SDL_WINDOW_BORDERLESS;
#if defined(__linux__)
  flags |= SDL_WINDOW_VULKAN;
#endif
  window_ = SDL_CreateWindow(title, width, height, flags);
  if (!window_) {
    error = SDL_GetError();
    return false;
  }
  SDL_SetWindowMinimumSize(window_, 980, 620);
  cursor_arrow_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  cursor_pointer_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  cursor_text_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
  cursor_move_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
  return true;
}

void SdlPlatform::shutdown() {
  if (cursor_arrow_) SDL_DestroyCursor(cursor_arrow_);
  if (cursor_pointer_) SDL_DestroyCursor(cursor_pointer_);
  if (cursor_text_) SDL_DestroyCursor(cursor_text_);
  if (cursor_move_) SDL_DestroyCursor(cursor_move_);
  cursor_arrow_ = cursor_pointer_ = cursor_text_ = cursor_move_ = nullptr;
  if (window_) SDL_DestroyWindow(window_);
  window_ = nullptr;
  SDL_Quit();
}

bool SdlPlatform::wait_for_event(const int timeout_ms) {
  SDL_Event event{};
  if (!SDL_WaitEventTimeout(&event, std::max(timeout_ms, 0)))
    return false;
  // Keep one event-processing path so raw editor/terminal input, RmlUi input
  // scaling and window events cannot drift apart. SDL_PushEvent preserves the
  // event for pump_event() immediately below in the application loop.
  return SDL_PushEvent(&event);
}

bool SdlPlatform::pump_event(Rml::Context& context, bool& quit, bool& resized) {
  SDL_Event event{};
  if (!SDL_PollEvent(&event))
    return false;
  if (raw_event_handler_ &&
      (event.type == SDL_EVENT_KEY_DOWN ||
       event.type == SDL_EVENT_KEY_UP ||
       event.type == SDL_EVENT_TEXT_INPUT ||
       event.type == SDL_EVENT_MOUSE_WHEEL) &&
      raw_event_handler_(event))
    return true;
  const float density = SDL_GetWindowPixelDensity(window_);
  const float input_scale = density / ui_scale_;
  switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      quit = true;
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      resized = true;
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      context.SetDensityIndependentPixelRatio(display_scale());
      resized = true;
      break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      context.ProcessMouseLeave();
      break;
    case SDL_EVENT_MOUSE_MOTION:
      context.ProcessMouseMove(static_cast<int>(event.motion.x * input_scale),
                               static_cast<int>(event.motion.y * input_scale), modifiers());
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      context.ProcessMouseButtonDown(mouse_button(event.button.button), modifiers());
      SDL_CaptureMouse(true);
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      SDL_CaptureMouse(false);
      context.ProcessMouseButtonUp(mouse_button(event.button.button), modifiers());
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      context.ProcessMouseWheel(-event.wheel.y, modifiers());
      break;
    case SDL_EVENT_KEY_DOWN:
      context.ProcessKeyDown(key_identifier(event.key.key), modifiers());
      if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
        context.ProcessTextInput('\n');
      break;
    case SDL_EVENT_KEY_UP:
      context.ProcessKeyUp(key_identifier(event.key.key), modifiers());
      break;
    case SDL_EVENT_TEXT_INPUT:
      context.ProcessTextInput(Rml::String(event.text.text));
      break;
    default:
      break;
  }
  return true;
}

int SdlPlatform::pixel_width() const noexcept {
  int width = 0, height = 0;
  if (window_) SDL_GetWindowSizeInPixels(window_, &width, &height);
  return width;
}

int SdlPlatform::pixel_height() const noexcept {
  int width = 0, height = 0;
  if (window_) SDL_GetWindowSizeInPixels(window_, &width, &height);
  return height;
}

float SdlPlatform::display_scale() const noexcept {
  return window_ ? std::max(SDL_GetWindowDisplayScale(window_), 0.5f) : 1.f;
}

int SdlPlatform::default_ui_scale_percent() const noexcept {
  if (!window_)
    return 100;
  // SDL's Windows backend reports window and mouse coordinates in physical
  // pixels for this borderless high-density window, while the OS and assistive
  // automation expose display-independent coordinates. Match the UI scale to
  // the monitor content scale so a 1440x900 logical shell remains 1440x900 on
  // screen instead of shrinking to 960x600 at 150% system scaling.
  const float percent = display_scale() * 100.f;
  const int quarter_step = static_cast<int>(std::lround(percent / 25.f)) * 25;
  return std::clamp(quarter_step, 100, 200);
}

void SdlPlatform::set_ui_scale(const float scale) noexcept {
  ui_scale_ = std::clamp(scale, 0.7f, 2.f);
}

void SdlPlatform::size_window_for_ui_scale(const int logical_width,
                                           const int logical_height) {
  if (!window_)
    return;
  SDL_SetWindowMinimumSize(
      window_, static_cast<int>(std::lround(712.f * ui_scale_)) + 16,
      static_cast<int>(std::lround(680.f * ui_scale_)));
  SDL_SetWindowSize(window_,
                    static_cast<int>(std::lround(logical_width * ui_scale_)),
                    static_cast<int>(std::lround(logical_height * ui_scale_)));
  SDL_SyncWindow(window_);
}

void SdlPlatform::minimize() { if (window_) SDL_MinimizeWindow(window_); }
void SdlPlatform::toggle_maximize() {
  if (!window_) return;
  if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window_);
  else SDL_MaximizeWindow(window_);
}
void SdlPlatform::begin_window_drag() {
  // Native dragging is supplied by SDL_SetWindowHitTest in the application
  // shell; this method remains as the controller-facing abstraction.
}

void SdlPlatform::set_raw_event_handler(
    std::function<bool(const SDL_Event&)> handler) {
  raw_event_handler_ = std::move(handler);
}

double SdlPlatform::GetElapsedTime() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void SdlPlatform::SetMouseCursor(const Rml::String& name) {
  SDL_Cursor* cursor = cursor_arrow_;
  if (name == "pointer") cursor = cursor_pointer_;
  else if (name == "text") cursor = cursor_text_;
  else if (name == "move" || name.rfind("rmlui-scroll", 0) == 0) cursor = cursor_move_;
  if (cursor) SDL_SetCursor(cursor);
}

void SdlPlatform::SetClipboardText(const Rml::String& text) {
  SDL_SetClipboardText(text.c_str());
}
void SdlPlatform::GetClipboardText(Rml::String& text) {
  char* value = SDL_GetClipboardText();
  text = value ? value : "";
  SDL_free(value);
}
void SdlPlatform::ActivateKeyboard(Rml::Vector2f position, float line_height) {
  const SDL_Rect area{static_cast<int>(position.x), static_cast<int>(position.y),
                      1, static_cast<int>(line_height)};
  SDL_SetTextInputArea(window_, &area, 0);
  SDL_StartTextInput(window_);
}
void SdlPlatform::DeactivateKeyboard() { if (window_) SDL_StopTextInput(window_); }

} // namespace tokmon::desk
