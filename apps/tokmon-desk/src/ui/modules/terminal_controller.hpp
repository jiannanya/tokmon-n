#pragma once

#include "terminal/terminal_service.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rml { class ElementDocument; class Event; }
union SDL_Event;

namespace tokmon::desk {

class DeskViewModel;
class SettingsController;
class SdlPlatform;

// Owns PTY sessions, Ghostty VT state, tab state, and terminal input routing.
// The shell only decides when terminal focus is active.
class TerminalController {
public:
  TerminalController(SdlPlatform& platform, DeskViewModel& view_model,
                     SettingsController& settings,
                     std::filesystem::path workspace);
  ~TerminalController();

  void attach(Rml::ElementDocument& document);
  void set_workspace(std::filesystem::path workspace);
  void start();
  [[nodiscard]] bool resize();
  void paste(bool allow_unsafe);
  void cancel_paste();
  void search();
  void create_tab();
  void close_tab();
  void select_tab(std::string_view id);
  void clear_search();
  void release_focus();

  [[nodiscard]] bool handle_pointer(Rml::Event& event);
  [[nodiscard]] bool handle_wheel(const SDL_Event& event);
  [[nodiscard]] bool handle_text(const SDL_Event& event);
  [[nodiscard]] bool handle_key(const SDL_Event& event);
  [[nodiscard]] bool update();
  [[nodiscard]] bool running() const noexcept;

private:
  struct Tab {
    std::string id;
    std::string title;
    std::unique_ptr<TerminalSession> session;
    std::unique_ptr<GhosttyVt> vt;
    TerminalLaunchOptions launch;
    std::string launch_error;
    bool started{false};
    int columns{100};
    int rows{28};
    int font_size{13};
    int cell_width{8};
    int cell_height{17};
  };

  [[nodiscard]] Tab& active_tab();
  [[nodiscard]] TerminalSession& session();
  [[nodiscard]] GhosttyVt& vt();
  [[nodiscard]] int cell_width_pixels(const Tab& tab) const;
  [[nodiscard]] int cell_height_pixels(const Tab& tab) const;
  void render_tabs();
  void set_status(std::string value);
  void set_hint(std::string value);

  SdlPlatform& platform_;
  DeskViewModel& view_model_;
  SettingsController& settings_;
  Rml::ElementDocument* document_{nullptr};
  std::filesystem::path workspace_;
  std::vector<std::unique_ptr<Tab>> tabs_;
  std::size_t active_index_{0};
  std::uint64_t next_id_{1};
  std::string pending_keydown_text_;
  std::string pending_paste_;
  bool mouse_down_{false};
};

} // namespace tokmon::desk
