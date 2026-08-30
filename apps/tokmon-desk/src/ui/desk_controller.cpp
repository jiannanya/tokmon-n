#include "ui/desk_controller.hpp"

#include "platform/sdl_platform.hpp"
#include "ui/elements/element_code_surface.hpp"
#include "ui/elements/element_diff_surface.hpp"
#include "ui/elements/element_file_tree.hpp"
#include "ui/elements/element_terminal.hpp"

#include "tokmon/surface.hpp"
#include "tokmon/config.hpp"
#include "tokmon/ids.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Input.h>

#include <chrono>
#include <charconv>
#include <array>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <iostream>
#include <limits>

namespace tokmon::desk {
namespace {

constexpr float legacy_frame_inset = 6.4f;
constexpr int legacy_launcher_width = 219;

constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
    slash_commands{{
        {"/review", "审查当前更改"},
        {"/fix", "诊断并修复问题"},
        {"/explore", "探索工作区"},
        {"/build", "构建新功能"},
    }};

Rml::ElementFormControl* control(Rml::ElementDocument& document, const char* id) {
  return dynamic_cast<Rml::ElementFormControl*>(document.GetElementById(id));
}

void text(Rml::ElementDocument& document, const char* id, const std::string& value) {
  if (auto* element = document.GetElementById(id))
    element->SetInnerRML(value);
}

std::string cbor_string(const tokmon::cbor::Value& values,
                        const std::string_view key,
                        const std::string_view fallback = {}) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::string>(value->data)
      ? std::string(value->as_string()) : std::string(fallback);
}

std::int64_t cbor_integer(const tokmon::cbor::Value& values,
                          const std::string_view key,
                          const std::int64_t fallback) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::int64_t>(value->data)
      ? value->as_integer() : fallback;
}

tokmon::cbor::Value select_cbor_keys(
    const tokmon::cbor::Value& source,
    const std::initializer_list<std::string_view> keys) {
  tokmon::cbor::Value::Map result;
  for (const auto key : keys)
    if (const auto* value = tokmon::cbor::find(source, key))
      result.emplace(std::string(key), *value);
  return result;
}

std::vector<DeskNavigationItem> legacy_navigation_seed(
    const std::filesystem::path& workspace, const bool select_first_session) {
  std::vector<DeskNavigationItem> items;
  const auto add = [&items, &workspace](const char* title, const char* kind,
                                        const int indent, const bool selected) {
    items.push_back({
        .id = tokmon::make_id("navigation"),
        .workspace = std::string_view(kind) == "project" ? workspace
                                                          : std::filesystem::path{},
        .kind = kind,
        .title = title,
        .indent = indent,
        .selected = selected,
        .expanded = true,
        .title_manual = std::string_view(kind) == "session",
    });
  };
  // This is the exact bundled hierarchy from the old Slint desktop. It is
  // presentation/sample state only; existing tokmon-desk projects and rays
  // are retained after it during the one-time migration.
  add("内容生产", "group", 0, false);
  add("字幕制作空间", "project", 1, false);
  add("生成音频时间轴字幕", "session", 2, select_first_session);
  add("字幕校对优化", "session", 2, false);
  add("批量字幕质检优化", "session", 2, false);
  add("音频切片处理", "project", 1, false);
  add("演示助手", "group", 0, false);
  add("PPT 智绘项目", "project", 1, false);
  add("PPT 大纲生成", "session", 2, false);
  add("演讲稿润色", "session", 2, false);
  add("旅行计划", "group", 0, false);
  return items;
}

bool same_workspace(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
  std::error_code left_error, right_error;
  const auto normalized_left = std::filesystem::weakly_canonical(left,
                                                                  left_error);
  const auto normalized_right = std::filesystem::weakly_canonical(right,
                                                                   right_error);
  return !left_error && !right_error && normalized_left == normalized_right;
}

std::string automatic_title(std::string_view prompt) {
  while (!prompt.empty() && std::isspace(
             static_cast<unsigned char>(prompt.front())))
    prompt.remove_prefix(1);
  auto end = prompt.find_first_of("\r\n");
  if (end == std::string_view::npos)
    end = prompt.size();
  end = std::min<std::size_t>(end, 32);
  std::string title(prompt.substr(0, end));
  if (prompt.size() > end)
    title += "…";
  return title.empty() ? "新会话" : title;
}

std::vector<tokmon::Photon> response_photons(
    const tokmon::cbor::Value& payload, const std::string& active_ray) {
  std::vector<tokmon::Photon> result;
  if (const auto* encoded = tokmon::cbor::find(payload, "photons");
      encoded && encoded->as_array()) {
    for (const auto& item : *encoded->as_array()) {
      auto photon = tokmon::photon_from_cbor(item);
      if (photon)
        result.push_back(std::move(*photon));
    }
  }
  const auto* encoded_surface = tokmon::cbor::find(payload, "surface");
  if (!encoded_surface)
    return result;
  auto surface = tokmon::surface_from_cbor(*encoded_surface);
  if (!surface)
    return result;
  const tokmon::SurfaceContribution* selected = nullptr;
  for (const auto& contribution : surface->contributions) {
    if (contribution.channel != "ui.trajectory" ||
        !contribution.value.as_array())
      continue;
    if (!selected || contribution.priority > selected->priority)
      selected = &contribution;
  }
  if (!selected)
    return result;
  for (const auto& item : *selected->value.as_array()) {
    tokmon::Photon photon;
    if (const auto* value = tokmon::cbor::find(item, "sequence"))
      photon.sequence = static_cast<std::uint64_t>(value->as_integer());
    if (const auto* value = tokmon::cbor::find(item, "id"))
      photon.id = std::string(value->as_string());
    photon.ray = active_ray;
    if (const auto* value = tokmon::cbor::find(item, "kind"))
      photon.kind = std::string(value->as_string());
    if (const auto* value = tokmon::cbor::find(item, "schema"))
      photon.schema = std::string(value->as_string());
    if (const auto* value = tokmon::cbor::find(item, "payload"))
      photon.payload = *value;
    if (const auto* value = tokmon::cbor::find(item, "time"))
      photon.committed_at_ms = value->as_integer();
    if (const auto* value = tokmon::cbor::find(item, "caused_by_act"))
      photon.caused_by_act = std::string(value->as_string());
    if (!photon.id.empty() && !photon.kind.empty())
      result.push_back(std::move(photon));
  }
  return result;
}

} // namespace

DeskController::DeskController(Rml::ElementDocument& document, SdlPlatform& platform,
                               DeskViewModel& view_model,
                               std::filesystem::path workspace,
                               DeskAppPaths app_paths,
                               std::filesystem::path daemon_endpoint)
    : document_(document), platform_(platform), view_model_(view_model),
      workspace_(workspace),
      watcher_(workspace), git_(workspace),
      change_tracker_(workspace, app_paths.change_snapshots),
      browser_(app_paths.data, view_model),
      state_store_(app_paths),
      settings_(view_model, browser_, platform.default_content_scale_percent()),
      renderer_(view_model),
      trajectory_(view_model),
      terminal_(platform, view_model, settings_, workspace),
      recovery_store_(app_paths.recovery),
      daemon_(std::move(daemon_endpoint)), navigation_(workspace) {}

DeskController::~DeskController() {
  platform_.set_raw_event_handler({});
  if (file_load_future_.valid())
    file_load_future_.wait();
  if (syntax_future_.valid())
    syntax_future_.wait();
  if (change_set_future_.valid())
    change_set_future_.wait();
  if (recovery_future_.valid())
    recovery_future_.wait();
  if (pending_recovery_snapshot_) {
    std::string ignored;
    (void)recovery_store_.save(*pending_recovery_snapshot_,
                               pending_recovery_workspace_, ignored);
  }
}

void DeskController::listen(const char* id, const char* event) {
  if (auto* element = document_.GetElementById(id))
    element->AddEventListener(event, this);
}

void DeskController::bind(const bool start_background_work) {
  browser_.attach(document_);
  terminal_.attach(document_);
  for (const char* id : {"new-session-button", "settings-button", "close-settings",
                         "save-settings", "reset-settings", "close-new-session",
                         "cancel-new-session", "confirm-new-session", "environment-toggle",
                         "environment-close", "environment-refresh", "environment-settings",
                         "thought-toggle", "workflow-toggle", "sidebar-toggle", "right-toggle",
                         "sidebar-restore", "right-restore",
                         "review-tab", "files-tab", "terminal-tab", "browser-tab",
                         "refresh-review", "diff-view-toggle", "branch-button",
                         "accept-agent-changes", "reject-agent-changes",
                         "commit-button", "close-commit", "cancel-commit",
                         "confirm-commit", "confirm-push", "send-button", "browser-launch",
                         "browser-go", "browser-refresh", "browser-stop",
                         "browser-back", "browser-forward", "browser-reload",
                         "browser-takeover", "browser-click", "browser-fill",
                         "title-edit", "chat-mode", "trajectory-mode", "attach-button",
                         "access-button", "active-model", "effort-button",
                         "right-fullscreen", "right-collapse", "add-tab-button",
                         "right-tab-close", "launcher-review", "launcher-files",
                         "save-file", "undo-file", "redo-file", "reload-file",
                         "editor-find-previous", "editor-find-next",
                         "editor-replace-one", "editor-go-line",
                         "editor-match-bracket",
                         "file-new", "folder-new", "file-rename", "file-delete",
                         "close-file-operation", "cancel-file-operation",
                         "confirm-file-operation",
                         "terminal-new-tab", "terminal-close-tab",
                         "terminal-clear-search",
                         "navigation-context-new", "navigation-context-rename",
                         "navigation-context-delete",
                         "open-notices",
                         "close-discard", "cancel-discard", "confirm-discard",
                         "cancel-terminal-paste", "confirm-terminal-paste",
                         "minimize-button", "maximize-button", "close-button"})
    listen(id);
  for (const char* id : {"settings-overlay", "new-session-overlay",
                         "commit-overlay", "discard-overlay",
                         "terminal-paste-overlay", "file-operation-overlay"})
    listen(id);
  listen("composer", "keydown");
  listen("composer", "input");
  listen("composer", "change");
  listen("editor-find", "keydown");
  listen("editor-replace", "keydown");
  listen("editor-line", "keydown");
  listen("editor-find");
  listen("editor-replace");
  listen("editor-line");
  listen("conversation", "mousescroll");
  listen("conversation", "click");
  listen("file-preview");
  listen("file-preview", "mousescroll");
  listen("file-tree");
  listen("file-tree", "mousescroll");
  listen("file-tree", "keydown");
  listen("terminal-surface");
  listen("terminal-surface", "mousedown");
  listen("terminal-surface", "mousemove");
  listen("terminal-surface", "mouseup");
  listen("file-search", "input");
  listen("file-search", "change");
  listen("file-search", "keyup");
  listen("navigation-search", "input");
  listen("navigation-search", "change");
  listen("navigation-search", "keyup");
  listen("navigation-tree", "keydown");
  listen("navigation-tree", "mousedown");
  listen("settings-search", "input");
  listen("settings-search", "change");
  listen("settings-search", "keyup");
  listen("terminal-search", "input");
  listen("terminal-search", "change");
  listen("terminal-search", "keyup");
  listen("terminal-search");
  listen("trajectory-search", "input");
  listen("trajectory-search", "change");
  listen("trajectory-search", "keyup");
  listen("trajectory-lane-track", "mousedown");
  listen("trajectory-lane-track", "mousemove");
  listen("trajectory-lane-track", "mouseup");
  listen("trajectory-lane-track", "mousescroll");
  listen("trajectory-lane-track", "keydown");
  for (const char* id : {"navigation-tree", "review-empty", "review-diff",
                         "branch-menu", "terminal-tabs", "settings-body",
                         "composer-popover", "trajectory"})
    listen(id);
  Rml::ElementList settings_navigation;
  document_.QuerySelectorAll(settings_navigation, "[setting-page]");
  for (auto* item : settings_navigation)
    item->AddEventListener("click", this);
  Rml::ElementList starter_cards;
  document_.QuerySelectorAll(starter_cards, "[starter-kind]");
  for (auto* item : starter_cards)
    item->AddEventListener("click", this);
  std::string local_warning;
  const auto stored_settings = state_store_.load_settings(local_warning);
  settings_.load(stored_settings);
  const bool migrate_legacy_shell =
      cbor_integer(stored_settings, "layout_revision", 0) < 3;
  const bool migrate_legacy_navigation =
      cbor_integer(stored_settings, "navigation_revision", 0) < 2;
  const auto current_workspace = workspace_.root().generic_string();
  const auto* stored_workspace =
      tokmon::cbor::find(settings_.values(), "last_workspace");
  const bool remember_workspace =
      !stored_workspace || stored_workspace->as_string() != current_workspace;
  if (remember_workspace)
    settings_.set("last_workspace", current_workspace);
  sidebar_width_ = static_cast<int>(std::clamp<std::int64_t>(
      settings_.integer("sidebar_width", 240), 196, 420));
  right_panel_width_ = static_cast<int>(std::clamp<std::int64_t>(
      migrate_legacy_shell
          ? legacy_launcher_width
          : settings_.integer("right_panel_width", legacy_launcher_width),
      214, 720));
  if (migrate_legacy_shell || remember_workspace) {
    settings_.set("right_panel_width",
                  static_cast<std::int64_t>(right_panel_width_));
    settings_.set("layout_revision", static_cast<std::int64_t>(3));
  }
  if (migrate_legacy_navigation)
    settings_.set("navigation_revision", static_cast<std::int64_t>(2));
  sidebar_visible_ = settings_.boolean("sidebar_visible", true);
  // The frozen legacy screenshot and the prototype both show the panel-open
  // launcher state: no feature tab is selected until the user picks one.
  right_panel_visible_ = true;
  active_right_view_ = "launcher";
  settings_.set("right_panel_visible", true);
  apply_panel_layout();
  show_right_launcher();
  std::string navigation_warning;
  const auto local_navigation = state_store_.load_navigation(navigation_warning);
  if (local_navigation.as_array() && !local_navigation.as_array()->empty()) {
    std::string navigation_error;
    if (!navigation_.load(local_navigation, navigation_error))
      navigation_warning = "本地导航状态无效：" + navigation_error;
  }
  bool seeded_legacy_navigation = false;
  if (migrate_legacy_navigation) {
    const bool has_group = std::ranges::any_of(
        navigation_.items(), [](const DeskNavigationItem& item) {
          return item.kind == "group";
        });
    if (!has_group && navigation_.items().size() <= 4) {
      auto existing = std::move(navigation_.items());
      auto seeded = legacy_navigation_seed(workspace_.root(), existing.empty());
      if (!existing.empty()) {
        for (auto& item : seeded)
          item.selected = false;
      }
      seeded.insert(seeded.end(), std::make_move_iterator(existing.begin()),
                    std::make_move_iterator(existing.end()));
      navigation_.items() = std::move(seeded);
      seeded_legacy_navigation = true;
      if (!state_store_.save_navigation(navigation_.encode(), navigation_warning))
        navigation_warning = "旧版导航示例迁移失败：" + navigation_warning;
    }
  }
  if (migrate_legacy_shell || migrate_legacy_navigation || remember_workspace) {
    std::string migration_error;
    if (!save_local_settings(migration_error) && local_warning.empty())
      local_warning = migration_error;
  }
  if (!local_warning.empty())
    settings_.set_status(local_warning);
  navigation_loaded_ = true;
  if (!navigation_warning.empty())
    text(document_, "daemon-status", escape(navigation_warning));

  const auto workspace_name = workspace_.root().filename().string();
  auto& shell_view = view_model_.state();
  shell_view.workspace_name = workspace_name;
  shell_view.workspace_path = workspace_.root().generic_string();
  shell_view.settings.workspace_path = shell_view.workspace_path;
  shell_view.branch = "正在读取 Git…";
  view_model_.dirty();
  text(document_, "environment-index",
       seeded_legacy_navigation ? "已恢复旧版导航" : "正在读取…");
  auto* selected_navigation = navigation_.selected();
  if (navigation_.items().empty() || !selected_navigation ||
      !same_workspace(navigation_.selected_workspace(), workspace_.root())) {
    (void)navigation_.ensure_workspace_project(workspace_.root());
    selected_navigation = &navigation_.create_session("新会话");
  } else if (selected_navigation->kind == "project") {
    selected_navigation = &navigation_.create_session("新会话");
  }
  if (selected_navigation) {
    active_ray_ = selected_navigation->ray;
    view_model_.state().session_title = selected_navigation->title;
  }
  render_navigation();
  if (start_background_work) {
    refresh_review();
    refresh_files();
  }
  platform_.set_raw_event_handler(
      [this](const SDL_Event& event) { return handle_raw_event(event); });
  update_composer_placeholder();
}

