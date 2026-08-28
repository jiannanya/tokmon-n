#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "review_panel.hpp"
#include "tokmon.h"

namespace tokmon::desktop {

// Drives the right side panel: tab bookkeeping plus every Git and workspace
// tree interaction. Git subprocesses run on a detached worker thread so the UI
// never blocks on repository I/O; results are posted back through
// invoke_from_event_loop.
class RightPanelController {
public:
  explicit RightPanelController(
      slint::ComponentWeakHandle<MainWindow> window);
  ~RightPanelController();

  void set_workspace(std::filesystem::path workspace);
  // Reloads branches, Git status and the directory tree for the workspace.
  void refresh();
  void reset();

  // Tabs
  void open_tab(std::string id);
  void close_tab(std::string id);
  void add_tab();
  void toggle_maximized();

  // Git review
  void select_review_file(std::string id);
  void switch_branch(std::string branch);
  void toggle_hunk(std::string hunk);
  void stage_all();
  void discard_unstaged();
  void commit_and_push(std::string message, bool push);
  void reveal_in_explorer();

  // Workspace tree
  void select_tree_item(int index);

  // List filters. Slint cannot filter models inline, so the controller
  // republishes a filtered projection whenever the query changes.
  void set_review_filter(std::string query);
  void set_tree_filter(std::string query);

  // Shows the transient toast strip at the top of the panel.
  void toast(std::string message);

private:
  void publish_tabs();
  void publish_review();
  void publish_diff();
  void publish_tree();
  void run_async(std::function<void()> task);
  static float estimate_width(const std::vector<DiffLine> &lines);

  slint::ComponentWeakHandle<MainWindow> window_;
  std::filesystem::path workspace_;
  std::mutex mutex_;
  ReviewPanel panel_;
  std::vector<std::string> tabs_;
  std::string active_tab_;
  std::string selected_file_;
  std::string review_filter_;
  std::string tree_filter_;
  // Paths behind the currently published (filtered) tree model.
  std::vector<std::string> tree_view_paths_;
  std::optional<bool> restore_sidebar_;
  std::optional<float> restore_width_;
  bool shutting_down_{false};
};

} // namespace tokmon::desktop
