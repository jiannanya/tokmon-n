#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

namespace tokmon::desktop {

class RightPanelController;

class UiController {
public:
  virtual ~UiController() = default;

  virtual void backend_connected() = 0;
  virtual void chat(std::string text, std::string name, std::string model,
                    std::string access_mode, std::string effort) = 0;
  // User pressed the composer stop button: leave the generating presentation
  // immediately and keep whatever content was already produced.
  virtual void interrupt_generation() = 0;
  virtual void slash_command(std::string text, std::string name,
                             std::string model, std::string access_mode,
                             std::string effort) = 0;
  virtual void rename_session(std::string title) = 0;
  virtual void reconcile() = 0;
  virtual void refresh_workspace() = 0;
  virtual void new_session(std::string workspace = {}) = 0;
  virtual void open_session(std::string ray, std::string workspace = {}) = 0;
  virtual void switch_workspace(std::string workspace) = 0;
  virtual void load_settings(const bool include_navigation = false) = 0;
  virtual void load_providers() = 0;
  virtual void save_navigation() = 0;
  virtual void save_settings(tokmon::cbor::Value values) = 0;
  virtual void configure_provider(tokmon::cbor::Value values) = 0;
  virtual void select_provider(std::string name) = 0;
  virtual void store_provider_secret(std::string name, std::string secret) = 0;
  virtual void test_provider(std::string name) = 0;
  virtual void publish_trace_view() = 0;
  virtual void export_trace() = 0;
  [[nodiscard]] virtual const std::filesystem::path &
  current_workspace() const noexcept = 0;
  virtual void select_session_file(const int index) = 0;
  virtual void filter_preview_lines(std::string query) = 0;
  virtual void notify_copied() = 0;
};

std::unique_ptr<UiController> make_ui_controller(
    std::filesystem::path endpoint, std::filesystem::path workspace,
    std::filesystem::path daemon_executable,
    std::shared_ptr<slint::VectorModel<TimelineItem>> timeline,
    std::shared_ptr<slint::VectorModel<TimelineItem>> conversation_workflow,
    std::shared_ptr<slint::VectorModel<ChatBlock>> assistant_blocks,
    std::shared_ptr<slint::VectorModel<CodeLine>> code,
    std::shared_ptr<slint::VectorModel<TraceEvent>> trace_events,
    std::shared_ptr<slint::VectorModel<GanttSegment>> gantt,
    std::shared_ptr<slint::VectorModel<NavigationItem>> navigation_model,
    std::shared_ptr<std::vector<NavigationItem>> navigation,
    std::filesystem::path assets, slint::ComponentWeakHandle<MainWindow> window,
    std::shared_ptr<RightPanelController> right_panel,
    bool restore_initial_workspace);

} // namespace tokmon::desktop