void DeskController::backend_connected() {
  std::cerr << "tokmon-desk: controller backend connected\n";
  backend_ready_.store(true, std::memory_order_release);
}

void DeskController::prepare_legacy_three_pane_contract(
    const bool expanded_feature_panel) {
  sidebar_width_ = 240;
  right_panel_width_ = expanded_feature_panel ? 440 : legacy_launcher_width;
  sidebar_visible_ = true;
  right_panel_visible_ = true;
  apply_panel_layout();
  if (!expanded_feature_panel)
    show_right_launcher();
}

void DeskController::toggle_hidden(const char* id) {
  if (auto* element = document_.GetElementById(id))
    element->SetClass("hidden", !element->IsClassSet("hidden"));
}

void DeskController::apply_panel_layout() {
  const auto density_units = [](const float value) {
    return std::to_string(value) + "dp";
  };
  if (auto* shell = document_.GetElementById("app-shell")) {
    shell->SetClass("sidebar-hidden", !sidebar_visible_);
    shell->SetClass("right-hidden", !right_panel_visible_);
  }
  if (auto* sidebar = document_.GetElementById("sidebar")) {
    sidebar->SetClass("hidden", !sidebar_visible_);
    sidebar->SetProperty("width",
                         density_units(static_cast<float>(sidebar_width_)));
  }
  if (auto* panel = document_.GetElementById("right-panel")) {
    panel->SetClass("hidden", !right_panel_visible_);
    panel->SetClass("compact", right_panel_width_ < 408);
    panel->SetClass("narrow", right_panel_width_ < 300);
    panel->SetProperty("width", density_units(
        static_cast<float>(right_panel_width_) + 0.5f));
  }
  if (auto* workspace = document_.GetElementById("workspace")) {
    workspace->SetProperty("left", density_units(sidebar_visible_
        ? static_cast<float>(sidebar_width_) + 0.5f : 0.f));
    workspace->SetProperty("right", density_units(right_panel_visible_
        ? static_cast<float>(right_panel_width_) + 0.5f : 0.f));
  }
  if (auto* toggle = document_.GetElementById("right-toggle"))
    toggle->SetProperty("right", density_units(
        static_cast<float>(right_panel_width_) + 8.f));
  if (auto* divider = document_.GetElementById("sidebar-resizer"))
    divider->SetProperty("left", density_units(
        static_cast<float>(std::max(0, sidebar_width_ - 5))));
  if (auto* divider = document_.GetElementById("right-resizer"))
    divider->SetProperty("right", density_units(
        static_cast<float>(std::max(0, right_panel_width_ - 4))));
}

void DeskController::set_sidebar_visible(const bool visible) {
  sidebar_visible_ = visible;
  settings_.set("sidebar_visible", visible);
  apply_panel_layout();
}

void DeskController::set_right_panel_visible(const bool visible) {
  right_panel_visible_ = visible;
  settings_.set("right_panel_visible", visible);
  apply_panel_layout();
}

bool DeskController::save_local_settings(std::string& error) {
  settings_.set("sidebar_visible", sidebar_visible_);
  settings_.set("right_panel_visible", right_panel_visible_);
  settings_.set("sidebar_width", static_cast<std::int64_t>(sidebar_width_));
  settings_.set("right_panel_width",
                static_cast<std::int64_t>(right_panel_width_));
  return state_store_.save_settings(select_cbor_keys(settings_.values(), {
      "language", "startup", "autosave", "update_channel", "index_mode",
      "workspace_sync", "git", "notifications", "desktop_notifications",
      "message_alerts", "quiet_hours", "density", "font_scale", "ui_scale",
      "theme_mode", "nickname", "email", "cloud_sync", "sidebar_visible",
      "right_panel_visible", "sidebar_width", "right_panel_width",
      "layout_revision", "navigation_revision", "last_workspace",
      "default_agent", "agent_autonomous", "agent_show_thoughts",
      "agent_code_enabled", "agent_architect_enabled",
      "agent_translator_enabled", "agent_analyst_enabled",
      "skills_enabled", "skills_auto_invoke", "skill_customizations_enabled",
      "skill_generative_ui_enabled", "skill_refactor_enabled",
      "skill_diagrams_enabled", "rules_enabled", "prefer_project_rules",
      "global_rules", "mcp_auto_start", "mcp_approval", "mcp_timeout",
      "browser_high_risk_confirmation",
      "terminal_profile", "terminal_executable", "terminal_arguments",
      "terminal_font_size", "terminal_scrollback"}), error);
}

void DeskController::show_toast(std::string message) {
  if (auto* toast = document_.GetElementById("right-toast")) {
    toast->SetInnerRML("✓ " + escape(message));
    toast->SetClass("hidden", false);
    toast_until_ = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(2500);
  }
}

bool DeskController::dismiss_transient_ui() {
  if (view_model_.state().navigation_context_visible) {
    close_navigation_context();
    return true;
  }
  for (const char* id : {"settings-overlay", "new-session-overlay",
                         "commit-overlay", "discard-overlay",
                         "terminal-paste-overlay", "file-operation-overlay",
                         "right-tab-menu", "branch-menu"}) {
    if (auto* element = document_.GetElementById(id);
        element && !element->IsClassSet("hidden")) {
      element->SetClass("hidden", true);
      if (std::string_view(id) == "settings-overlay")
        if (auto* button = document_.GetElementById("settings-button"))
          button->Focus();
      return true;
    }
  }
  if (view_model_.state().composer_popover_visible) {
    renderer_.close_composer_popover();
    if (auto* composer = document_.GetElementById("composer"))
      composer->Focus();
    return true;
  }
  return false;
}

void DeskController::show_right_launcher() {
  active_right_view_ = "launcher";
  // Expanded feature surfaces are temporary. Closing their tab restores the
  // compact utility rail used by the legacy shell.
  if (right_panel_width_ != legacy_launcher_width) {
    right_panel_width_ = legacy_launcher_width;
    settings_.set("right_panel_width",
                  static_cast<std::int64_t>(legacy_launcher_width));
    apply_panel_layout();
    std::string ignored;
    (void)save_local_settings(ignored);
  }
  if (auto* launcher = document_.GetElementById("right-launcher"))
    launcher->SetClass("hidden", false);
  for (const char* view : {"review-view", "files-view", "terminal-view", "browser-view"})
    if (auto* element = document_.GetElementById(view))
      element->SetClass("hidden", true);
  if (auto* tab = document_.GetElementById("right-active-tab"))
    tab->SetClass("hidden", true);
  if (auto* menu = document_.GetElementById("right-tab-menu"))
    menu->SetClass("hidden", true);
}

void DeskController::show_right_view(const char* id) {
  if (std::string_view(id) != "terminal-view" &&
      heavy_focus_ != HeavyFocus::none) {
    heavy_focus_ = HeavyFocus::none;
    terminal_.release_focus();
  }
  set_right_panel_visible(true);
  active_right_view_ = id;
  if (auto* launcher = document_.GetElementById("right-launcher"))
    launcher->SetClass("hidden", true);
  if (auto* tab = document_.GetElementById("right-active-tab"))
    tab->SetClass("hidden", false);
  if (auto* menu = document_.GetElementById("right-tab-menu"))
    menu->SetClass("hidden", true);
  for (const char* view : {"review-view", "files-view", "terminal-view", "browser-view"})
    if (auto* element = document_.GetElementById(view))
      element->SetClass("hidden", std::string_view(view) != id);
  const std::pair<const char*, const char*> labels[] = {
      {"review-view", "审查"}, {"files-view", "文件"},
      {"terminal-view", "终端"}, {"browser-view", "浏览器"}};
  for (const auto& [view, label] : labels)
    if (std::string_view(view) == id)
      text(document_, "active-right-label", label);
  if (auto* icon = document_.GetElementById("active-right-icon")) {
    const auto source = view_model_.state().asset_root +
        (std::string_view(id) == "review-view" ? "/figma/icon-51.svg" :
         std::string_view(id) == "files-view" ? "/figma/icon-18.svg" :
                                                "/figma/icon-19.svg");
    icon->SetAttribute("src", source);
  }
}

void DeskController::refresh_review() {
  if (review_future_.valid()) {
    review_refresh_queued_ = true;
    return;
  }
  renderer_.review_loading();
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async, [root] {
    ReviewTaskResult result;
    result.kind = ReviewTaskResult::Kind::status;
    result.snapshot = GitService(root).status();
    result.success = result.snapshot.repository;
    result.error = result.snapshot.error;
    return result;
  });
}

void DeskController::render_review_snapshot(const GitSnapshot& snapshot) {
  renderer_.review(snapshot);
  const auto branch_name = snapshot.branch.empty() ? "workspace" : snapshot.branch;
  text(document_, "environment-branch", branch_name);
  text(document_, "environment-change-count",
       std::to_string(snapshot.files.size()) + " 个文件");
}

void DeskController::apply_review_task() {
  if (!review_future_.valid() ||
      review_future_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
    return;
  auto result = review_future_.get();
  if (result.kind == ReviewTaskResult::Kind::status) {
    render_review_snapshot(result.snapshot);
  } else if (result.kind == ReviewTaskResult::Kind::branches) {
    renderer_.branches(result.branches, result.snapshot.branch, result.error);
  } else if (result.kind == ReviewTaskResult::Kind::checkout) {
    if (result.success) {
      if (auto* menu = document_.GetElementById("branch-menu"))
        menu->SetClass("hidden", true);
      file_tree_children_.clear();
      refresh_files();
      show_toast("已切换到 " + result.branch);
      render_review_snapshot(result.snapshot);
    } else {
      show_toast(result.error);
    }
  } else if (result.kind == ReviewTaskResult::Kind::diff) {
    if (result.diff) {
      renderer_.diff(*result.diff, result.path, result.staged);
      if (auto* surface = dynamic_cast<ElementDiffSurface*>(
              document_.GetElementById("diff-surface"))) {
        surface->set_diff(*result.diff);
        surface->set_split_view(diff_split_view_);
        surface->AddEventListener("mousescroll", this);
      }
    } else
      renderer_.diff_error(result.error);
  } else if (result.kind == ReviewTaskResult::Kind::mutation &&
             result.operation.starts_with("discard-")) {
    if (!result.success) {
      show_toast(result.error);
    } else {
      pending_discard_path_ = result.path;
      pending_discard_hunk_ = result.hunk;
      pending_discard_file_ = result.operation == "discard-file";
      pending_discard_hash_ = result.content_hash;
      text(document_, "discard-description",
           escape(std::string(pending_discard_file_
               ? "将放弃文件的全部未暂存修改：" : "将放弃这个修改块：") +
               result.path));
      toggle_hidden("discard-overlay");
    }
  } else if (result.kind == ReviewTaskResult::Kind::mutation ||
             result.kind == ReviewTaskResult::Kind::discard) {
    if (!result.success) {
      show_toast(result.error);
    } else {
      render_review_snapshot(result.snapshot);
      current_diff_path_.clear();
      renderer_.close_diff();
    }
  } else if (result.kind == ReviewTaskResult::Kind::commit) {
    if (!result.success) {
      auto& view = view_model_.state();
      view.review_has_files = false;
      view.review_title = result.commit_created
          ? "提交已创建，但推送失败" : "提交失败";
      view.review_detail = result.error;
      view_model_.dirty();
      if (result.commit_created && result.push_requested) {
        if (auto* overlay = document_.GetElementById("commit-overlay"))
          overlay->SetClass("hidden", true);
        show_toast("本地提交已保留；推送失败：" + result.error);
      }
    } else {
      if (auto* overlay = document_.GetElementById("commit-overlay"))
        overlay->SetClass("hidden", true);
      renderer_.close_diff();
      show_toast(result.push_requested ? "已提交并推送" : "已创建提交");
      render_review_snapshot(result.snapshot);
    }
  }
  if (result.snapshot.repository) {
    view_model_.state().branch = result.snapshot.branch.empty()
        ? "无 Git" : result.snapshot.branch;
    view_model_.dirty();
  } else if (result.kind == ReviewTaskResult::Kind::status) {
    view_model_.state().branch = "无 Git";
    view_model_.dirty();
  }
  if (review_refresh_queued_) {
    review_refresh_queued_ = false;
    refresh_review();
  }
}

void DeskController::toggle_branch_menu() {
  auto* menu = document_.GetElementById("branch-menu");
  if (!menu)
    return;
  if (!menu->IsClassSet("hidden")) {
    menu->SetClass("hidden", true);
    return;
  }
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  renderer_.branch_loading();
  menu->SetClass("hidden", false);
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async, [root] {
    ReviewTaskResult result;
    result.kind = ReviewTaskResult::Kind::branches;
    GitService service(root);
    result.branches = service.branches(result.error);
    result.snapshot = service.status();
    result.success = result.error.empty();
    return result;
  });
}

void DeskController::switch_branch(Rml::Element& element) {
  const auto branch = element.GetAttribute<Rml::String>("git-branch", "");
  if (branch.empty())
    return;
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async,
      [root, branch = std::string(branch)] {
        ReviewTaskResult result;
        result.kind = ReviewTaskResult::Kind::checkout;
        result.branch = branch;
        GitService service(root);
        result.success = service.checkout_branch(branch, result.error);
        result.snapshot = service.status();
        return result;
      });
  renderer_.branch_loading();
}

void DeskController::refresh_files() {
  if (!document_.GetElementById("file-tree"))
    return;
  if (!file_tree_children_.contains(""))
    load_tree_children("");
  rebuild_file_tree();
}

void DeskController::load_tree_children(std::string relative_directory) {
  relative_directory = std::filesystem::path(relative_directory).generic_string();
  if (file_tree_future_.valid()) {
    queued_tree_directory_ = std::move(relative_directory);
    return;
  }
  loading_tree_directory_ = std::move(relative_directory);
  const auto root = workspace_.root();
  const auto directory = loading_tree_directory_;
  file_tree_future_ = std::async(std::launch::async, [root, directory] {
    return WorkspaceService(root).children(directory, 4000);
  });
}

void DeskController::rebuild_file_tree() {
  auto* surface = dynamic_cast<ElementFileTree*>(
      document_.GetElementById("file-tree"));
  if (!surface)
    return;
  std::vector<WorkspaceEntry> rows;
  const auto append = [this, &rows](auto&& self, const std::string& parent,
                                     const std::size_t depth) -> void {
    const auto found = file_tree_children_.find(parent);
    if (found == file_tree_children_.end())
      return;
    for (auto entry : found->second) {
      entry.depth = depth;
      const auto key = std::filesystem::path(entry.relative_path).generic_string();
      entry.relative_path = key;
      entry.expanded = entry.directory && expanded_directories_.contains(key);
      rows.push_back(entry);
      if (entry.expanded)
        self(self, key, depth + 1);
    }
  };
  append(append, "", 0);
  surface->set_rows(std::move(rows));
  if (const auto root = file_tree_children_.find("");
      root != file_tree_children_.end())
    text(document_, "environment-index",
         std::to_string(root->second.size()) + " 个根条目");
  if (!current_file_.empty()) {
    std::error_code error;
    const auto relative = std::filesystem::relative(
        current_file_, workspace_.root(), error);
    if (!error)
      surface->set_selected(relative.generic_string());
  }
}

void DeskController::handle_file_tree(const float local_y) {
  auto* surface = dynamic_cast<ElementFileTree*>(
      document_.GetElementById("file-tree"));
  if (!surface)
    return;
  const auto selected = surface->row_at(local_y);
  if (!selected)
    return;
  handle_file_tree_entry(*selected);
}

void DeskController::handle_file_tree_entry(const WorkspaceEntry& entry) {
  auto* surface = dynamic_cast<ElementFileTree*>(
      document_.GetElementById("file-tree"));
  if (!surface)
    return;
  if (entry.directory) {
    const auto path = std::filesystem::path(entry.relative_path).generic_string();
    selected_tree_path_ = path;
    selected_tree_directory_ = true;
    surface->set_selected(path);
    if (expanded_directories_.erase(path) == 0) {
      expanded_directories_.insert(path);
      if (!file_tree_children_.contains(path))
        load_tree_children(path);
    }
    rebuild_file_tree();
    return;
  }
  Rml::Element synthetic("button");
  synthetic.SetAttribute("data-path", entry.relative_path);
  preview_file(synthetic);
  selected_tree_path_ = entry.relative_path;
  selected_tree_directory_ = false;
  surface->set_selected(entry.relative_path);
}

void DeskController::search_files() {
  auto* input = control(document_, "file-search");
  if (!input)
    return;
  pending_file_query_ = input->GetValue();
  ++file_search_generation_;
  if (file_search_cancel_)
    file_search_cancel_->store(true);
  if (pending_file_query_.empty()) {
    file_search_cancel_.reset();
    file_tree_children_.clear();
    refresh_files();
    return;
  }
  if (file_search_future_.valid())
    return;
  const auto root = workspace_.root();
  const auto query = pending_file_query_;
  const auto generation = file_search_generation_;
  file_search_cancel_ = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = file_search_cancel_;
  file_search_future_ = std::async(std::launch::async,
      [root, query, generation, cancelled] {
        return FileSearchTaskResult{
            generation, query,
            WorkspaceService(root).search(query, 200, cancelled.get())};
      });
}

