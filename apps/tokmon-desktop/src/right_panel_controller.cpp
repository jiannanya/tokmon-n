#include "right_panel_controller.hpp"

#include <algorithm>
#include <cctype>
#include <thread>

#include "platform_utils.hpp"

namespace tokmon::desktop {
namespace {

// Case-insensitive containment used by both list filters.
bool matches(const std::string &haystack, const std::string &needle) {
  if (needle.empty())
    return true;
  std::string lower_haystack;
  std::string lower_needle;
  lower_haystack.reserve(haystack.size());
  lower_needle.reserve(needle.size());
  for (const char character : haystack)
    lower_haystack.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  for (const char character : needle)
    lower_needle.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  return lower_haystack.find(lower_needle) != std::string::npos;
}

std::shared_ptr<slint::VectorModel<RightTab>>
make_tab_model(const std::vector<std::string> &tabs) {
  auto model = std::make_shared<slint::VectorModel<RightTab>>();
  for (const auto &id : tabs) {
    RightTab tab;
    tab.id = slint::SharedString(id);
    tab.title = slint::SharedString(id == "review" ? "审查" : "打开文件");
    model->push_back(std::move(tab));
  }
  return model;
}

std::shared_ptr<slint::VectorModel<slint::SharedString>>
make_string_model(const std::vector<std::string> &values) {
  auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
  for (const auto &value : values)
    model->push_back(slint::SharedString(value));
  return model;
}

} // namespace

RightPanelController::RightPanelController(
    slint::ComponentWeakHandle<MainWindow> window)
    : window_(std::move(window)) {}

RightPanelController::~RightPanelController() {
  std::scoped_lock lock(mutex_);
  shutting_down_ = true;
}

void RightPanelController::set_workspace(std::filesystem::path workspace) {
  std::scoped_lock lock(mutex_);
  if (workspace_ == workspace)
    return;
  workspace_ = std::move(workspace);
}

void RightPanelController::reset() {
  {
    std::scoped_lock lock(mutex_);
    tabs_.clear();
    active_tab_.clear();
    selected_file_.clear();
  }
  publish_tabs();
}

void RightPanelController::refresh() {
  run_async([this] {
    {
      std::scoped_lock lock(mutex_);
      panel_.set_workspace(workspace_);
      panel_.refresh();
      if (selected_file_.empty()) {
        const auto files = panel_.review_files();
        if (!files.empty())
          selected_file_ = std::string(files.front().id);
      }
    }
    // Every publish re-applies the active filters, so no separate model
    // assembly is needed here.
    publish_review();
    publish_tree();
    publish_diff();
  });
}

void RightPanelController::publish_tabs() {
  std::vector<std::string> tabs;
  std::string active;
  {
    std::scoped_lock lock(mutex_);
    tabs = tabs_;
    active = active_tab_.empty() ? "launcher" : active_tab_;
  }
  auto window = window_;
  auto model = make_tab_model(tabs);
  const auto active_value = slint::SharedString(active);
  (void)slint::invoke_from_event_loop([window, model, active_value] {
    if (auto locked = window.lock()) {
      (*locked)->set_right_tabs(model);
      (*locked)->set_right_tab(active_value);
    }
  });
}

void RightPanelController::publish_review() {
  std::vector<std::string> branches;
  std::vector<ReviewFile> files;
  std::string branch;
  std::string selected;
  std::string selected_path;
  int file_added = 0;
  int file_removed = 0;
  int total_added = 0;
  int total_removed = 0;
  std::string filter;
  {
    std::scoped_lock lock(mutex_);
    branches = panel_.branches();
    branch = panel_.branch();
    files = panel_.review_files();
    selected = selected_file_;
    filter = review_filter_;
    for (const auto &file : files) {
      total_added += file.additions;
      total_removed += file.deletions;
      if (std::string(file.id) != selected)
        continue;
      file_added = file.additions;
      file_removed = file.deletions;
      selected_path = std::string(file.path);
    }
  }
  const auto summary =
      files.empty() ? std::string("工作区没有待提交的更改")
                    : "工作区共 " + std::to_string(files.size()) +
                          " 个文件发生变更 (+" + std::to_string(total_added) +
                          " -" + std::to_string(total_removed) + ")";
  auto window = window_;
  auto branches_model = make_string_model(branches);
  auto files_model = std::make_shared<slint::VectorModel<ReviewFile>>();
  std::vector<ReviewFile> visible_files;
  for (auto &file : files) {
    if (!matches(std::string(file.path), filter) &&
        !matches(std::string(file.display_name), filter))
      continue;
    visible_files.push_back(std::move(file));
  }
  std::ranges::stable_sort(visible_files, {}, [](const ReviewFile &file) {
    return std::string(file.path);
  });
  std::string previous_folder;
  int review_list_height = 0;
  for (auto &file : visible_files) {
    const auto folder = std::string(file.display_folder);
    file.show_folder = folder != previous_folder;
    previous_folder = folder;
    if (review_list_height > 0)
      review_list_height += 4;
    review_list_height += file.show_folder ? 42 : 26;
    files_model->push_back(std::move(file));
  }
  (void)slint::invoke_from_event_loop(
      [window, branches_model, files_model, branch, selected, selected_path,
       file_added, file_removed, total_added, total_removed,
       review_list_height, summary] {
        if (auto locked = window.lock()) {
          auto handle = *locked;
          handle->set_branch_options(branches_model);
          handle->set_current_branch(slint::SharedString(branch));
          handle->set_review_files(files_model);
          handle->set_review_added(total_added);
          handle->set_review_removed(total_removed);
          handle->set_review_list_height(review_list_height);
          handle->set_selected_review_file_id(slint::SharedString(selected));
          handle->set_selected_review_path(slint::SharedString(selected_path));
          handle->set_selected_review_added(file_added);
          handle->set_selected_review_removed(file_removed);
          handle->set_staged_summary(slint::SharedString(summary));
        }
      });
}

void RightPanelController::publish_diff() {
  std::string selected;
  {
    std::scoped_lock lock(mutex_);
    selected = selected_file_;
  }
  if (selected.empty())
    return;
  run_async([this, selected] {
    std::vector<DiffLine> lines;
    {
      std::scoped_lock lock(mutex_);
      lines = panel_.diff_lines(selected);
    }
    const auto width = estimate_width(lines);
    auto window = window_;
    auto model = std::make_shared<slint::VectorModel<DiffLine>>();
    for (auto &line : lines)
      model->push_back(std::move(line));
    (void)slint::invoke_from_event_loop([window, model, width] {
      if (auto locked = window.lock()) {
        (*locked)->set_diff_lines(model);
        (*locked)->set_diff_content_width(width);
      }
    });
  });
}

void RightPanelController::publish_tree() {
  std::vector<TreeItem> items;
  std::string filter;
  std::filesystem::path workspace;
  {
    std::scoped_lock lock(mutex_);
    items = panel_.tree_items();
    filter = tree_filter_;
    workspace = workspace_;
  }
  // `tree_view_paths_` mirrors the filtered model so a later click resolves to
  // the entry the user actually saw.
  std::vector<std::string> visible_paths;
  auto window = window_;
  auto workspace_label = path_to_utf8(workspace.filename());
  if (workspace_label.empty())
    workspace_label = path_to_utf8(workspace);
  const auto workspace_label_value = slint::SharedString(workspace_label);
  auto model = std::make_shared<slint::VectorModel<TreeItem>>();
  for (auto &item : items) {
    const auto path = std::string(item.path);
    if (!matches(path, filter))
      continue;
    visible_paths.push_back(path);
    model->push_back(std::move(item));
  }
  {
    std::scoped_lock lock(mutex_);
    tree_view_paths_ = std::move(visible_paths);
  }
  (void)slint::invoke_from_event_loop([window, model, workspace_label_value] {
    if (auto locked = window.lock()) {
      (*locked)->set_tree_items(model);
      (*locked)->set_workspace_tree_label(workspace_label_value);
    }
  });
}

void RightPanelController::set_review_filter(std::string query) {
  {
    std::scoped_lock lock(mutex_);
    review_filter_ = std::move(query);
  }
  publish_review();
}

void RightPanelController::set_tree_filter(std::string query) {
  {
    std::scoped_lock lock(mutex_);
    tree_filter_ = std::move(query);
  }
  publish_tree();
}

void RightPanelController::open_tab(std::string id) {
  std::string tab = id;
  {
    std::scoped_lock lock(mutex_);
    if (std::ranges::find(tabs_, tab) == tabs_.end())
      tabs_.push_back(tab);
    active_tab_ = tab;
    if (tab == "review" && selected_file_.empty()) {
      const auto files = panel_.review_files();
      if (!files.empty())
        selected_file_ = std::string(files.front().id);
    }
  }
  publish_tabs();
  if (tab == "review") {
    publish_review();
    publish_diff();
  }
}

void RightPanelController::close_tab(std::string id) {
  {
    std::scoped_lock lock(mutex_);
    std::erase(tabs_, id);
    if (active_tab_ == id)
      active_tab_ = tabs_.empty() ? std::string{} : tabs_.back();
  }
  publish_tabs();
}

void RightPanelController::add_tab() {
  std::string opening;
  {
    std::scoped_lock lock(mutex_);
    if (std::ranges::find(tabs_, "review") == tabs_.end())
      opening = "review";
    else if (std::ranges::find(tabs_, "openFile") == tabs_.end())
      opening = "openFile";
  }
  if (opening.empty()) {
    toast("已打开全部标签页");
    return;
  }
  open_tab(opening);
}

void RightPanelController::toggle_maximized() {
  auto window = window_;
  (void)slint::invoke_from_event_loop([window, this] {
    if (auto locked = window.lock()) {
      auto handle = *locked;
      const bool next = !handle->get_right_panel_maximized();
      handle->set_right_panel_maximized(next);
      if (next) {
        restore_sidebar_ = handle->get_sidebar_visible();
        restore_width_ = handle->get_code_panel_width();
        handle->set_sidebar_visible(false);
        handle->set_code_panel_width(720.0f);
      } else {
        if (restore_sidebar_.has_value())
          handle->set_sidebar_visible(*restore_sidebar_);
        if (restore_width_.has_value())
          handle->set_code_panel_width(*restore_width_);
        restore_sidebar_.reset();
        restore_width_.reset();
      }
    }
  });
}

void RightPanelController::select_review_file(std::string id) {
  {
    std::scoped_lock lock(mutex_);
    selected_file_ = std::move(id);
  }
  publish_review();
  publish_diff();
}

void RightPanelController::toggle_hunk(std::string hunk) {
  std::string selected;
  {
    std::scoped_lock lock(mutex_);
    selected = selected_file_;
  }
  if (selected.empty() || hunk.empty())
    return;
  run_async([this, selected, hunk] {
    {
      std::scoped_lock lock(mutex_);
      panel_.toggle_hunk(selected, hunk);
    }
    publish_diff();
  });
}

void RightPanelController::switch_branch(std::string branch) {
  run_async([this, branch] {
    bool ok = false;
    std::string error;
    {
      std::scoped_lock lock(mutex_);
      ok = panel_.checkout(branch);
      if (!ok)
        error = panel_.last_error();
    }
    if (ok) {
      refresh();
      toast("✓ 已切换至分支: " + branch);
    } else {
      toast("切换分支失败：" +
            (error.empty() ? "git checkout 返回非零状态" : error));
    }
  });
}

void RightPanelController::stage_all() {
  run_async([this] {
    bool ok = false;
    std::string error;
    {
      std::scoped_lock lock(mutex_);
      ok = panel_.stage_all();
      if (!ok)
        error = panel_.last_error();
    }
    if (ok) {
      refresh();
      toast("✓ 已暂存全部更改");
    } else {
      toast("暂存失败：" + (error.empty() ? "git add 返回非零状态" : error));
    }
  });
}

void RightPanelController::discard_unstaged() {
  run_async([this] {
    bool ok = false;
    std::string error;
    {
      std::scoped_lock lock(mutex_);
      ok = panel_.discard_unstaged();
      if (!ok)
        error = panel_.last_error();
    }
    if (ok) {
      refresh();
      toast("✓ 已放弃未暂存更改");
    } else {
      toast("放弃失败：" +
            (error.empty() ? "git restore 返回非零状态" : error));
    }
  });
}

void RightPanelController::commit_and_push(std::string message, bool push) {
  run_async([this, message, push] {
    bool ok = false;
    std::string error;
    {
      std::scoped_lock lock(mutex_);
      ok = panel_.stage_all() && panel_.commit(message);
      if (!ok)
        error = panel_.last_error();
      if (ok && push) {
        ok = panel_.push();
        if (!ok)
          error = panel_.last_error();
      }
    }
    if (ok) {
      refresh();
      toast(push ? "✓ 已成功提交并推送到远程仓库！"
                 : "✓ 已成功提交到本地仓库！");
    } else {
      toast("提交失败：" + (error.empty() ? "git commit 返回非零状态" : error));
    }
  });
}

void RightPanelController::reveal_in_explorer() {
  std::filesystem::path target;
  {
    std::scoped_lock lock(mutex_);
    std::string selected_path;
    for (const auto &file : panel_.review_files()) {
      if (std::string(file.id) != selected_file_)
        continue;
      selected_path = std::string(file.path);
      break;
    }
    target = selected_path.empty() ? workspace_ : workspace_ / selected_path;
  }
  const auto ok = reveal_path_in_file_manager(target);
  toast(ok ? "已在文件管理器中定位" : "无法在文件管理器中定位该文件");
}

void RightPanelController::select_tree_item(int index) {
  std::string path;
  std::string kind;
  {
    std::scoped_lock lock(mutex_);
    if (index < 0 || static_cast<std::size_t>(index) >= tree_view_paths_.size())
      return;
    path = tree_view_paths_[static_cast<std::size_t>(index)];
    for (const auto &entry : panel_.tree_items()) {
      if (std::string(entry.path) != path)
        continue;
      kind = std::string(entry.kind);
      break;
    }
  }
  if (path.empty())
    return;
  if (kind == "folder") {
    run_async([this, path] {
      {
        std::scoped_lock lock(mutex_);
        panel_.toggle_tree_folder(path);
      }
      publish_tree();
    });
    return;
  }
  run_async([this, path] {
    std::string name;
    std::string content;
    bool ok = false;
    {
      std::scoped_lock lock(mutex_);
      ok = panel_.read_tree_file(path, &name, &content);
    }
    auto window = window_;
    const auto shown =
        ok ? content : std::string("（无法预览该文件的文本内容）");
    const auto title = ok ? name : path;
    (void)slint::invoke_from_event_loop([window, title, shown, path] {
      if (auto locked = window.lock()) {
        (*locked)->set_tree_file_name(slint::SharedString(title));
        (*locked)->set_tree_file_content(slint::SharedString(shown));
        (*locked)->set_tree_selected_path(slint::SharedString(path));
      }
    });
  });
}

void RightPanelController::toast(std::string message) {
  auto window = window_;
  (void)slint::invoke_from_event_loop([window, message] {
    if (auto locked = window.lock()) {
      (*locked)->set_toast_text(slint::SharedString(message));
      (*locked)->set_toast_visible(true);
    }
  });
}

void RightPanelController::run_async(std::function<void()> task) {
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_)
      return;
  }
  std::thread([this, task = std::move(task)] {
    try {
      task();
    } catch (...) {
      // A failed Git probe must never take the UI thread down with it.
    }
  }).detach();
}

float RightPanelController::estimate_width(const std::vector<DiffLine> &lines) {
  float widest = 0.0f;
  for (const auto &line : lines) {
    float width = 0.0f;
    for (const char character : std::string_view(line.text)) {
      // CJK glyphs are roughly twice the width of a mono Latin character.
      width += (static_cast<unsigned char>(character) & 0x80) != 0 ? 12.0f
                                                                   : 6.7f;
    }
    widest = std::max(widest, width);
  }
  // Gutter, padding and a little slack for the widest row.
  return widest + 120.0f;
}

} // namespace tokmon::desktop
