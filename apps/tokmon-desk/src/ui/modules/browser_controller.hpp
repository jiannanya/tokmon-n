#pragma once

#include "browser/browser_manager.hpp"

#include <filesystem>
#include <future>
#include <string>

namespace Rml { class ElementDocument; }

namespace tokmon::desk {

class DeskViewModel;

// Owns the external browser lifecycle and its presentation state. It has no
// dependency on DeskController or daemon/workspace orchestration.
class BrowserController {
public:
  BrowserController(std::filesystem::path data_root, DeskViewModel& view_model);
  ~BrowserController();

  void attach(Rml::ElementDocument& document);
  void launch();
  void refresh();
  void back();
  void forward();
  void reload();
  void stop();
  void click();
  void fill();
  void toggle_takeover();
  [[nodiscard]] bool update();
  [[nodiscard]] bool busy() const noexcept { return future_.valid(); }
  [[nodiscard]] bool takeover() const noexcept { return takeover_; }
  [[nodiscard]] std::filesystem::path discovered_executable() const;

private:
  void present(const BrowserSessionState& state);

  BrowserManager manager_;
  DeskViewModel& view_model_;
  Rml::ElementDocument* document_{nullptr};
  std::future<BrowserSessionState> future_;
  std::string session_{"tokmon-desk"};
  bool takeover_{false};
};

} // namespace tokmon::desk