void DeskController::render_search_results(
    const std::vector<WorkspaceSearchResult>& results) {
  auto* tree = dynamic_cast<ElementFileTree*>(document_.GetElementById("file-tree"));
  if (!tree)
    return;
  std::vector<WorkspaceEntry> rows;
  rows.reserve(results.size());
  for (const auto& result : results) {
    auto label = result.relative_path + ":" + std::to_string(result.line) +
                 ":" + std::to_string(result.column) + "  " + result.preview;
    if (label.size() > 220)
      label.resize(220);
    rows.push_back({workspace_.root() / result.relative_path,
                    result.relative_path, std::move(label), 0, false, false});
  }
  tree->set_rows(std::move(rows));
}

void DeskController::open_file_operation(std::string operation) {
  auto selected = selected_tree_path_;
  if (selected.empty() && !current_file_.empty()) {
    std::error_code error;
    selected = std::filesystem::relative(
        current_file_, workspace_.root(), error).generic_string();
    if (error)
      selected.clear();
  }
  if ((operation == "rename" || operation == "delete") && selected.empty()) {
    show_toast("请先在文件树中选择文件或文件夹");
    return;
  }
  pending_file_operation_ = std::move(operation);
  auto* input = control(document_, "file-operation-name");
  auto* label = document_.GetElementById("file-operation-label");
  auto* confirm = document_.GetElementById("confirm-file-operation");
  const bool deleting = pending_file_operation_ == "delete";
  if (input) {
    input->SetClass("hidden", deleting);
    if (pending_file_operation_ == "new-file") input->SetValue("untitled.txt");
    else if (pending_file_operation_ == "new-folder") input->SetValue("new-folder");
    else if (pending_file_operation_ == "rename")
      input->SetValue(std::filesystem::path(selected).filename().string());
    else input->SetValue("");
  }
  if (label) label->SetClass("hidden", deleting);
  if (confirm) {
    confirm->SetClass("danger-button", deleting);
    confirm->SetClass("primary-button", !deleting);
    confirm->SetInnerRML(deleting ? "确认删除" : "确认");
  }
  const auto directory = selected_tree_directory_
      ? std::filesystem::path(selected)
      : std::filesystem::path(selected).parent_path();
  if (pending_file_operation_ == "new-file") {
    text(document_, "file-operation-title", "新建文件");
    text(document_, "file-operation-description",
         escape("位置：" + (directory.empty() ? std::string("工作区根目录")
                                                : directory.generic_string())));
  } else if (pending_file_operation_ == "new-folder") {
    text(document_, "file-operation-title", "新建文件夹");
    text(document_, "file-operation-description",
         escape("位置：" + (directory.empty() ? std::string("工作区根目录")
                                                : directory.generic_string())));
  } else if (pending_file_operation_ == "rename") {
    text(document_, "file-operation-title", "重命名");
    text(document_, "file-operation-description", escape("选中项：" + selected));
  } else {
    text(document_, "file-operation-title", "删除文件或文件夹");
    text(document_, "file-operation-description",
         escape("将永久删除工作区中的 “" + selected + "”。此操作不会经过回收站。"));
  }
  text(document_, "file-operation-error", "");
  if (auto* overlay = document_.GetElementById("file-operation-overlay"))
    overlay->SetClass("hidden", false);
  if (input && !deleting)
    input->Focus();
}

void DeskController::confirm_file_operation() {
  auto* input = control(document_, "file-operation-name");
  auto selected = std::filesystem::path(selected_tree_path_);
  const auto directory = selected_tree_directory_ ? selected : selected.parent_path();
  const auto name = input ? std::filesystem::path(std::string(input->GetValue()))
                          : std::filesystem::path{};
  std::string error;
  bool success = false;
  std::filesystem::path resulting_path;
  if (pending_file_operation_ == "new-file") {
    resulting_path = directory / name;
    success = workspace_.create_file(resulting_path, {}, error);
  } else if (pending_file_operation_ == "new-folder") {
    resulting_path = directory / name;
    success = workspace_.create_directory(resulting_path, error);
  } else if (pending_file_operation_ == "rename") {
    resulting_path = selected.parent_path() / name;
    success = workspace_.rename_entry(selected, name, error);
    if (success && !current_file_.empty()) {
      std::error_code relative_error;
      const auto current_relative = std::filesystem::relative(
          current_file_, workspace_.root(), relative_error);
      if (!relative_error && current_relative == selected)
        current_file_ = workspace_.root() / resulting_path;
    }
  } else if (pending_file_operation_ == "delete") {
    success = workspace_.remove_entry(selected, true, error);
    if (success && !current_file_.empty()) {
      std::error_code relative_error;
      const auto current_relative = std::filesystem::relative(
          current_file_, workspace_.root(), relative_error);
      if (!relative_error && (current_relative == selected ||
          current_relative.generic_string().starts_with(
              selected.generic_string() + "/"))) {
        current_file_.clear();
        view_model_.state().file_open = false;
        view_model_.dirty();
        if (auto* surface = dynamic_cast<ElementCodeSurface*>(
                document_.GetElementById("file-preview")))
          surface->set_document({}, {}, 0, false);
        text(document_, "file-path", "选择文件以预览或编辑");
      }
    }
    selected_tree_path_.clear();
    selected_tree_directory_ = false;
  }
  if (!success) {
    text(document_, "file-operation-error", escape(error));
    return;
  }
  selected_tree_path_ = resulting_path.generic_string();
  selected_tree_directory_ = pending_file_operation_ == "new-folder";
  pending_file_operation_.clear();
  if (auto* overlay = document_.GetElementById("file-operation-overlay"))
    overlay->SetClass("hidden", true);
  file_tree_children_.clear();
  refresh_files();
  refresh_review();
  show_toast("工作区文件操作已完成");
}

void DeskController::send_message() {
  if (chat_future_.valid()) {
    stop_message();
    return;
  }
  auto* composer = control(document_, "composer");
  auto* conversation = document_.GetElementById("conversation");
  if (!composer || !conversation || composer->GetValue().empty() ||
      change_set_future_.valid()) return;
  const std::string prompt = composer->GetValue();
  if (auto* selected = navigation_.selected(); selected &&
      selected->kind == "session" && !selected->title_manual) {
    const auto title = automatic_title(prompt);
    if (navigation_.rename_selected(title, false)) {
      view_model_.state().session_title = title;
      render_navigation();
      save_navigation();
    }
    pending_automatic_title_ = true;
  }
  if (auto* initial = document_.GetElementById("initial-session"))
    initial->SetClass("hidden", true);
  conversation->SetClass("hidden", false);
  composer->SetValue("");
  update_composer_placeholder();
  selected_attachment_.clear();
  if (auto* pill = document_.GetElementById("attachment-pill")) {
    pill->SetClass("hidden", true);
    pill->SetInnerRML("");
  }
  const auto ray = active_ray_;
  const auto provider = settings_.provider();
  const auto model = settings_.model();
  const auto effort = settings_.effort();
  const auto access = settings_.access();
  const auto change_run = tokmon::make_id("desk-agent-run");
  active_chat_request_id_ = tokmon::next_snow_request_id();
  const auto request_id = active_chat_request_id_;
  view_model_.state().chat_running = true;
  view_model_.state().chat_stopping = false;
  view_model_.dirty();
  update_composer_placeholder();
  chat_future_ = std::async(std::launch::async,
      [this, prompt, ray, provider, model, effort, access, change_run,
       request_id] {
        ChatTaskResult task;
        const bool tracking = change_tracker_.begin(change_run,
                                                     task.tracker_error);
        task.stream = daemon_.stream_intent(
            "chat", tokmon::cbor::object({
                {"text", prompt}, {"ray", ray}, {"surface", "desktop-ui"},
                {"name", provider}, {"model", model}, {"effort", effort},
                {"access_mode", access}}), snow_cursor_,
            [this](tokmon::Photon photon) {
              std::scoped_lock lock(photon_mutex_);
              pending_photons_.push_back(std::move(photon));
            }, request_id);
        if (tracking)
          task.changes = change_tracker_.finish(task.tracker_error);
        return task;
      });
}

void DeskController::stop_message() {
  if (!chat_future_.valid() || active_chat_request_id_ == 0 ||
      chat_cancel_future_.valid())
    return;
  const auto request_id = active_chat_request_id_;
  view_model_.state().chat_stopping = true;
  view_model_.dirty();
  chat_cancel_future_ = std::async(std::launch::async,
      [this, request_id] {
        ChatCancelResult result;
        result.success = daemon_.cancel(request_id, result.error);
        return result;
      });
}

void DeskController::choose_attachment() {
  if (attachment_dialog_ &&
      !attachment_dialog_->complete.load(std::memory_order_acquire))
    return;
  attachment_dialog_ = std::make_shared<AttachmentDialogState>();
  auto* state = new std::shared_ptr<AttachmentDialogState>(attachment_dialog_);
  const auto start = workspace_.root().string();
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* files, int) {
        std::unique_ptr<std::shared_ptr<AttachmentDialogState>> holder(
            static_cast<std::shared_ptr<AttachmentDialogState>*>(userdata));
        auto shared = *holder;
        {
          std::scoped_lock lock(shared->mutex);
          if (!files)
            shared->error = SDL_GetError();
          else if (files[0])
            shared->selected = std::filesystem::path(files[0]);
        }
        shared->complete.store(true, std::memory_order_release);
      },
      state, platform_.window(), nullptr, 0, start.c_str(), false);
}

void DeskController::apply_pending_photons() {
  std::vector<tokmon::Photon> incoming;
  {
    std::scoped_lock lock(photon_mutex_);
    incoming.swap(pending_photons_);
  }
  if (incoming.empty())
    return;
  for (auto& photon : incoming) {
    snow_cursor_ = std::max(snow_cursor_, photon.sequence);
    if (active_ray_.empty() && !photon.ray.empty())
      active_ray_ = photon.ray;
    const auto found = std::ranges::find(photons_, photon.id, &tokmon::Photon::id);
    if (found == photons_.end())
      photons_.push_back(std::move(photon));
    else
      *found = std::move(photon);
  }
  std::ranges::sort(photons_, {}, &tokmon::Photon::sequence);
  conversation_dirty_ = true;
}

void DeskController::render_conversation() {
  if (!conversation_dirty_)
    return;
  conversation_dirty_ = false;
  auto* conversation = document_.GetElementById("conversation");
  if (!conversation)
    return;
  const float previous_scroll = conversation->GetScrollTop();
  const bool followed_tail = conversation->GetScrollHeight() <= 0.f ||
      previous_scroll + conversation->GetClientHeight() >=
          conversation->GetScrollHeight() - 36.f;
  const auto requested_start = followed_tail
      ? std::numeric_limits<std::size_t>::max()
      : conversation_window_start_;
  const auto result = renderer_.conversation(photons_, requested_start);
  conversation_total_turns_ = result.total_turns;
  const auto maximum_start = result.total_turns > conversation_window_turns
      ? result.total_turns - conversation_window_turns : 0;
  conversation_window_start_ = followed_tail
      ? maximum_start : std::min(conversation_window_start_, maximum_start);
  conversation_follow_tail_pending_ = followed_tail;
  render_trajectory();
}

void DeskController::render_trajectory() {
  trajectory_.render(photons_, active_ray_, snow_cursor_);
}

void DeskController::export_trajectory() {
  std::filesystem::path path;
  std::string error;
  if (!trajectory_.export_json(workspace_.root() / "exports", photons_,
                               active_ray_, snow_cursor_, path, error)) {
    show_toast("导出失败：" + error);
    return;
  }
  show_toast("轨迹已导出到 " + path.generic_string());
}

void DeskController::seed_acceptance_conversation(const std::size_t turns) {
  photons_.clear();
  photons_.reserve(turns * 2);
  for (std::size_t index = 0; index < turns; ++index) {
    photons_.push_back(tokmon::Photon{
        .sequence = index * 2 + 1,
        .id = "acceptance-photon-" + std::to_string(index),
        .ray = "acceptance-ray",
        .kind = "user.message",
        .schema = "tokmon.user.message.v1",
        .payload = tokmon::cbor::object(
            {{"text", "验收会话消息 " + std::to_string(index)}}),
        .committed_at_ms = static_cast<std::int64_t>(index * 1800)});
    const auto answer = index == 0
        ? std::string("已完成验收样例。\n\n```cpp\nint answer = 42;\n```\n\n代码块和整条消息都可以复制。")
        : "验收回复 " + std::to_string(index) +
              "：Markdown **粗体**、列表与滚动语义保持稳定。";
    photons_.push_back(tokmon::Photon{
        .sequence = index * 2 + 2,
        .id = "acceptance-assistant-" + std::to_string(index),
        .ray = "acceptance-ray",
        .kind = "assistant.message",
        .schema = "tokmon.assistant.message.v1",
        .payload = tokmon::cbor::object({{"text", answer}}),
        .committed_at_ms = static_cast<std::int64_t>(index * 1800 + 1200)});
  }
  snow_cursor_ = turns * 2;
  active_ray_ = "acceptance-ray";
  conversation_window_start_ = 0;
  conversation_dirty_ = true;
  render_conversation();
}

void DeskController::seed_acceptance_trajectory(const bool include_error) {
  photons_.clear();
  const auto add = [this](const std::uint64_t sequence, std::string kind,
                          std::string schema, tokmon::cbor::Value payload,
                          const std::int64_t time) {
    photons_.push_back(tokmon::Photon{
        .sequence = sequence,
        .id = "acceptance-trace-" + std::to_string(sequence),
        .ray = "acceptance-ray",
        .kind = std::move(kind),
        .schema = std::move(schema),
        .payload = std::move(payload),
        .committed_at_ms = time});
  };
  add(1, "user.message", "tokmon.user.message.v1",
      tokmon::cbor::object({{"text", "检查工作区并完成目标"}}), 1000);
  add(2, "runtime.context", "tokmon.runtime.context.v1",
      tokmon::cbor::object({{"workspace", workspace_.root().generic_string()}}),
      1120);
  add(3, "model.started", "tokmon.model.started.v1",
      tokmon::cbor::object({{"model", "local-deterministic"}}), 1280);
  add(4, "model.tool-call", "tokmon.model.tool-call.v1",
      tokmon::cbor::object({{"kind", "process.exec"},
                            {"command", "git status --short"}}), 1660);
  add(5, "tool.result", "tokmon.tool.result.v1",
      tokmon::cbor::object({{"result", "工作区状态已读取"},
                            {"duration_ms", std::int64_t{312}}}), 1972);
  add(6, "assistant.message", "tokmon.assistant.message.v1",
      tokmon::cbor::object({{"text", "检查完成，工作区状态正常。"}}), 2310);
  add(7, "model.usage", "tokmon.model.usage.v1",
      tokmon::cbor::object({{"input_tokens", std::int64_t{1324}},
                            {"output_tokens", std::int64_t{7132}}}), 2460);
  if (include_error)
    add(8, "act.failed", "tokmon.act.failed.v1",
        tokmon::cbor::object({{"detail", "命令退出码为 1；工作区内容未被覆盖"},
                              {"duration_ms", std::int64_t{84}}}), 2600);
  snow_cursor_ = photons_.back().sequence;
  active_ray_ = "acceptance-ray";
  trajectory_.select(include_error ? 8 : 5);
  conversation_window_start_ = 0;
  conversation_dirty_ = true;
  render_conversation();
}

void DeskController::seed_acceptance_chat_state(const bool running,
                                                const bool stopping) {
  view_model_.state().chat_running = running;
  view_model_.state().chat_stopping = running && stopping;
  view_model_.dirty();
  update_composer_placeholder();
}

void DeskController::seed_acceptance_navigation_context() {
  view_model_.state().navigation_context_top = "270dp";
  view_model_.state().navigation_context_visible = true;
  view_model_.dirty();
}

void DeskController::render_navigation() {
  const auto* query = control(document_, "navigation-search");
  renderer_.navigation(navigation_, query ? query->GetValue() : "",
                       navigation_.selected_workspace());
}

void DeskController::open_navigation_context(Rml::Element& item,
                                             const float mouse_y) {
  const auto id = item.GetAttribute<Rml::String>("nav-id", "");
  const auto found = std::ranges::find(navigation_.items(), id,
                                      &DeskNavigationItem::id);
  if (found == navigation_.items().end() || found->kind != "session")
    return;
  (void)navigation_.select(id);
  active_ray_ = found->ray;
  view_model_.state().session_title = found->title;
  const auto ratio = document_.GetContext()
      ? std::max(document_.GetContext()->GetDensityIndependentPixelRatio(),
                 0.01f)
      : 1.f;
  const auto logical_y = std::clamp(mouse_y / ratio, 80.f, 760.f);
  view_model_.state().navigation_context_top =
      std::to_string(logical_y) + "dp";
  view_model_.state().navigation_context_visible = true;
  view_model_.dirty();
  render_navigation();
}

void DeskController::close_navigation_context() {
  view_model_.state().navigation_context_visible = false;
  view_model_.dirty();
}

void DeskController::delete_navigation_session() {
  if (!navigation_.remove_selected_session()) {
    show_toast("只能删除会话");
    close_navigation_context();
    return;
  }
  auto* selected = navigation_.selected();
  if (!selected || selected->kind != "session")
    selected = &navigation_.create_session("新会话");
  active_ray_ = selected->ray;
  photons_.clear();
  conversation_dirty_ = true;
  view_model_.state().session_title = selected->title;
  close_navigation_context();
  render_navigation();
  save_navigation();
  show_toast("会话已删除");
}

bool DeskController::update_navigation_scrollbar() {
  auto* tree = document_.GetElementById("navigation-tree");
  auto* thumb = document_.GetElementById("navigation-scrollbar-thumb");
  if (!tree || !thumb)
    return false;

  const float viewport_height = tree->GetClientHeight();
  const float content_height = tree->GetScrollHeight();
  const bool visible = viewport_height > 0.f &&
      content_height > viewport_height + 0.5f;
  if (!visible) {
    const bool changed = navigation_thumb_visible_;
    navigation_thumb_visible_ = false;
    navigation_thumb_height_ = -1.f;
    navigation_thumb_top_ = -1.f;
    thumb->SetClass("hidden", true);
    return changed;
  }

  const float thumb_height = std::max(
      36.f, viewport_height * viewport_height / content_height);
  const float travel = std::max(0.f, viewport_height - thumb_height);
  const float scroll_range = std::max(1.f, content_height - viewport_height);
  const float thumb_top = tree->GetOffsetTop() +
      std::clamp(tree->GetScrollTop() / scroll_range, 0.f, 1.f) * travel;

  const bool changed = !navigation_thumb_visible_ ||
      std::abs(navigation_thumb_height_ - thumb_height) > 0.25f ||
      std::abs(navigation_thumb_top_ - thumb_top) > 0.25f;
  if (changed) {
    navigation_thumb_visible_ = true;
    navigation_thumb_height_ = thumb_height;
    navigation_thumb_top_ = thumb_top;
    thumb->SetClass("hidden", false);
    thumb->SetProperty("height", std::to_string(thumb_height) + "px");
    thumb->SetProperty("top", std::to_string(thumb_top) + "px");
  }
  return changed;
}

void DeskController::apply_settings(const tokmon::cbor::Value& payload) {
  const auto* values = tokmon::cbor::find(payload, "values");
  if (!values || !values->as_map())
    return;
  settings_.apply_shared(payload);
  auto* selected = navigation_.selected();
  if (navigation_.items().empty() || !selected ||
      !same_workspace(navigation_.selected_workspace(), workspace_.root())) {
    (void)navigation_.ensure_workspace_project(workspace_.root());
    selected = &navigation_.create_session("新会话");
  } else if (selected->kind == "project") {
    selected = &navigation_.create_session("新会话");
  }
  active_ray_.clear();
  if (selected) {
    active_ray_ = selected->ray;
    view_model_.state().session_title = selected->title;
  }
  view_model_.dirty();
  render_navigation();
  render_settings_page(settings_.page());
  enqueue_intent("model.providers", tokmon::cbor::object({}));
}

void DeskController::save_navigation() {
  std::string error;
  if (!state_store_.save_navigation(navigation_.encode(), error)) {
    text(document_, "daemon-status", escape("导航保存失败：" + error));
    return;
  }
  text(document_, "daemon-status", "导航已保存到 tokmon-desk 本地状态");
}

void DeskController::handle_navigation(Rml::Element& element) {
  const auto id = element.GetAttribute<Rml::String>("nav-id", "");
  if (id.empty() || !navigation_.select(id))
    return;
  const auto* selected = navigation_.selected();
  if (selected &&
      !same_workspace(navigation_.selected_workspace(), workspace_.root())) {
    begin_workspace_switch(navigation_.selected_workspace(),
                           selected->kind == "project");
    render_navigation();
    save_navigation();
    return;
  }
  if (selected && selected->kind == "session") {
    active_ray_ = selected->ray;
    view_model_.state().session_title = selected->title;
    photons_.clear();
    conversation_dirty_ = true;
    request_selected_surface();
  } else if (selected && selected->kind == "project") {
    auto& created = navigation_.create_session("新会话");
    active_ray_.clear();
    photons_.clear();
    conversation_dirty_ = true;
    view_model_.state().session_title = created.title;
  }
  render_navigation();
  save_navigation();
}

void DeskController::begin_workspace_switch(std::filesystem::path target,
                                            const bool create_session_after) {
  if (workspace_switch_future_.valid()) {
    text(document_, "daemon-status", "正在切换工作空间，请稍候");
    return;
  }
  if (chat_future_.valid() || startup_future_.valid() || intent_future_.valid() ||
      review_future_.valid() ||
      !intent_queue_.empty()) {
    text(document_, "daemon-status",
         "当前工作空间仍有后台请求；完成后再切换工作空间");
    return;
  }
  if (const auto current = documents_.snapshot(current_file_);
      current && current->dirty) {
    text(document_, "daemon-status",
         "当前文件有未保存修改；保存或重新载入后再切换工作空间");
    return;
  }
  std::error_code error;
  target = std::filesystem::weakly_canonical(std::move(target), error);
  if (error || !std::filesystem::is_directory(target, error)) {
    text(document_, "daemon-status", "目标工作空间不是可读目录");
    return;
  }
  create_session_after_workspace_switch_ = create_session_after;
  text(document_, "daemon-status", "正在连接目标工作空间后台服务…");
#if defined(_WIN32)
  const auto daemon_executable =
      std::filesystem::path(SDL_GetBasePath()) / "tokmon.exe";
#else
  const auto daemon_executable =
      std::filesystem::path(SDL_GetBasePath()) / "tokmon";
#endif
  workspace_switch_future_ = std::async(
      std::launch::async, [target, daemon_executable] {
        WorkspaceSwitchResult result;
        result.workspace = target;
        auto paths = tokmon::resolve_paths(target);
        if (!paths) {
          result.error = paths.error().describe();
          return result;
        }
        result.endpoint = tokmon::workspace_snow_endpoint(
            paths->run, paths->project.parent_path());
        auto connection = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
            .endpoint = result.endpoint,
            .workspace = target,
            .executable = daemon_executable});
        if (!connection) {
          result.error = connection.error().describe();
          return result;
        }
        result.started = connection->started;
        auto lease = tokmon::DaemonClientLease::attach(
            tokmon::DaemonClientOptions{
                .endpoint = result.endpoint,
                .client_id = tokmon::make_id("tokmon-desk-workspace-client"),
                .client_kind = "desktop",
                .shutdown_when_idle = true,
                .idle_timeout = std::chrono::milliseconds(250),
                .lease_ttl = std::chrono::seconds(6)});
        if (!lease) {
          result.error = lease.error().describe();
          return result;
        }
        result.lease.emplace(std::move(*lease));
        return result;
      });
}

void DeskController::finish_workspace_switch() {
  if (!workspace_switch_future_.valid() ||
      workspace_switch_future_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
    return;
  auto result = workspace_switch_future_.get();
  if (!result.error.empty() || !result.lease) {
    text(document_, "daemon-status",
         escape("工作空间切换失败；原工作空间仍连接：" + result.error));
    create_session_after_workspace_switch_ = false;
    return;
  }
  if (workspace_lease_)
    (void)workspace_lease_->detach();
  workspace_lease_.emplace(std::move(*result.lease));
  std::string error;
  if (!workspace_.set_root(result.workspace, error)) {
    text(document_, "daemon-status", escape("工作空间切换失败：" + error));
    return;
  }
  settings_.set("last_workspace", workspace_.root().generic_string());
  std::string settings_error;
  if (!save_local_settings(settings_error))
    text(document_, "daemon-status",
         escape("工作空间已切换，但保存最近工作空间失败：" + settings_error));
  watcher_.reset(result.workspace);
  git_.set_workspace(result.workspace);
  change_tracker_.set_workspace(result.workspace);
  current_change_set_.reset();
  daemon_.set_endpoint(result.endpoint);
  file_tree_children_.clear();
  expanded_directories_.clear();
  if (file_search_cancel_)
    file_search_cancel_->store(true);
  ++file_search_generation_;
  selected_tree_path_.clear();
  selected_tree_directory_ = false;
  current_file_.clear();
  view_model_.state().file_open = false;
  view_model_.dirty();
  current_diff_path_.clear();
  photons_.clear();
  active_ray_.clear();
  snow_cursor_ = 0;
  startup_loaded_ = false;
  conversation_dirty_ = true;
  terminal_.set_workspace(result.workspace);
  const auto name = workspace_.root().filename().string();
  view_model_.state().workspace_name = name;
  view_model_.state().workspace_path = workspace_.root().generic_string();
  view_model_.state().settings.workspace_path = view_model_.state().workspace_path;
  view_model_.state().branch = "正在读取 Git…";
  view_model_.dirty();
  text(document_, "file-path", "选择文件以预览或编辑");
  if (auto* editor = dynamic_cast<ElementCodeSurface*>(
          document_.GetElementById("file-preview")))
    editor->set_document({}, {}, 0, false);
  text(document_, "daemon-status",
       result.started ? "目标工作空间后台服务已启动" : "目标工作空间已连接");
  refresh_files();
  refresh_review();
  if (create_session_after_workspace_switch_) {
    create_session_after_workspace_switch_ = false;
    (void)navigation_.create_session("新会话");
    view_model_.state().session_title = "新会话";
    render_navigation();
    save_navigation();
  } else if (const auto* selected = navigation_.selected();
             selected && selected->kind == "session") {
    active_ray_ = selected->ray;
    view_model_.state().session_title = selected->title;
    request_selected_surface();
  }
  backend_ready_.store(true, std::memory_order_release);
}

void DeskController::filter_navigation() {
  render_navigation();
}

void DeskController::request_selected_surface() {
  if (active_ray_.empty() || startup_future_.valid()) {
    startup_loaded_ = true;
    return;
  }
  const auto ray = active_ray_;
  startup_action_ = "surface";
  startup_future_ = std::async(std::launch::async, [this, ray] {
    return daemon_.stream_intent("surface", tokmon::cbor::object({{"ray", ray}}),
                                 snow_cursor_, [](tokmon::Photon) {});
  });
}

void DeskController::choose_starter(Rml::Element& card) {
  const auto kind = card.GetAttribute<Rml::String>("starter-kind", "");
  auto* composer = control(document_, "composer");
  if (!composer)
    return;
  if (kind == "explore")
    composer->SetValue("请全面分析并梳理当前工作空间的代码架构、模块依赖与核心实现逻辑。");
  else if (kind == "build")
    composer->SetValue("我想为当前工作空间构建一个新功能，请帮我规划设计方案并编写实现代码。");
  else if (kind == "review")
    composer->SetValue("请对当前项目代码进行全面审查，指出潜在质量风险、规范问题并提出优化重构建议。");
  else if (kind == "fix")
    composer->SetValue("请帮我诊断当前工作空间中的报错和运行异常，定位原因并提供修复补丁。");
  update_composer_placeholder();
  composer->Focus();
}

void DeskController::update_composer_placeholder() {
  const auto* composer = control(document_, "composer");
  if (auto* placeholder = document_.GetElementById("composer-placeholder"))
    placeholder->SetClass("hidden", composer && !composer->GetValue().empty());
  if (auto* send = document_.GetElementById("send-button")) {
    const bool running = view_model_.state().chat_running;
    send->SetClass("running", running);
    send->SetClass("stopping", view_model_.state().chat_stopping);
    send->SetClass("disabled", !running &&
        (!composer || composer->GetValue().empty()));
  }
}

void DeskController::render_slash_commands() {
  auto* composer = control(document_, "composer");
  if (!composer)
    return;
  const std::string value = composer->GetValue();
  if (value.empty() || value.front() != '/' ||
      value.find_first_of(" \\t\\r\\n") != std::string::npos) {
    slash_command_count_ = 0;
    slash_command_index_ = 0;
    renderer_.slash_commands({}, 0, false);
    return;
  }
  std::string query = value.substr(1);
  std::ranges::transform(query, query.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  std::vector<std::pair<std::string_view, std::string_view>> matches;
  for (const auto& command : slash_commands) {
    std::string name(command.first.substr(1));
    std::ranges::transform(name, name.begin(), [](const unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (query.empty() || name.find(query) != std::string::npos ||
        command.second.find(query) != std::string_view::npos)
      matches.push_back(command);
  }
  slash_command_count_ = matches.size();
  slash_command_index_ = matches.empty()
      ? 0 : std::min(slash_command_index_, matches.size() - 1);
  renderer_.slash_commands(matches, slash_command_index_, true);
}

void DeskController::select_slash_command(const std::size_t index) {
  auto* composer = control(document_, "composer");
  const auto& commands = view_model_.state().slash_commands;
  if (!composer || commands.empty())
    return;
  const auto selected = std::min(index, commands.size() - 1);
  composer->SetValue(commands[selected].command + " ");
  update_composer_placeholder();
  composer->Focus();
  slash_command_count_ = 0;
  slash_command_index_ = 0;
  renderer_.slash_commands({}, 0, false);
}

void DeskController::create_session() {
  auto* title_input = control(document_, "new-session-title");
  std::string title_value = title_input ? title_input->GetValue() : "";
  const auto first = title_value.find_first_not_of(" \t\r\n");
  const auto last = title_value.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) {
    text(document_, "new-session-error", "请输入会话名称");
    if (title_input)
      title_input->Focus();
    return;
  }
  title_value = title_value.substr(first, last - first + 1);
  if (title_value.size() > 120) {
    text(document_, "new-session-error", "会话名称不能超过 120 个字节");
    if (title_input)
      title_input->Focus();
    return;
  }
  text(document_, "new-session-error", "");
  if (auto* project = control(document_, "new-session-project");
      project && !project->GetValue().empty())
    (void)navigation_.select(project->GetValue());
  auto& created = navigation_.create_session(title_value);
  created.title_manual = title_value != "新会话";
  active_ray_.clear();
  photons_.clear();
  conversation_dirty_ = true;
  view_model_.state().session_title = created.title;
  render_navigation();
  save_navigation();
  if (auto* overlay = document_.GetElementById("new-session-overlay"))
    overlay->SetClass("hidden", true);
  if (auto* composer = control(document_, "composer")) composer->Focus();
}

void DeskController::preview_file(Rml::Element& row) {
  const auto relative = row.GetAttribute<Rml::String>("data-path", "");
  if (relative.empty()) return;
  const auto selected = workspace_.root() / relative;
  std::error_code type_error;
  if (std::filesystem::is_directory(selected, type_error)) return;
  // At the legacy launcher width the file feature intentionally shows only
  // the tree. Opening a file expands to the old editor-capable width.
  if (right_panel_width_ < 408) {
    right_panel_width_ = 620;
    settings_.set("right_panel_width",
                  static_cast<std::int64_t>(right_panel_width_));
    apply_panel_layout();
    std::string ignored;
    (void)save_local_settings(ignored);
  }
  current_file_.clear();
  view_model_.state().file_open = false;
  view_model_.dirty();
  heavy_focus_ = HeavyFocus::none;
  ++syntax_generation_;
  pending_syntax_.reset();
  if (auto* editor = dynamic_cast<ElementCodeSurface*>(
          document_.GetElementById("file-preview")))
    editor->set_document({}, {}, 0, false);
  pending_file_load_ = selected;
  ++file_load_generation_;
  text(document_, "file-path", escape(relative) + " · 后台载入中…");
  text(document_, "syntax-status", "等待文档载入");
  start_pending_file_load();
}

void DeskController::start_pending_file_load() {
  if (file_load_future_.valid() || !pending_file_load_)
    return;
  const auto path = std::move(*pending_file_load_);
  pending_file_load_.reset();
  const auto generation = file_load_generation_;
  const auto workspace = workspace_.root();
  const auto recovery = recovery_store_;
  file_load_future_ = std::async(std::launch::async,
      [path, generation, workspace, recovery] {
        FileLoadTaskResult result;
        result.generation = generation;
        result.path = path;
        DocumentStore detached;
        result.snapshot = detached.open(path, result.error);
        if (result.snapshot)
          result.recovery = recovery.load(result.snapshot->path, workspace,
                                          result.recovery_error);
        return result;
      });
}

void DeskController::apply_file_load() {
  if (!file_load_future_.valid() ||
      file_load_future_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
    return;
  auto result = file_load_future_.get();
  if (result.generation == file_load_generation_) {
    if (!result.snapshot) {
      text(document_, "file-path", escape(result.error));
    } else {
      std::string adopt_error;
      if (!documents_.adopt(*result.snapshot, adopt_error)) {
        text(document_, "file-path", escape(adopt_error));
      } else {
        current_file_ = result.snapshot->path;
        view_model_.state().file_open = true;
        view_model_.dirty();
        last_recovery_version_ = 0;
        last_recovery_dirty_ = false;
        if (result.recovery) {
          if (result.recovery->disk_hash == result.snapshot->disk_hash &&
              result.recovery->text != result.snapshot->text &&
              documents_.edit(result.snapshot->path, 0,
                              result.snapshot->text.size(),
                              result.recovery->text,
                              result.snapshot->version, adopt_error)) {
            show_toast("已恢复上次未保存的编辑内容");
          } else if (result.recovery->disk_hash != result.snapshot->disk_hash) {
            show_toast("恢复快照对应的磁盘文件已变化，未自动覆盖");
          }
        } else if (!result.recovery_error.empty()) {
          show_toast("恢复快照不可用：" + result.recovery_error);
        }
        std::error_code relative_error;
        const auto relative = std::filesystem::relative(
            current_file_, workspace_.root(), relative_error);
        text(document_, "file-path",
             escape(relative_error ? current_file_.filename().generic_string()
                                   : relative.generic_string()));
        refresh_code_surface(false);
        update_editor_status();
      }
    }
  }
  start_pending_file_load();
}

void DeskController::save_file() {
  if (current_file_.empty()) return;
  const auto before = documents_.snapshot(current_file_);
  if (!before) return;
  std::string error;
  if (!documents_.save(current_file_, before->version, error)) {
    text(document_, "file-path", escape("保存失败：" + error));
    return;
  }
  watcher_.acknowledge_self_write(current_file_);
  text(document_, "file-path", escape(current_file_.filename().string() + " · 已保存"));
  update_editor_status();
  refresh_review();
}

void DeskController::undo_file() {
  const auto before = documents_.snapshot(current_file_);
  std::string error;
  if (!before || !documents_.undo(current_file_, before->version, error)) {
    text(document_, "file-path", escape("撤销失败：" + error));
    return;
  }
  refresh_code_surface(true);
  update_editor_status();
}

void DeskController::redo_file() {
  const auto before = documents_.snapshot(current_file_);
  std::string error;
  if (!before || !documents_.redo(current_file_, before->version, error)) {
    text(document_, "file-path", escape("重做失败：" + error));
    return;
  }
  refresh_code_surface(true);
  update_editor_status();
}

void DeskController::reload_file() {
  if (current_file_.empty())
    return;
  std::string error;
  if (!documents_.reload(current_file_, true, error)) {
    text(document_, "file-path", escape("重新载入失败：" + error));
    return;
  }
  refresh_code_surface(false);
  update_editor_status();
}

void DeskController::refresh_code_surface(const bool preserve_caret) {
  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document_.GetElementById("file-preview"));
  const auto snapshot = documents_.snapshot(current_file_);
  if (!editor || !snapshot)
    return;
  const auto language = syntax_language_for_path(current_file_.generic_string());
  editor->set_document(snapshot->text, {}, snapshot->version,
                       preserve_caret);
  if (snapshot->large_file) {
    ++syntax_generation_;
    pending_syntax_.reset();
    text(document_, "syntax-status",
         std::string(syntax_language_name(language)) +
             " · 大文件模式（已关闭全量语法解析）");
    return;
  }
  text(document_, "syntax-status",
       std::string(syntax_language_name(language)) + " · 后台解析中…");
  schedule_syntax(*snapshot);
}

void DeskController::schedule_syntax(const DocumentSnapshot& snapshot) {
  pending_syntax_ = SyntaxTaskRequest{
      .path = snapshot.path,
      .text = snapshot.text,
      .version = snapshot.version,
      .generation = ++syntax_generation_,
      .language = syntax_language_for_path(snapshot.path.generic_string())};
  start_pending_syntax();
}

void DeskController::start_pending_syntax() {
  if (syntax_future_.valid() || !pending_syntax_)
    return;
  auto request = std::move(*pending_syntax_);
  pending_syntax_.reset();
  syntax_future_ = std::async(std::launch::async,
      [this, request = std::move(request)]() mutable {
        SyntaxTaskResult result;
        result.request = std::move(request);
        const auto started = std::chrono::steady_clock::now();
        result.success = syntax_.update(result.request.language,
                                        result.request.text, result.error);
        if (result.success)
          result.spans = syntax_.spans();
        result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        return result;
      });
}

void DeskController::apply_syntax_task() {
  if (!syntax_future_.valid() ||
      syntax_future_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
    return;
  auto result = syntax_future_.get();
  const auto current = documents_.snapshot(current_file_);
  if (current && result.request.path == current_file_ &&
      result.request.version == current->version &&
      result.request.generation == syntax_generation_) {
    if (result.success) {
      if (auto* editor = dynamic_cast<ElementCodeSurface*>(
              document_.GetElementById("file-preview")))
        editor->set_document(current->text, std::move(result.spans),
                             current->version, true);
      text(document_, "syntax-status",
           std::string(syntax_language_name(result.request.language)) + " · " +
               std::to_string(syntax_.spans().size()) + " spans · " +
               std::to_string(result.elapsed.count() / 1000) + " ms");
    } else {
      text(document_, "syntax-status", escape(result.error));
    }
  }
  start_pending_syntax();
}

void DeskController::apply_code_edit(const CodeEditIntent& intent) {
  if (current_file_.empty())
    return;
  const auto snapshot = documents_.snapshot(current_file_);
  if (!snapshot)
    return;
  std::string error;
  if (!documents_.edit(current_file_, intent.offset, intent.erase_count,
                       intent.replacement, snapshot->version, error)) {
    text(document_, "file-path", escape("编辑失败：" + error));
    return;
  }
  refresh_code_surface(true);
  if (auto* editor = dynamic_cast<ElementCodeSurface*>(
          document_.GetElementById("file-preview")))
    editor->set_caret_offset(intent.caret_after);
  update_editor_status();
}

void DeskController::find_editor(const bool backwards) {
  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document_.GetElementById("file-preview"));
  auto* query = control(document_, "editor-find");
  if (!editor || !query)
    return;
  const bool found = editor->find(query->GetValue(), backwards);
  text(document_, "editor-find-status", found ? "已定位" : "未找到");
  if (found) {
    heavy_focus_ = HeavyFocus::editor;
    editor->Focus();
  }
}

void DeskController::replace_editor_selection() {
  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document_.GetElementById("file-preview"));
  auto* query = control(document_, "editor-find");
  auto* replacement = control(document_, "editor-replace");
  if (!editor || !query || !replacement || query->GetValue().empty())
    return;
  if (editor->selected_text() != query->GetValue()) {
    find_editor(false);
    return;
  }
  if (auto edit = editor->insert_text(replacement->GetValue())) {
    apply_code_edit(*edit);
    if (auto* refreshed = dynamic_cast<ElementCodeSurface*>(
            document_.GetElementById("file-preview")))
      refreshed->set_caret_offset(edit->caret_after);
    text(document_, "editor-find-status", "已替换 1 处");
  }
}

void DeskController::go_to_editor_line() {
  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document_.GetElementById("file-preview"));
  auto* input = control(document_, "editor-line");
  if (!editor || !input)
    return;
  try {
    const auto line = static_cast<std::size_t>(
        std::stoull(std::string(input->GetValue())));
    const bool found = editor->go_to_line(line);
    text(document_, "editor-find-status", found ? "已跳转" : "行号超出范围");
    if (found) {
      heavy_focus_ = HeavyFocus::editor;
      editor->Focus();
    }
  } catch (...) {
    text(document_, "editor-find-status", "请输入有效行号");
  }
}

void DeskController::match_editor_bracket() {
  if (auto* editor = dynamic_cast<ElementCodeSurface*>(
          document_.GetElementById("file-preview")))
    text(document_, "editor-find-status",
         editor->jump_to_matching_bracket() ? "已跳到匹配括号" : "光标附近没有匹配括号");
}

void DeskController::update_editor_status() {
  const auto snapshot = documents_.snapshot(current_file_);
  auto* undo = control(document_, "undo-file");
  auto* redo = control(document_, "redo-file");
  if (undo)
    undo->SetDisabled(!snapshot || !snapshot->can_undo);
  if (redo)
    redo->SetDisabled(!snapshot || !snapshot->can_redo);
  if (!snapshot)
    return;
  if (snapshot->external_conflict) {
    text(document_, "file-path",
         "检测到外部修改冲突 · 点击“重新载入”放弃本地编辑");
    return;
  }
  std::error_code relative_error;
  auto display = std::filesystem::relative(snapshot->path, workspace_.root(),
                                           relative_error);
  if (relative_error || display.empty())
    display = snapshot->path.filename();
  text(document_, "file-path",
       escape(display.generic_string()) +
           (snapshot->dirty ? " · 未保存" : " · 已保存"));
  schedule_document_recovery(*snapshot);
}

void DeskController::schedule_document_recovery(
    const DocumentSnapshot& snapshot) {
  if (snapshot.version == last_recovery_version_ &&
      snapshot.dirty == last_recovery_dirty_)
    return;
  last_recovery_version_ = snapshot.version;
  last_recovery_dirty_ = snapshot.dirty;
  pending_recovery_snapshot_ = snapshot;
  pending_recovery_workspace_ = workspace_.root();
  start_pending_document_recovery();
}

void DeskController::start_pending_document_recovery() {
  if (recovery_future_.valid() || !pending_recovery_snapshot_)
    return;
  auto snapshot = std::move(*pending_recovery_snapshot_);
  pending_recovery_snapshot_.reset();
  const auto workspace = pending_recovery_workspace_;
  const auto store = recovery_store_;
  recovery_future_ = std::async(std::launch::async,
      [store, snapshot = std::move(snapshot), workspace] {
        RecoveryTaskResult result;
        result.success = store.save(snapshot, workspace, result.error);
        return result;
      });
}

void DeskController::preview_diff(Rml::Element& row) {
  const auto path = row.GetAttribute<Rml::String>("diff-path", "");
  if (path.empty()) return;
  const bool staged = row.GetAttribute<Rml::String>("diff-staged", "0") == "1";
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  current_diff_path_ = path;
  current_diff_staged_ = staged;
  view_model_.state().diff_visible = true;
  view_model_.state().diff_error_visible = true;
  view_model_.state().diff_error = "正在工作线程生成 Diff…";
  view_model_.dirty();
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async,
      [root, path = std::string(path), staged] {
        ReviewTaskResult result;
        result.kind = ReviewTaskResult::Kind::diff;
        result.path = path;
        result.staged = staged;
        result.diff = GitService(root).diff_model(path, staged, result.error);
        result.success = result.diff.has_value();
        return result;
      });
}

void DeskController::handle_git_action(Rml::Element& element) {
  const auto action = element.GetAttribute<Rml::String>("git-action", "");
  const auto path = element.GetAttribute<Rml::String>("git-path", "");
  const auto hunk_text = element.GetAttribute<Rml::String>("git-hunk", "0");
  const auto hunk = static_cast<std::size_t>(std::stoull(hunk_text));
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async,
      [root, action = std::string(action), path = std::string(path), hunk] {
        ReviewTaskResult result;
        result.kind = ReviewTaskResult::Kind::mutation;
        result.operation = action;
        result.path = path;
        result.hunk = hunk;
        GitService service(root);
        if (action == "stage-file")
          result.success = service.stage_file(path, result.error);
        else if (action == "unstage-file")
          result.success = service.unstage_file(path, result.error);
        else if (action == "stage-hunk")
          result.success = service.stage_hunk(path, hunk, result.error);
        else if (action == "unstage-hunk")
          result.success = service.unstage_hunk(path, hunk, result.error);
        else if (action == "discard-file" || action == "discard-hunk") {
          std::string read_error;
          const auto content = WorkspaceService(root).read_text(
              root / path, 16u * 1024u * 1024u, read_error);
          result.error = std::move(read_error);
          result.success = result.error.empty();
          result.content_hash = result.success
              ? DocumentStore::content_hash(content) : 0;
        }
        if (result.success && !action.starts_with("discard-"))
          result.snapshot = service.status();
        return result;
      });
  text(document_, "daemon-status", "正在执行 Git 操作…");
}

void DeskController::confirm_discard() {
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  toggle_hidden("discard-overlay");
  const auto root = workspace_.root();
  const auto path = pending_discard_path_;
  const auto hunk = pending_discard_hunk_;
  const auto hash = pending_discard_hash_;
  const auto file = pending_discard_file_;
  review_future_ = std::async(std::launch::async,
      [root, path, hunk, hash, file] {
        ReviewTaskResult result;
        result.kind = ReviewTaskResult::Kind::discard;
        result.operation = file ? "discard-file" : "discard-hunk";
        result.path = path;
        GitService service(root);
        result.success = file
            ? service.discard_file(path, hash, true, result.error)
            : service.discard_hunk(path, hunk, hash, result.error);
        result.snapshot = service.status();
        return result;
      });
}

void DeskController::commit_changes(bool push_after_commit) {
  auto* message = control(document_, "commit-message");
  if (!message || message->GetValue().empty()) {
    show_toast("请输入提交信息");
    if (message)
      message->Focus();
    return;
  }
  const std::string commit_message = message->GetValue();
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  const auto root = workspace_.root();
  review_future_ = std::async(std::launch::async,
      [root, commit_message, push_after_commit] {
        ReviewTaskResult result;
        result.kind = ReviewTaskResult::Kind::commit;
        result.push_requested = push_after_commit;
        GitService service(root);
        result.commit_created = service.commit(commit_message, result.error);
        result.success = result.commit_created &&
            (!push_after_commit || service.push(result.error));
        result.snapshot = service.status();
        return result;
      });
  text(document_, "review-empty", push_after_commit
      ? "正在提交并推送…" : "正在创建提交…");
}

void DeskController::show_settings_page(Rml::Element& navigation) {
  capture_settings_page();
  render_settings_page(navigation.GetAttribute<Rml::String>(
      "setting-page", "general"));
}

void DeskController::render_settings_page(std::string page) {
  settings_.show(std::move(page), workspace_.root());
}

void DeskController::capture_settings_page() {
  (void)settings_.shared_values();
}

void DeskController::save_settings() {
  const auto shared = settings_.shared_values();
  std::string error;
  if (!save_local_settings(error)) {
    settings_.set_status("本地设置保存失败：" + error);
    return;
  }
  enqueue_intent("settings.save", tokmon::cbor::object({{"values", shared}}));
  settings_.set_status("Desktop 偏好已本地保存；正在保存共享 Agent 配置…");
}

void DeskController::reset_settings() {
  settings_.reset(workspace_.root());
}

void DeskController::apply_providers(const tokmon::cbor::Value& payload) {
  settings_.apply_providers(payload);
}

void DeskController::enqueue_intent(std::string action,
                                    tokmon::cbor::Value payload) {
  intent_queue_.push_back({std::move(action), std::move(payload)});
  start_next_intent();
}

void DeskController::start_next_intent() {
  if (intent_future_.valid() || intent_queue_.empty())
    return;
  auto next = std::move(intent_queue_.front());
  intent_queue_.pop_front();
  intent_action_ = next.action;
  const auto cursor = snow_cursor_;
  intent_future_ = std::async(std::launch::async,
      [this, action = std::move(next.action), payload = std::move(next.payload), cursor] () mutable {
        return daemon_.stream_intent(std::move(action), std::move(payload), cursor,
                                     [](tokmon::Photon) {});
      });
}

void DeskController::finish_intent() {
  if (!intent_future_.valid() ||
      intent_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  auto result = intent_future_.get();
  snow_cursor_ = std::max(snow_cursor_, result.cursor);
  if (!result.success) {
    settings_.set_status("操作失败：" + result.error);
  } else if (intent_action_ == "model.providers") {
    apply_providers(result.payload);
  } else if (intent_action_ == "settings.save") {
    settings_.set_status(
        "Desktop 偏好已保存到本地；共享 Agent 配置已保存到当前项目");
  } else if (intent_action_ == "model.provider.use") {
    settings_.set_status("默认模型平台已切换；后台校验中");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.configure") {
    settings_.set_status("Provider 配置已原子保存；后台校验中");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.secret.set") {
    settings_.set_status("API Key 已写入系统安全存储");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.test") {
    settings_.set_status("Provider 测试已完成");
  }
  intent_action_.clear();
  start_next_intent();
}

bool DeskController::handle_raw_event(const SDL_Event& event) {
  const auto event_x = [this, &event]() {
    const float coordinate = event.type == SDL_EVENT_MOUSE_MOTION
        ? event.motion.x : event.button.x;
    return coordinate * platform_.design_coordinate_scale();
  };
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
    if (dismiss_transient_ui()) return true;
    const auto& view = view_model_.state();
    const auto* trajectory = document_.GetElementById("trajectory");
    if (trajectory && !trajectory->IsClassSet("hidden") &&
        (view.trajectory_zoomed || view.trajectory_selection_visible)) {
      trajectory_.clear_timeline_focus();
      render_trajectory();
      return true;
    }
  }
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    const float x = event_x();
    const float y = event.button.y * platform_.design_coordinate_scale();
    const float layout_scale = std::max(
        document_.GetContext()->GetDensityIndependentPixelRatio(), 0.01f);
    const float viewport_width = document_.GetClientWidth() / layout_scale;
    if (y >= 46.f + legacy_frame_inset && sidebar_visible_ &&
        std::abs(x - (static_cast<float>(sidebar_width_) +
                      legacy_frame_inset)) <= 6.f) {
      panel_resize_ = PanelResize::sidebar;
      panel_resize_anchor_x_ = x;
      panel_resize_start_width_ = sidebar_width_;
      SDL_CaptureMouse(true);
      return true;
    }
    if (y >= 46.f + legacy_frame_inset && right_panel_visible_ &&
        std::abs(x - (viewport_width - legacy_frame_inset -
                      static_cast<float>(right_panel_width_))) <= 6.f) {
      panel_resize_ = PanelResize::right;
      panel_resize_anchor_x_ = x;
      panel_resize_start_width_ = right_panel_width_;
      SDL_CaptureMouse(true);
      return true;
    }
  }
  if (event.type == SDL_EVENT_MOUSE_MOTION &&
      panel_resize_ != PanelResize::none) {
    const float x = event_x();
    const float layout_scale = std::max(
        document_.GetContext()->GetDensityIndependentPixelRatio(), 0.01f);
    const int viewport_width = static_cast<int>(std::lround(
        document_.GetClientWidth() / layout_scale - legacy_frame_inset * 2.f));
    if (panel_resize_ == PanelResize::sidebar) {
      const int next = panel_resize_start_width_ +
          static_cast<int>(std::lround(x - panel_resize_anchor_x_));
      const int limit = std::min(420, viewport_width -
          (right_panel_visible_ ? right_panel_width_ + 1 : 0) - 301);
      if (next <= 196) {
        set_sidebar_visible(false);
      } else if (limit >= 196) {
        sidebar_width_ = std::clamp(next, 196, limit);
        set_sidebar_visible(true);
      }
    } else {
      const int next = panel_resize_start_width_ -
          static_cast<int>(std::lround(x - panel_resize_anchor_x_));
      const int limit = std::min(720, viewport_width -
          (sidebar_visible_ ? sidebar_width_ + 1 : 0) - 301);
      if (next < 214) {
        set_right_panel_visible(false);
      } else if (limit >= 214) {
        right_panel_width_ = std::clamp(next, 214, limit);
        set_right_panel_visible(true);
      }
    }
    return true;
  }
  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
      event.button.button == SDL_BUTTON_LEFT &&
      panel_resize_ != PanelResize::none) {
    panel_resize_ = PanelResize::none;
    SDL_CaptureMouse(false);
    std::string ignored;
    (void)save_local_settings(ignored);
    return true;
  }
  if (event.type == SDL_EVENT_KEY_DOWN &&
      (event.key.mod & SDL_KMOD_CTRL) != 0) {
    const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
    if (event.key.key == SDLK_F && !shift && !current_file_.empty()) {
      heavy_focus_ = HeavyFocus::none;
      if (auto* input = control(document_, "editor-find"))
        input->Focus();
      return true;
    }
    if (event.key.key == SDLK_H && !shift && !current_file_.empty()) {
      heavy_focus_ = HeavyFocus::none;
      if (auto* input = control(document_, "editor-replace"))
        input->Focus();
      return true;
    }
    if (event.key.key == SDLK_G && !shift && !current_file_.empty()) {
      heavy_focus_ = HeavyFocus::none;
      if (auto* input = control(document_, "editor-line"))
        input->Focus();
      return true;
    }
    if (event.key.key == SDLK_N && !shift) {
      render_navigation();
      if (auto* overlay = document_.GetElementById("new-session-overlay"))
        overlay->SetClass("hidden", false);
      return true;
    }
    if (event.key.key == SDLK_COMMA && !shift) {
      render_settings_page(settings_.page());
      if (auto* overlay = document_.GetElementById("settings-overlay"))
        overlay->SetClass("hidden", false);
      return true;
    }
    if (event.key.key == SDLK_G && shift) {
      show_right_view("review-view");
      refresh_review();
      return true;
    }
    if (event.key.key == SDLK_P && !shift) {
      show_right_view("files-view");
      refresh_files();
      return true;
    }
    if (event.key.key == SDLK_P && shift) {
      if (auto* composer = control(document_, "composer")) {
        composer->SetValue("/");
        composer->Focus();
      }
      slash_command_index_ = 0;
      render_slash_commands();
      return true;
    }
  }
  if (heavy_focus_ == HeavyFocus::none)
    return false;
  if (event.type == SDL_EVENT_MOUSE_WHEEL) {
    if (heavy_focus_ == HeavyFocus::editor) {
      if (auto* editor = dynamic_cast<ElementCodeSurface*>(
              document_.GetElementById("file-preview")))
        editor->scroll_lines(static_cast<int>(std::lround(-event.wheel.y * 3.f)));
      return true;
    }
    if (heavy_focus_ == HeavyFocus::terminal) {
      return terminal_.handle_wheel(event);
    }
    return false;
  }

  if (event.type == SDL_EVENT_TEXT_INPUT) {
    if (heavy_focus_ == HeavyFocus::editor) {
      if (auto* editor = dynamic_cast<ElementCodeSurface*>(
              document_.GetElementById("file-preview"))) {
        editor->set_composition({}, 0, 0);
        if (auto edit = editor->insert_text(event.text.text))
          apply_code_edit(*edit);
      }
    } else if (heavy_focus_ == HeavyFocus::terminal) {
      return terminal_.handle_text(event);
    }
    return true;
  }
  if (event.type == SDL_EVENT_TEXT_EDITING) {
    if (heavy_focus_ == HeavyFocus::editor) {
      if (auto* editor = dynamic_cast<ElementCodeSurface*>(
              document_.GetElementById("file-preview")))
        editor->set_composition(event.edit.text,
                                static_cast<std::size_t>(
                                    std::max(event.edit.start, 0)),
                                static_cast<std::size_t>(
                                    std::max(event.edit.length, 0)));
    }
    return true;
  }
  if (event.type != SDL_EVENT_KEY_DOWN)
    return event.type == SDL_EVENT_KEY_UP;

  const auto modifiers = event.key.mod;
  const bool control_key = (modifiers & SDL_KMOD_CTRL) != 0;
  const bool shift_key = (modifiers & SDL_KMOD_SHIFT) != 0;
  if (event.key.key == SDLK_ESCAPE) {
    if (heavy_focus_ == HeavyFocus::editor)
      if (auto* editor = dynamic_cast<ElementCodeSurface*>(
              document_.GetElementById("file-preview")))
        editor->set_composition({}, 0, 0);
    heavy_focus_ = HeavyFocus::none;
    terminal_.release_focus();
    return true;
  }

  if (heavy_focus_ == HeavyFocus::terminal) {
    return terminal_.handle_key(event);
  }

  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document_.GetElementById("file-preview"));
  if (!editor)
    return true;
  if (control_key) {
    if (event.key.key == SDLK_S) save_file();
    else if (event.key.key == SDLK_Z && shift_key) redo_file();
    else if (event.key.key == SDLK_Z) undo_file();
    else if (event.key.key == SDLK_Y) redo_file();
    else if (event.key.key == SDLK_A) editor->select_all();
    else if (event.key.key == SDLK_C)
      platform_.SetClipboardText(editor->selected_text());
    else if (event.key.key == SDLK_X) {
      platform_.SetClipboardText(editor->selected_text());
      if (auto edit = editor->erase_backward()) apply_code_edit(*edit);
    } else if (event.key.key == SDLK_V) {
      Rml::String clipboard;
      platform_.GetClipboardText(clipboard);
      if (auto edit = editor->insert_text(clipboard)) apply_code_edit(*edit);
    }
    return true;
  }
  std::optional<CodeEditIntent> edit;
  switch (event.key.key) {
    case SDLK_BACKSPACE: edit = editor->erase_backward(); break;
    case SDLK_DELETE: edit = editor->erase_forward(); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: edit = editor->insert_text("\n"); break;
    case SDLK_TAB: edit = editor->insert_text("  "); break;
    case SDLK_LEFT: editor->move_horizontal(-1, shift_key); break;
    case SDLK_RIGHT: editor->move_horizontal(1, shift_key); break;
    case SDLK_UP: editor->move_vertical(-1, shift_key); break;
    case SDLK_DOWN: editor->move_vertical(1, shift_key); break;
    case SDLK_HOME: editor->move_line_edge(false, shift_key); break;
    case SDLK_END: editor->move_line_edge(true, shift_key); break;
    case SDLK_PAGEUP: editor->page(-1, shift_key); break;
    case SDLK_PAGEDOWN: editor->page(1, shift_key); break;
    default: break;
  }
  if (edit)
    apply_code_edit(*edit);
  return true;
}

void DeskController::ProcessEvent(Rml::Event& event) {
  auto* listener = event.GetCurrentElement();
  if (!listener) return;
  const auto& listener_id = listener->GetId();
  const bool overlay_listener =
      listener_id == "settings-overlay" || listener_id == "new-session-overlay" ||
      listener_id == "commit-overlay" || listener_id == "discard-overlay" ||
      listener_id == "terminal-paste-overlay" ||
      listener_id == "file-operation-overlay";
  // Overlay children have their own listeners. Let the event reach those
  // listeners exactly once, while retaining a direct backdrop click as the
  // dismiss gesture. Without this guard a child button bubbles back to this
  // controller through the overlay and toggle actions execute twice.
  if (event.GetType() == "click" && overlay_listener &&
      event.GetTargetElement() != listener)
    return;
  auto* element = listener;
  if (event.GetType() == "click" || event.GetType() == "keydown" ||
      event.GetType() == "mousedown") {
    for (auto* candidate = event.GetTargetElement(); candidate &&
         candidate != listener; candidate = candidate->GetParentNode()) {
      if (!candidate->GetAttribute<Rml::String>("nav-id", "").empty()) {
        element = candidate;
        break;
      }
      if (candidate->GetTagName() == "button") {
        element = candidate;
        break;
      }
    }
  }
  const auto& id = element->GetId();
  if (event.GetType() == "mousedown" &&
      !element->GetAttribute<Rml::String>("nav-id", "").empty() &&
      event.GetParameter<int>("button", 0) == 1) {
    open_navigation_context(
        *element, static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
    event.StopPropagation();
    return;
  }
  if (event.GetType() == "click" && event.GetTargetElement() == listener &&
      (id == "settings-overlay" || id == "new-session-overlay" ||
       id == "commit-overlay" || id == "discard-overlay" ||
       id == "terminal-paste-overlay" || id == "file-operation-overlay" ||
       id == "composer-popover")) {
    (void)dismiss_transient_ui();
    return;
  }
  if (listener_id == "trajectory-lane-track") {
    const auto absolute = listener->GetAbsoluteOffset(Rml::BoxArea::Content);
    const auto width = std::max(1.f, listener->GetClientWidth());
    const auto mouse_x = static_cast<float>(
        event.GetParameter<int>("mouse_x", static_cast<int>(absolute.x)));
    const auto fraction = std::clamp(
        static_cast<double>((mouse_x - absolute.x) / width), 0.0, 1.0);
    if (event.GetType() == "mousescroll") {
      trajectory_.zoom_timeline(
          fraction, event.GetParameter<float>("wheel_delta_y", 0.f));
      render_trajectory();
      event.StopPropagation();
      return;
    }
    if (event.GetType() == "keydown" &&
        event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE) {
      trajectory_.clear_timeline_focus();
      render_trajectory();
      event.StopPropagation();
      return;
    }
    if (event.GetType() == "mousedown") {
      const auto button = event.GetParameter<int>("button", 0);
      if (button == 0 || button == 1) {
        listener->Focus();
        trajectory_.begin_timeline_gesture(fraction, button == 1);
        render_trajectory();
      }
      return;
    }
    if (event.GetType() == "mousemove" &&
        trajectory_.timeline_gesture_active()) {
      trajectory_.update_timeline_gesture(fraction);
      render_trajectory();
      return;
    }
    if (event.GetType() == "mouseup" &&
        trajectory_.timeline_gesture_active()) {
      trajectory_.end_timeline_gesture(fraction);
      render_trajectory();
      return;
    }
  }
  if (id == "conversation" && event.GetType() == "mousescroll") {
    constexpr float kEstimatedTurnHeight = 210.f;
    constexpr std::size_t kLeadTurns = conversation_overscan_turns;
    const auto visible_turn = static_cast<std::size_t>(std::max(
        0.f, element->GetScrollTop()) / kEstimatedTurnHeight);
    const auto desired = visible_turn > kLeadTurns
        ? visible_turn - kLeadTurns : 0;
    const auto maximum = conversation_total_turns_ > conversation_window_turns
        ? conversation_total_turns_ - conversation_window_turns : 0;
    const auto clamped = std::min(desired, maximum);
    if (clamped != conversation_window_start_) {
      conversation_window_start_ = clamped;
      conversation_dirty_ = true;
    }
    return;
  }
  if (id == "diff-surface" && event.GetType() == "mousescroll") {
    if (auto* diff = dynamic_cast<ElementDiffSurface*>(element))
      diff->scroll_lines(static_cast<int>(std::lround(
          event.GetParameter<float>("wheel_delta_y", 0.f) * 3.f)));
    return;
  }
  if (id == "file-tree" && event.GetType() == "mousescroll") {
    if (auto* tree = dynamic_cast<ElementFileTree*>(element))
      tree->scroll_lines(static_cast<int>(std::lround(
          event.GetParameter<float>("wheel_delta_y", 0.f) * 3.f)));
    return;
  }
  if (id == "file-tree" && event.GetType() == "keydown") {
    auto* tree = dynamic_cast<ElementFileTree*>(element);
    if (!tree)
      return;
    const auto key = event.GetParameter<int>("key_identifier", 0);
    std::optional<WorkspaceEntry> selected;
    if (key == Rml::Input::KI_UP)
      selected = tree->move_selection(-1);
    else if (key == Rml::Input::KI_DOWN)
      selected = tree->move_selection(1);
    else if (key == Rml::Input::KI_HOME)
      selected = tree->select_edge(false);
    else if (key == Rml::Input::KI_END)
      selected = tree->select_edge(true);
    else if (key == Rml::Input::KI_RETURN)
      selected = tree->selected_row();
    else
      return;
    if (selected) {
      selected_tree_path_ = selected->relative_path;
      selected_tree_directory_ = selected->directory;
      if (key == Rml::Input::KI_RETURN)
        handle_file_tree_entry(*selected);
    }
    event.StopPropagation();
    return;
  }
  if (id == "file-preview" && event.GetType() == "mousescroll" &&
      (SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
    if (auto* editor = dynamic_cast<ElementCodeSurface*>(element))
      editor->scroll_columns(
          event.GetParameter<float>("wheel_delta_y", 0.f) * 42.f);
    return;
  }
  if (id == "file-tree" && event.GetType() == "click") {
    element->Focus();
    const auto absolute = element->GetAbsoluteOffset(Rml::BoxArea::Content);
    handle_file_tree(static_cast<float>(event.GetParameter<int>("mouse_y", 0)) -
                     absolute.y);
    return;
  }
  if (event.GetType() == "click" && id == "file-preview") {
    heavy_focus_ = HeavyFocus::editor;
    element->Focus();
    const auto absolute = element->GetAbsoluteOffset(Rml::BoxArea::Content);
    if (auto* editor = dynamic_cast<ElementCodeSurface*>(element))
      editor->click(
          static_cast<float>(event.GetParameter<int>("mouse_x", 0)) - absolute.x,
          static_cast<float>(event.GetParameter<int>("mouse_y", 0)) - absolute.y,
          (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    platform_.ActivateKeyboard(absolute, 17.f);
    return;
  }
  if (id == "terminal-surface" &&
      (event.GetType() == "mousedown" || event.GetType() == "mousemove" ||
       event.GetType() == "mouseup" || event.GetType() == "click")) {
    heavy_focus_ = HeavyFocus::terminal;
    (void)terminal_.handle_pointer(event);
    return;
  }
  if (event.GetType() == "click") {
    const auto copy_id = element->GetAttribute<Rml::String>(
        "data-copy-markdown", "");
    if (!copy_id.empty()) {
      if (const auto* payload = renderer_.conversation_copy_payload(copy_id)) {
        platform_.SetClipboardText(*payload);
        show_toast(element->IsClassSet("code-copy")
                       ? "代码已复制" : "消息已按纯文本复制");
      } else {
        show_toast("复制内容已离开当前虚拟窗口");
      }
      return;
    }
    if (const auto sequence = element->GetAttribute<Rml::String>(
            "data-trace-sequence", "");
        !sequence.empty()) {
      std::uint64_t value = 0;
      const auto [end, parse_error] = std::from_chars(
          sequence.data(), sequence.data() + sequence.size(), value);
      if (parse_error == std::errc{} && end == sequence.data() + sequence.size()) {
        trajectory_.select(value);
        render_trajectory();
      }
      return;
    }
    if (const auto tab = element->GetAttribute<Rml::String>(
            "trajectory-detail-tab", "");
        !tab.empty()) {
      trajectory_.select_detail_tab(tab);
      render_trajectory();
      return;
    }
    if (const auto label = element->GetAttribute<Rml::String>(
            "data-trace-turn", "");
        !label.empty()) {
      const auto separator = label.find_last_of(' ');
      const auto digits = separator == Rml::String::npos
          ? std::string_view(label) : std::string_view(label).substr(separator + 1);
      int turn = 0;
      const auto [end, parse_error] = std::from_chars(
          digits.data(), digits.data() + digits.size(), turn);
      if (parse_error == std::errc{} && end == digits.data() + digits.size()) {
        trajectory_.toggle_turn(turn);
        render_trajectory();
      }
      return;
    }
  }
  if (event.GetType() == "keydown") {
    const auto key = event.GetParameter<int>("key_identifier", 0);
    if (!element->GetAttribute<Rml::String>("nav-id", "").empty()) {
      if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_SPACE) {
        handle_navigation(*element);
        event.StopPropagation();
      }
      return;
    }
    if (id == "composer") {
      const bool slash_open = view_model_.state().slash_visible &&
          slash_command_count_ > 0;
      if (slash_open && (key == Rml::Input::KI_UP ||
                         key == Rml::Input::KI_DOWN)) {
        if (key == Rml::Input::KI_UP)
          slash_command_index_ = slash_command_index_ == 0
              ? slash_command_count_ - 1 : slash_command_index_ - 1;
        else
          slash_command_index_ = (slash_command_index_ + 1) %
              slash_command_count_;
        render_slash_commands();
        event.StopPropagation();
        return;
      }
      if (key == Rml::Input::KI_ESCAPE &&
          view_model_.state().composer_popover_visible) {
        slash_command_count_ = 0;
        slash_command_index_ = 0;
        renderer_.close_composer_popover();
        event.StopPropagation();
        return;
      }
      if (key == Rml::Input::KI_RETURN && slash_open) {
        select_slash_command(slash_command_index_);
        event.StopPropagation();
        return;
      }
    }
    if (key == Rml::Input::KI_RETURN) {
      if (id == "composer") send_message();
      else if (id == "editor-find") find_editor(false);
      else if (id == "editor-replace") replace_editor_selection();
      else if (id == "editor-line") go_to_editor_line();
    }
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change") &&
      id == "composer") {
    update_composer_placeholder();
    slash_command_index_ = 0;
    render_slash_commands();
    return;
  }
  if (event.GetType() == "click" &&
      (id == "editor-find" || id == "editor-replace" || id == "editor-line")) {
    heavy_focus_ = HeavyFocus::none;
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "file-search") {
    if (const auto* search = control(document_, "file-search"))
      if (auto* placeholder = document_.GetElementById("file-search-placeholder"))
        placeholder->SetClass("hidden", !search->GetValue().empty());
    search_files();
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "navigation-search") {
    if (const auto* search = control(document_, "navigation-search"))
      if (auto* placeholder =
              document_.GetElementById("navigation-search-placeholder"))
        placeholder->SetClass("hidden", !search->GetValue().empty());
    filter_navigation();
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "settings-search") {
    const auto query = control(document_, "settings-search")->GetValue();
    const std::pair<std::string_view, std::string_view> pages[] = {
        {"general", "语言启动默认工作区通用"}, {"model", "模型provider平台推理"},
        {"agents", "智能体角色子任务思考"}, {"skills", "skill技能自动唤起扩展"},
        {"rules", "规则agents提示词偏好"}, {"mcp", "mcp服务工具协议审批超时"},
        {"access", "文件访问命令审批联网高风险权限安全"}, {"workspace", "工作区索引同步git"},
        {"notifications", "通知桌面消息免打扰"}, {"appearance", "外观主题颜色密度缩放字体"},
        {"shortcuts", "快捷键键盘"}, {"account", "账户昵称邮箱云同步"},
        {"terminal", "终端shell滚动"}, {"browser", "浏览器chrome chromium profile"},
        {"about", "关于许可版本"}};
    if (!query.empty()) {
      for (const auto& [page, keywords] : pages)
        if (keywords.find(query) != std::string_view::npos) {
          render_settings_page(std::string(page));
          Rml::ElementList items;
          document_.QuerySelectorAll(items, "[setting-page]");
          for (auto* item : items)
            item->SetClass("active",
                item->GetAttribute<Rml::String>("setting-page", "") == page);
          break;
        }
    }
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "terminal-search") {
    terminal_.search();
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") && id == "trajectory-search") {
    if (const auto* search = control(document_, "trajectory-search")) {
      trajectory_.set_search(search->GetValue());
      render_trajectory();
    }
    return;
  }
  if (event.GetType() == "click" && id == "terminal-search") {
    heavy_focus_ = HeavyFocus::none;
    terminal_.release_focus();
    return;
  }
  if (event.GetType() == "click" &&
      element->GetAttribute<Rml::String>("nav-id", "").size()) {
    handle_navigation(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("git-branch", "").size()) {
    switch_branch(*element);
    return;
  }
  if (const auto terminal_tab =
          element->GetAttribute<Rml::String>("terminal-tab-id", "");
      !terminal_tab.empty()) {
    terminal_.select_tab(terminal_tab);
    return;
  }
  if (const auto provider = element->GetAttribute<Rml::String>("provider-id", "");
      !provider.empty()) {
    settings_.select_provider(provider);
    enqueue_intent("model.provider.use",
                   tokmon::cbor::object({{"name", settings_.provider()}}));
    render_settings_page("model");
    return;
  }
  if (const auto kind = element->GetAttribute<Rml::String>("choice-kind", "");
      !kind.empty()) {
    const auto value = element->GetAttribute<Rml::String>("choice-value", "");
    if (kind == "effort") {
      settings_.select_effort(value);
    } else if (kind == "access") {
      settings_.select_access(value);
    } else if (kind == "provider") {
      settings_.select_provider(value);
      enqueue_intent("model.provider.use",
                     tokmon::cbor::object({{"name", settings_.provider()}}));
    }
    renderer_.close_composer_popover();
    return;
  }
  if (const auto command = element->GetAttribute<Rml::String>("command-name", "");
      !command.empty()) {
    if (auto* composer = control(document_, "composer")) {
      composer->SetValue(command + " ");
      update_composer_placeholder();
      composer->Focus();
    }
    renderer_.close_composer_popover();
    return;
  }
  if (element->GetAttribute<Rml::String>("data-path", "").size()) {
    preview_file(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("git-action", "").size()) {
    handle_git_action(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("diff-path", "").size()) {
    preview_diff(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("setting-page", "").size()) {
    show_settings_page(*element);
    return;
  }
  if (const auto key = element->GetAttribute<Rml::String>("setting-toggle", "");
      !key.empty()) {
    settings_.toggle(key);
    return;
  }
  if (const auto key = element->GetAttribute<Rml::String>("setting-choice", "");
      !key.empty()) {
    settings_.choose(key,
        element->GetAttribute<Rml::String>("setting-value", ""));
    return;
  }
  if (element->GetAttribute<Rml::String>("starter-kind", "").size()) {
    choose_starter(*element);
    return;
  }
  if (id == "new-session-button") {
    render_navigation();
    toggle_hidden("new-session-overlay");
  }
  else if (id == "navigation-context-new") {
    close_navigation_context();
    auto& created = navigation_.create_session("新会话");
    active_ray_.clear();
    photons_.clear();
    conversation_dirty_ = true;
    view_model_.state().session_title = created.title;
    view_model_.dirty();
    render_navigation();
    save_navigation();
  }
  else if (id == "navigation-context-rename") {
    const auto* selected = navigation_.selected();
    close_navigation_context();
    renderer_.rename_popover(selected ? selected->title : "新会话");
  }
  else if (id == "navigation-context-delete") delete_navigation_session();
  else if (id == "settings-button") {
    render_settings_page(settings_.page());
    toggle_hidden("settings-overlay");
  }
  else if (id == "close-settings" || id == "cancel-settings")
    toggle_hidden("settings-overlay");
  else if (id == "save-settings") { save_settings(); toggle_hidden("settings-overlay"); }
  else if (id == "reset-settings") reset_settings();
  else if (id == "close-new-session" || id == "cancel-new-session") toggle_hidden("new-session-overlay");
  else if (id == "confirm-new-session") create_session();
  else if (id == "environment-toggle" || id == "environment-close") {
    const auto* panel = document_.GetElementById("environment-panel");
    const bool opening = panel && panel->IsClassSet("hidden");
    if (auto* mutable_panel = document_.GetElementById("environment-panel"))
      mutable_panel->SetClass("hidden", !opening);
    if (auto* orb = document_.GetElementById("environment-orb"))
      orb->SetClass("hidden", opening);
  }
  else if (id == "environment-refresh") {
    refresh_review();
    file_tree_children_.clear();
    refresh_files();
    text(document_, "environment-index", "正在刷新…");
  }
  else if (id == "environment-settings") {
    if (auto* panel = document_.GetElementById("environment-panel"))
      panel->SetClass("hidden", true);
    if (auto* orb = document_.GetElementById("environment-orb"))
      orb->SetClass("hidden", false);
    render_settings_page("workspace");
    if (auto* overlay = document_.GetElementById("settings-overlay"))
      overlay->SetClass("hidden", false);
  }
  else if (id == "chat-mode") {
    document_.GetElementById("chat-mode")->SetClass("active", true);
    document_.GetElementById("trajectory-mode")->SetClass("active", false);
    document_.GetElementById("trajectory")->SetClass("hidden", true);
    const bool empty = photons_.empty();
    document_.GetElementById("conversation")->SetClass("hidden", empty);
    document_.GetElementById("initial-session")->SetClass("hidden", !empty);
  }
  else if (id == "trajectory-mode") {
    document_.GetElementById("chat-mode")->SetClass("active", false);
    document_.GetElementById("trajectory-mode")->SetClass("active", true);
    document_.GetElementById("conversation")->SetClass("hidden", true);
    document_.GetElementById("initial-session")->SetClass("hidden", true);
    document_.GetElementById("trajectory")->SetClass("hidden", false);
    render_trajectory();
  }
  else if (id == "export-trajectory") export_trajectory();
  else if (id == "refresh-trajectory") {
    render_trajectory();
    show_toast("轨迹已刷新");
  }
  else if (id == "trajectory-duration-toggle") {
    trajectory_.toggle_actual_duration();
    render_trajectory();
  }
  else if (id == "trajectory-turns-toggle") {
    trajectory_.toggle_all_turns();
    render_trajectory();
  }
  else if (id == "trajectory-calls-toggle") {
    trajectory_.toggle_all_calls();
    render_trajectory();
  }
  else if (id == "trajectory-filter") {
    trajectory_.cycle_filter();
    render_trajectory();
  }
  else if (id == "trajectory-load-earlier") {
    trajectory_.load_earlier();
    render_trajectory();
  }
  else if (id == "trajectory-follow") {
    trajectory_.follow_latest();
    render_trajectory();
  }
  else if (id == "trajectory-detail-close") {
    trajectory_.close_details();
    render_trajectory();
  }
  else if (id == "trajectory-reset-timeline") {
    trajectory_.clear_timeline_focus();
    render_trajectory();
  }
  else if (id == "title-edit") {
    const auto* selected = navigation_.selected();
    renderer_.rename_popover(selected ? selected->title : "新会话");
  }
  else if (id == "confirm-title-rename") {
    auto* title = control(document_, "rename-title-input");
    if (title && navigation_.rename_selected(title->GetValue(), true)) {
      view_model_.state().session_title = title->GetValue();
      view_model_.dirty();
      render_navigation();
      save_navigation();
      if (!active_ray_.empty())
        enqueue_intent("command.execute", tokmon::cbor::object({
            {"text", "/rename " + std::string(title->GetValue())},
            {"ray", active_ray_}, {"surface", "desktop-ui"}}));
    }
    renderer_.close_composer_popover();
  }
  else if (id == "attach-button") choose_attachment();
  else if (id == "access-button" || id == "active-model" || id == "effort-button") {
    std::string title;
    std::vector<ComposerChoiceView> choices;
    if (id == "access-button") {
      title = "访问权限";
      choices = {{"access", "full", "完全访问", {}},
                 {"access", "restricted", "受限访问", {}}};
    } else if (id == "effort-button") {
      title = "推理强度";
      for (const char* value : {"标准", "高", "最高"})
        choices.push_back({"effort", value, value, {}});
    } else {
      title = "模型平台";
      if (const auto* providers = tokmon::cbor::find(
              settings_.providers_payload(), "providers");
          providers && providers->as_array())
        for (const auto& provider : *providers->as_array())
          choices.push_back({"provider", cbor_string(provider, "name"),
                             cbor_string(provider, "name"),
                             cbor_string(provider, "model")});
    }
    renderer_.choice_popover(std::move(title), std::move(choices));
  }
  else if (id == "thought-toggle") toggle_hidden("thought-content");
  else if (id == "workflow-toggle") toggle_hidden("workflow-content");
  else if (id == "sidebar-toggle") {
    set_sidebar_visible(!sidebar_visible_);
    std::string ignored;
    (void)save_local_settings(ignored);
  }
  else if (id == "right-toggle") {
    set_right_panel_visible(!right_panel_visible_);
    if (right_panel_visible_ && active_right_view_ == "launcher")
      show_right_launcher();
  }
  else if (id == "sidebar-restore") {
    set_sidebar_visible(true);
    std::string ignored;
    (void)save_local_settings(ignored);
  }
  else if (id == "right-restore") {
    set_right_panel_visible(true);
    if (active_right_view_ == "launcher")
      show_right_launcher();
  }
  else if (id == "right-collapse") {
    set_right_panel_visible(false);
  }
  else if (id == "right-fullscreen") {
    if (auto* body = document_.GetElementById("app-shell")) {
      const bool fullscreen = !body->IsClassSet("right-fullscreen");
      body->SetClass("right-fullscreen", fullscreen);
      if (auto* panel = document_.GetElementById("right-panel")) {
        if (fullscreen) {
          // apply_panel_layout() intentionally writes a persisted dp width as
          // an inline property. A stylesheet-only fullscreen rule cannot
          // override that inline value, and the stale narrow class would keep
          // the editor pane hidden even if the panel happened to grow.
          panel->SetProperty("left", "0dp");
          panel->SetProperty("width", "100%");
          panel->SetClass("compact", false);
          panel->SetClass("narrow", false);
        } else {
          panel->RemoveProperty("left");
          apply_panel_layout();
        }
      }
    }
  }
  else if (id == "right-tab-close") show_right_launcher();
  else if (id == "launcher-review" || id == "review-tab") {
    show_right_view("review-view");
    refresh_review();
  }
  else if (id == "launcher-files" || id == "files-tab") {
    show_right_view("files-view");
    refresh_files();
  }
  else if (id == "terminal-tab") {
    // A terminal is a full working surface rather than a compact launcher.
    // Keep the legacy 214dp rail for Review/Files launch actions, but expand
    // before creating the PTY so its toolbar and 80x24 cell grid are usable.
    if (right_panel_width_ < 520) {
      right_panel_width_ = 620;
      settings_.set("right_panel_width",
                    static_cast<std::int64_t>(right_panel_width_));
      apply_panel_layout();
      std::string ignored;
      (void)save_local_settings(ignored);
    }
    show_right_view("terminal-view");
    terminal_.start();
    if (auto* surface = document_.GetElementById("terminal-surface")) {
      heavy_focus_ = HeavyFocus::terminal;
      surface->Focus();
      (void)terminal_.resize();
      platform_.ActivateKeyboard(
          surface->GetAbsoluteOffset(Rml::BoxArea::Content), 17.f);
      if (terminal_.running()) {
        text(document_, "terminal-status", "正在运行 · 已聚焦");
        text(document_, "terminal-hint",
             "终端已聚焦 · 直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Esc 释放焦点");
      }
    }
  }
  else if (id == "terminal-new-tab") {
    terminal_.create_tab();
    terminal_.start();
  }
  else if (id == "terminal-close-tab") terminal_.close_tab();
  else if (id == "browser-tab") show_right_view("browser-view");
  else if (id == "diff-view-toggle") {
    diff_split_view_ = !diff_split_view_;
    text(document_, "diff-view-toggle", diff_split_view_ ? "统一" : "并排");
    if (auto* button = document_.GetElementById("diff-view-toggle"))
      button->SetClass("active", diff_split_view_);
    if (auto* surface = dynamic_cast<ElementDiffSurface*>(
            document_.GetElementById("diff-surface")))
      surface->set_split_view(diff_split_view_);
  }
  else if (id == "add-tab-button") toggle_hidden("right-tab-menu");
  else if (id == "branch-button") toggle_branch_menu();
  else if (id == "close-diff") {
    current_diff_path_.clear();
    renderer_.close_diff();
    refresh_review();
  }
  else if (id == "refresh-review") refresh_review();
  else if (id == "accept-agent-changes") {
    if (!current_change_set_ || change_set_future_.valid()) {
      show_toast("没有可接受的 Agent 修改，或操作仍在进行");
    } else {
      auto changes = *current_change_set_;
      change_set_future_ = std::async(std::launch::async,
          [this, changes = std::move(changes)]() mutable {
            ChangeSetTaskResult result{.changes = std::move(changes)};
            result.success = change_tracker_.accept(result.changes, result.error);
            return result;
          });
      show_toast("正在接受 Agent 修改…");
    }
  }
  else if (id == "reject-agent-changes") {
    if (!current_change_set_ || change_set_future_.valid()) {
      show_toast("没有可拒绝的 Agent 修改，或操作仍在进行");
    } else {
      auto changes = *current_change_set_;
      change_set_future_ = std::async(std::launch::async,
          [this, changes = std::move(changes)]() mutable {
            ChangeSetTaskResult result{.changes = std::move(changes),
                                       .reject = true};
            result.success = change_tracker_.reject(result.changes, result.error);
            return result;
          });
      show_toast("正在按 Desktop baseline 回滚 Agent 修改…");
    }
  }
  else if (id == "send-button") send_message();
  else if (id == "browser-launch" || id == "browser-go") browser_.launch();
  else if (id == "browser-refresh") browser_.refresh();
  else if (id == "browser-back") browser_.back();
  else if (id == "browser-forward") browser_.forward();
  else if (id == "browser-reload") browser_.reload();
  else if (id == "browser-takeover") browser_.toggle_takeover();
  else if (id == "browser-stop") browser_.stop();
  else if (id == "browser-click") browser_.click();
  else if (id == "browser-fill") browser_.fill();
  else if (id == "configure-provider") {
    enqueue_intent("model.provider.configure",
                   settings_.provider_configuration());
    settings_.set_status("正在保存 Provider 配置…");
  }
  else if (id == "store-provider-secret") {
    if (!view_model_.state().settings.provider_secret.empty()) {
      enqueue_intent("model.provider.secret.set", settings_.provider_secret());
      view_model_.state().settings.provider_secret.clear();
      view_model_.dirty();
      settings_.set_status("正在写入系统安全存储…");
    }
  }
  else if (id == "test-provider") {
    enqueue_intent("model.provider.test", settings_.provider_test());
    settings_.set_status("正在通过真实模型光路测试…");
  }
  else if (id == "open-notices") {
    const auto base = std::filesystem::path(SDL_GetBasePath());
    auto notices = base / "THIRD_PARTY_NOTICES.txt";
    if (!std::filesystem::exists(notices))
      notices = base / "THIRD_PARTY_NOTICES.md";
    std::string open_error;
    if (platform_.open_local_file(notices, open_error))
      settings_.set_status("已使用系统默认应用打开许可声明");
    else
      settings_.set_status("无法打开许可声明：" + open_error);
  }
  else if (id == "save-file") save_file();
  else if (id == "undo-file") undo_file();
  else if (id == "redo-file") redo_file();
  else if (id == "reload-file") reload_file();
  else if (id == "editor-find-previous") find_editor(true);
  else if (id == "editor-find-next") find_editor(false);
  else if (id == "editor-replace-one") replace_editor_selection();
  else if (id == "editor-go-line") go_to_editor_line();
  else if (id == "editor-match-bracket") match_editor_bracket();
  else if (id == "file-new") open_file_operation("new-file");
  else if (id == "folder-new") open_file_operation("new-folder");
  else if (id == "file-rename") open_file_operation("rename");
  else if (id == "file-delete") open_file_operation("delete");
  else if (id == "close-file-operation" || id == "cancel-file-operation") {
    pending_file_operation_.clear();
    toggle_hidden("file-operation-overlay");
  }
  else if (id == "confirm-file-operation") confirm_file_operation();
  else if (id == "minimize-button") platform_.minimize();
  else if (id == "maximize-button") platform_.toggle_maximize();
  else if (id == "close-button") quit_requested_ = true;
  else if (id == "commit-button") toggle_hidden("commit-overlay");
  else if (id == "close-commit" || id == "cancel-commit") toggle_hidden("commit-overlay");
  else if (id == "close-discard" || id == "cancel-discard") toggle_hidden("discard-overlay");
  else if (id == "confirm-discard") confirm_discard();
  else if (id == "cancel-terminal-paste") {
    terminal_.cancel_paste();
  }
  else if (id == "confirm-terminal-paste") terminal_.paste(true);
  else if (id == "terminal-clear-search") {
    if (auto* search = control(document_, "terminal-search"))
      search->SetValue("");
    terminal_.search();
  }
  else if (id == "confirm-commit") commit_changes(false);
  else if (id == "confirm-push") commit_changes(true);
}

void DeskController::apply_change_set_task() {
  if (!change_set_future_.valid() ||
      change_set_future_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
    return;
  auto result = change_set_future_.get();
  if (!result.success) {
    show_toast(result.error.empty() ? "Agent 修改审查操作失败" : result.error);
    return;
  }
  current_change_set_ = std::move(result.changes);
  if (auto* accept = document_.GetElementById("accept-agent-changes"))
    accept->SetClass("hidden", true);
  if (auto* reject = document_.GetElementById("reject-agent-changes"))
    reject->SetClass("hidden", true);
  if (result.reject) {
    show_toast("已按 Desktop baseline 回滚 Agent 修改");
    file_tree_children_.clear();
    refresh_files();
    refresh_review();
  } else {
    show_toast("已接受 Agent 修改；Git 暂存状态未改变");
  }
}

bool DeskController::update() {
  bool changed = false;
  changed = update_navigation_scrollbar() || changed;
  if (conversation_follow_tail_pending_) {
    conversation_follow_tail_pending_ = false;
    if (auto* conversation = document_.GetElementById("conversation")) {
      conversation->SetScrollTop(conversation->GetScrollHeight());
      changed = true;
    }
  }
  if (change_set_future_.valid() &&
      change_set_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    changed = true;
  apply_change_set_task();
  if (file_load_future_.valid() &&
      file_load_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    changed = true;
  apply_file_load();
  if (syntax_future_.valid() &&
      syntax_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    changed = true;
  apply_syntax_task();
  if (recovery_future_.valid() &&
      recovery_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    auto result = recovery_future_.get();
    if (!result.success && !result.error.empty())
      show_toast("编辑恢复快照保存失败：" + result.error);
    start_pending_document_recovery();
    changed = true;
  }
  if (review_future_.valid() &&
      review_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    changed = true;
  apply_review_task();
  if (toast_until_ != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= toast_until_) {
    toast_until_ = {};
    if (auto* toast = document_.GetElementById("right-toast"))
      toast->SetClass("hidden", true);
    changed = true;
  }
  if (workspace_switch_future_.valid() &&
      workspace_switch_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready)
    changed = true;
  finish_workspace_switch();
  if (attachment_dialog_ &&
      attachment_dialog_->complete.load(std::memory_order_acquire)) {
    std::filesystem::path selected;
    std::string dialog_error;
    {
      std::scoped_lock lock(attachment_dialog_->mutex);
      selected = attachment_dialog_->selected;
      dialog_error = attachment_dialog_->error;
    }
    attachment_dialog_.reset();
    changed = true;
    if (!dialog_error.empty()) {
      text(document_, "daemon-status", escape("附件选择失败：" + dialog_error));
    } else if (!selected.empty()) {
      selected_attachment_ = std::move(selected);
      std::error_code relative_error;
      auto display = std::filesystem::relative(
          selected_attachment_, workspace_.root(), relative_error);
      if (relative_error || display.empty() ||
          (display.begin() != display.end() && *display.begin() == ".."))
        display = selected_attachment_;
      const auto value = display.generic_string();
      if (auto* composer = control(document_, "composer")) {
        auto prompt = std::string(composer->GetValue());
        if (!prompt.empty() && !std::isspace(
                static_cast<unsigned char>(prompt.back())))
          prompt.push_back(' ');
        prompt += value.find(' ') == std::string::npos
            ? "@" + value : "@\"" + value + "\"";
        composer->SetValue(prompt);
        update_composer_placeholder();
      }
      if (auto* pill = document_.GetElementById("attachment-pill")) {
        pill->SetInnerRML(escape(selected_attachment_.filename().string()));
        pill->SetClass("hidden", false);
      }
    }
  }
  if (intent_future_.valid() &&
      intent_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    changed = true;
  finish_intent();
  if (!startup_future_.valid() &&
      startup_retry_at_ != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= startup_retry_at_) {
    startup_retry_at_ = {};
    backend_ready_.store(true, std::memory_order_release);
    changed = true;
  }
  if (backend_ready_.exchange(false, std::memory_order_acq_rel) &&
      !startup_future_.valid()) {
    startup_action_ = "validate";
    std::cerr << "tokmon-desk: requesting config.validate\n";
    startup_future_ = std::async(std::launch::async, [this] {
      return daemon_.stream_intent("config.validate", tokmon::cbor::object({}),
                                   snow_cursor_, [](tokmon::Photon) {});
    });
    changed = true;
  }
  apply_pending_photons();
  changed = changed || conversation_dirty_;
  render_conversation();
  const auto changes = watcher_.take_changes();
  if (!changes.empty()) {
    changed = true;
    for (const auto& change : changes)
      if (!current_file_.empty() && change.path == current_file_ &&
          change.origin == WorkspaceChangeOrigin::external)
        documents_.observe_external_change(current_file_);
    update_editor_status();
    refresh_review();
    if (pending_file_query_.empty()) {
      file_tree_children_.clear();
      refresh_files();
    }
  }
  if (file_tree_future_.valid() &&
      file_tree_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    file_tree_children_[loading_tree_directory_] = file_tree_future_.get();
    loading_tree_directory_.clear();
    rebuild_file_tree();
    changed = true;
    if (!queued_tree_directory_.empty()) {
      auto queued = std::move(queued_tree_directory_);
      queued_tree_directory_.clear();
      load_tree_children(std::move(queued));
    }
  }
  if (file_search_future_.valid() &&
      file_search_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    changed = true;
    const auto result = file_search_future_.get();
    auto* search = control(document_, "file-search");
    if (search && result.generation == file_search_generation_ &&
        result.query == pending_file_query_ &&
        search->GetValue() == pending_file_query_)
      render_search_results(result.results);
    else
      search_files();
  }
  if (startup_future_.valid() &&
      startup_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    changed = true;
    auto result = startup_future_.get();
    std::cerr << "tokmon-desk: startup action=" << startup_action_
              << " success=" << result.success
              << " error=" << result.error << '\n';
    if (!result.success) {
      if (result.error.find("invalid_state") != std::string::npos ||
          result.error.find("still running") != std::string::npos) {
        text(document_, "daemon-status", "正在检查配置");
        startup_retry_at_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(120);
      } else {
        text(document_, "daemon-status", "后台服务未连接");
      }
    } else {
      text(document_, "daemon-status", "后台服务已连接");
      snow_cursor_ = std::max(snow_cursor_, result.cursor);
      if (startup_action_ == "validate") {
        const auto* state = tokmon::cbor::find(result.payload, "state");
        if (state && state->as_string() == "starting") {
          text(document_, "daemon-status", "正在检查配置");
          startup_retry_at_ = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(120);
        } else if (state && state->as_string() == "failed") {
          const auto* detail = tokmon::cbor::find(result.payload, "error");
          text(document_, "daemon-status",
               escape(detail ? detail->as_string() : "配置校验失败"));
        } else {
          startup_action_ = "settings";
          startup_future_ = std::async(std::launch::async, [this] {
            return daemon_.stream_intent("settings.get", tokmon::cbor::object({}),
                                         snow_cursor_, [](tokmon::Photon) {});
          });
        }
      } else if (startup_action_ == "settings") {
        apply_settings(result.payload);
        request_selected_surface();
      } else {
        auto incoming = response_photons(result.payload, active_ray_);
        {
          std::scoped_lock lock(photon_mutex_);
          pending_photons_.insert(pending_photons_.end(),
              std::make_move_iterator(incoming.begin()),
              std::make_move_iterator(incoming.end()));
        }
        startup_loaded_ = true;
      }
    }
  }
  if (chat_future_.valid() &&
      chat_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    changed = true;
    auto chat_task = chat_future_.get();
    active_chat_request_id_ = 0;
    view_model_.state().chat_running = false;
    view_model_.state().chat_stopping = false;
    view_model_.dirty();
    update_composer_placeholder();
    auto result = std::move(chat_task.stream);
    if (chat_task.changes && !chat_task.changes->changes.empty()) {
        current_change_set_ = std::move(*chat_task.changes);
        if (auto* accept = document_.GetElementById("accept-agent-changes"))
          accept->SetClass("hidden", false);
        if (auto* reject = document_.GetElementById("reject-agent-changes"))
          reject->SetClass("hidden", false);
        show_toast("检测到 " + std::to_string(current_change_set_->changes.size()) +
                   " 个 Agent 修改，可接受或拒绝");
        refresh_review();
      } else if (!chat_task.tracker_error.empty()) {
        std::cerr << "tokmon-desk: change attribution ended: "
                  << chat_task.tracker_error << '\n';
    }
    snow_cursor_ = std::max(snow_cursor_, result.cursor);
    if (const auto* ray = tokmon::cbor::find(result.payload, "ray");
        ray && !ray->as_string().empty())
      active_ray_ = std::string(ray->as_string());
    if (!active_ray_.empty() && navigation_.bind_selected_ray(active_ray_)) {
      render_navigation();
      save_navigation();
    }
    if (pending_automatic_title_ && !active_ray_.empty()) {
      pending_automatic_title_ = false;
      if (const auto* selected = navigation_.selected())
        enqueue_intent("command.execute", tokmon::cbor::object({
            {"text", "/rename " + selected->title}, {"ray", active_ray_},
            {"surface", "desktop-ui"}}));
    }
    auto incoming = response_photons(result.payload, active_ray_);
    if (!result.success) {
      tokmon::Photon failure;
      failure.sequence = ++snow_cursor_;
      failure.id = "desk-local-failure-" + std::to_string(snow_cursor_);
      failure.ray = active_ray_;
      failure.kind = "act.failed";
      failure.payload = tokmon::cbor::object({{"detail", result.error}});
      incoming.push_back(std::move(failure));
    }
    {
      std::scoped_lock lock(photon_mutex_);
      pending_photons_.insert(pending_photons_.end(),
          std::make_move_iterator(incoming.begin()),
          std::make_move_iterator(incoming.end()));
    }
  }
  if (chat_cancel_future_.valid() &&
      chat_cancel_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    changed = true;
    const auto result = chat_cancel_future_.get();
    if (result.success)
      show_toast("已请求停止当前任务");
    else {
      view_model_.state().chat_stopping = false;
      view_model_.dirty();
      show_toast("停止失败：" + result.error);
    }
  }
  changed = browser_.update() || changed;
  changed = terminal_.update() || changed;
  return changed;
}

int DeskController::update_poll_interval_ms() const noexcept {
  if (chat_future_.valid() || chat_cancel_future_.valid() ||
      startup_future_.valid() ||
      browser_.busy() || intent_future_.valid() ||
      file_search_future_.valid() || file_tree_future_.valid() ||
      file_load_future_.valid() || workspace_switch_future_.valid() ||
      syntax_future_.valid() || change_set_future_.valid())
    return 16;
  if (terminal_.running())
    return 50;
  return 250;
}

std::string DeskController::escape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '\"': result += "&quot;"; break;
      case '\'': result += "&#39;"; break;
      default: result += ch; break;
    }
  }
  return result;
}

} // namespace tokmon::desk
