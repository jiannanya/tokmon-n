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
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Input.h>

#include <chrono>
#include <array>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <sstream>
#include <iostream>
#include <fstream>

namespace tokmon::desk {
namespace {

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

std::string escape_json(const std::string_view value) {
  std::string result;
  result.reserve(value.size() + 16);
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          result += "\\u00";
          result += hex[(character >> 4) & 0xf];
          result += hex[character & 0xf];
        } else {
          result += static_cast<char>(character);
        }
    }
  }
  return result;
}

std::string cbor_string(const tokmon::cbor::Value& values,
                        const std::string_view key,
                        const std::string_view fallback = {}) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::string>(value->data)
      ? std::string(value->as_string()) : std::string(fallback);
}

bool cbor_bool(const tokmon::cbor::Value& values, const std::string_view key,
               const bool fallback) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<bool>(value->data)
      ? value->as_bool() : fallback;
}

std::int64_t cbor_integer(const tokmon::cbor::Value& values,
                          const std::string_view key,
                          const std::int64_t fallback) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::int64_t>(value->data)
      ? value->as_integer() : fallback;
}

void set_cbor(tokmon::cbor::Value& values, std::string key,
              tokmon::cbor::Value value) {
  if (!values.as_map())
    values = tokmon::cbor::Value::Map{};
  (*values.as_map())[std::move(key)] = std::move(value);
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

void merge_cbor_map(tokmon::cbor::Value& destination,
                    const tokmon::cbor::Value& source) {
  if (!destination.as_map())
    destination = tokmon::cbor::Value::Map{};
  if (!source.as_map())
    return;
  for (const auto& [key, value] : *source.as_map())
    destination.as_map()->insert_or_assign(key, value);
}

tokmon::cbor::Value default_desktop_settings(const int ui_scale = 100) {
  return tokmon::cbor::object({
      {"language", "简体中文"}, {"startup", "首页"},
      {"autosave", "5 分钟"}, {"update_channel", "稳定版"},
      {"index_mode", "标准"}, {"workspace_sync", true}, {"git", true},
      {"notifications", true}, {"desktop_notifications", true},
      {"message_alerts", true}, {"quiet_hours", "关闭"},
      {"density", "舒适"}, {"font_scale", static_cast<std::int64_t>(100)},
      {"ui_scale", static_cast<std::int64_t>(ui_scale)}, {"nickname", ""},
      {"email", ""}, {"cloud_sync", false}, {"sidebar_visible", true},
      {"right_panel_visible", false},
      {"sidebar_width", static_cast<std::int64_t>(240)},
      {"right_panel_width", static_cast<std::int64_t>(440)},
      {"terminal_profile", "auto"},
      {"terminal_executable", ""}, {"terminal_arguments", ""},
      {"terminal_font_size", static_cast<std::int64_t>(13)},
      {"terminal_scrollback", static_cast<std::int64_t>(10000)}});
}

void normalize_legacy_settings(tokmon::cbor::Value& values) {
  const auto normalize_boolean = [&values](const char* key,
                                           const char* enabled,
                                           const char* disabled) {
    const auto* value = tokmon::cbor::find(values, key);
    if (value && std::holds_alternative<bool>(value->data))
      set_cbor(values, key, value->as_bool() ? enabled : disabled);
  };
  normalize_boolean("autosave", "5 分钟", "关闭");
  normalize_boolean("quiet_hours", "22:00 - 08:00", "关闭");

  const auto normalize_alias = [&values](const char* key,
                                         const std::string_view legacy,
                                         const char* current) {
    const auto* value = tokmon::cbor::find(values, key);
    if (value && value->as_string() == legacy)
      set_cbor(values, key, current);
  };
  normalize_alias("startup", "恢复上次会话", "上次打开的会话");
  normalize_alias("command_approval", "高风险操作时询问", "按需确认");
  normalize_alias("file_access", "工作区", "仅工作区");
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

std::string payload_text(const tokmon::Photon& photon) {
  if (const auto* value = tokmon::cbor::find(photon.payload, "text"))
    return std::string(value->as_string());
  return {};
}

std::uint16_t terminal_modifiers(const SDL_Keymod value) {
  std::uint16_t result = 0;
  if (value & SDL_KMOD_SHIFT) result |= terminal_shift;
  if (value & SDL_KMOD_CTRL) result |= terminal_ctrl;
  if (value & SDL_KMOD_ALT) result |= terminal_alt;
  if (value & SDL_KMOD_GUI) result |= terminal_super;
  if (value & SDL_KMOD_CAPS) result |= terminal_caps_lock;
  if (value & SDL_KMOD_NUM) result |= terminal_num_lock;
  return result;
}

TerminalKey terminal_key(const SDL_Keycode value) {
  if (value >= SDLK_A && value <= SDLK_Z)
    return static_cast<TerminalKey>(
        static_cast<int>(TerminalKey::key_a) + (value - SDLK_A));
  if (value >= SDLK_0 && value <= SDLK_9)
    return static_cast<TerminalKey>(
        static_cast<int>(TerminalKey::digit_0) + (value - SDLK_0));
  switch (value) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return TerminalKey::enter;
    case SDLK_BACKSPACE: return TerminalKey::backspace;
    case SDLK_TAB: return TerminalKey::tab;
    case SDLK_ESCAPE: return TerminalKey::escape;
    case SDLK_SPACE: return TerminalKey::space;
    case SDLK_LEFT: return TerminalKey::left;
    case SDLK_RIGHT: return TerminalKey::right;
    case SDLK_UP: return TerminalKey::up;
    case SDLK_DOWN: return TerminalKey::down;
    case SDLK_HOME: return TerminalKey::home;
    case SDLK_END: return TerminalKey::end;
    case SDLK_PAGEUP: return TerminalKey::page_up;
    case SDLK_PAGEDOWN: return TerminalKey::page_down;
    case SDLK_INSERT: return TerminalKey::insert_key;
    case SDLK_DELETE: return TerminalKey::delete_key;
    case SDLK_F1: return TerminalKey::f1;
    case SDLK_F2: return TerminalKey::f2;
    case SDLK_F3: return TerminalKey::f3;
    case SDLK_F4: return TerminalKey::f4;
    case SDLK_F5: return TerminalKey::f5;
    case SDLK_F6: return TerminalKey::f6;
    case SDLK_F7: return TerminalKey::f7;
    case SDLK_F8: return TerminalKey::f8;
    case SDLK_F9: return TerminalKey::f9;
    case SDLK_F10: return TerminalKey::f10;
    case SDLK_F11: return TerminalKey::f11;
    case SDLK_F12: return TerminalKey::f12;
    default: return TerminalKey::unidentified;
  }
}

std::string key_text(const SDL_Keycode value) {
  if (value >= SDLK_A && value <= SDLK_Z)
    return std::string(1, static_cast<char>('a' + value - SDLK_A));
  if (value >= SDLK_0 && value <= SDLK_9)
    return std::string(1, static_cast<char>('0' + value - SDLK_0));
  if (value == SDLK_SPACE)
    return " ";
  return {};
}

std::string printable_key_text(const SDL_Keycode value,
                               const SDL_Keymod modifiers) {
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
  const bool caps = (modifiers & SDL_KMOD_CAPS) != 0;
  if (value >= SDLK_A && value <= SDLK_Z) {
    char character = static_cast<char>('a' + value - SDLK_A);
    if (shift != caps)
      character = static_cast<char>(std::toupper(
          static_cast<unsigned char>(character)));
    return std::string(1, character);
  }
  if (value >= SDLK_0 && value <= SDLK_9) {
    constexpr std::string_view shifted = ")!@#$%^&*(";
    return std::string(1, shift ? shifted[static_cast<std::size_t>(
                                          value - SDLK_0)]
                                : static_cast<char>('0' + value - SDLK_0));
  }
  switch (value) {
    case SDLK_SPACE: return " ";
    case SDLK_MINUS: return shift ? "_" : "-";
    case SDLK_EQUALS: return shift ? "+" : "=";
    case SDLK_LEFTBRACKET: return shift ? "{" : "[";
    case SDLK_RIGHTBRACKET: return shift ? "}" : "]";
    case SDLK_BACKSLASH: return shift ? "|" : "\\";
    case SDLK_SEMICOLON: return shift ? ":" : ";";
    case SDLK_APOSTROPHE: return shift ? "\"" : "'";
    case SDLK_GRAVE: return shift ? "~" : "`";
    case SDLK_COMMA: return shift ? "<" : ",";
    case SDLK_PERIOD: return shift ? ">" : ".";
    case SDLK_SLASH: return shift ? "?" : "/";
    default: return {};
  }
}

} // namespace

DeskController::DeskController(Rml::ElementDocument& document, SdlPlatform& platform,
                               std::filesystem::path workspace,
                               DeskAppPaths app_paths,
                               std::filesystem::path daemon_endpoint)
    : document_(document), platform_(platform), workspace_(workspace),
      watcher_(workspace), git_(workspace),
      change_tracker_(workspace, app_paths.change_snapshots),
      browser_(app_paths.data),
      state_store_(app_paths), recovery_store_(app_paths.recovery),
      daemon_(std::move(daemon_endpoint)), navigation_(workspace) {}

DeskController::~DeskController() {
  platform_.set_raw_event_handler({});
  for (auto& tab : terminal_tabs_)
    tab->session->stop();
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

DeskController::TerminalTab& DeskController::active_terminal_tab() {
  if (terminal_tabs_.empty())
    create_terminal_tab();
  active_terminal_index_ = std::min(active_terminal_index_,
                                    terminal_tabs_.size() - 1);
  return *terminal_tabs_[active_terminal_index_];
}

TerminalSession& DeskController::terminal_session() {
  return *active_terminal_tab().session;
}

GhosttyVt& DeskController::terminal_vt() {
  return *active_terminal_tab().vt;
}

void DeskController::listen(const char* id, const char* event) {
  if (auto* element = document_.GetElementById(id))
    element->AddEventListener(event, this);
}

void DeskController::bind(const bool start_background_work) {
  for (const char* id : {"new-session-button", "settings-button", "close-settings",
                         "save-settings", "reset-settings", "new-session-overlay", "close-new-session",
                         "cancel-new-session", "confirm-new-session", "environment-toggle",
                         "thought-toggle", "workflow-toggle", "sidebar-toggle", "right-toggle",
                         "review-tab", "files-tab", "terminal-tab", "browser-tab",
                         "refresh-review", "diff-view-toggle", "branch-button",
                         "accept-agent-changes", "reject-agent-changes",
                         "commit-button", "close-commit", "cancel-commit",
                         "confirm-commit", "confirm-push", "send-button", "browser-launch",
                         "browser-go", "browser-refresh", "browser-stop",
                         "browser-back", "browser-forward", "browser-reload",
                         "browser-takeover", "browser-click", "browser-fill",
                         "title-edit", "chat-mode", "trajectory-mode", "attach-button",
                         "network-toggle", "access-button", "active-model", "effort-button",
                         "right-fullscreen", "right-collapse", "add-tab-button",
                         "save-file", "undo-file", "redo-file", "reload-file",
                         "editor-find-previous", "editor-find-next",
                         "editor-replace-one", "editor-go-line",
                         "editor-match-bracket",
                         "file-new", "folder-new", "file-rename", "file-delete",
                         "close-file-operation", "cancel-file-operation",
                         "confirm-file-operation",
                         "terminal-new-tab", "terminal-close-tab",
                         "terminal-clear-search",
                         "close-discard", "cancel-discard", "confirm-discard",
                         "cancel-terminal-paste", "confirm-terminal-paste",
                         "minimize-button", "maximize-button", "close-button"})
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
  listen("settings-search", "input");
  listen("settings-search", "change");
  listen("settings-search", "keyup");
  listen("terminal-search", "input");
  listen("terminal-search", "change");
  listen("terminal-search", "keyup");
  listen("terminal-search");
  Rml::ElementList settings_navigation;
  document_.QuerySelectorAll(settings_navigation, "[data-page]");
  for (auto* item : settings_navigation)
    item->AddEventListener("click", this);
  Rml::ElementList starter_cards;
  document_.QuerySelectorAll(starter_cards, "[data-starter]");
  for (auto* item : starter_cards)
    item->AddEventListener("click", this);
  std::string local_warning;
  settings_values_ = default_desktop_settings(
      platform_.default_content_scale_percent());
  merge_cbor_map(settings_values_, state_store_.load_settings(local_warning));
  normalize_legacy_settings(settings_values_);
  sidebar_width_ = static_cast<int>(std::clamp<std::int64_t>(
      cbor_integer(settings_values_, "sidebar_width", 240), 196, 420));
  right_panel_width_ = static_cast<int>(std::clamp<std::int64_t>(
      cbor_integer(settings_values_, "right_panel_width", 440), 214, 720));
  sidebar_visible_ = cbor_bool(settings_values_, "sidebar_visible", true);
  // Match the legacy desktop exactly: the right panel is session-local and
  // starts collapsed even when an old settings snapshot says it was open.
  right_panel_visible_ = false;
  set_cbor(settings_values_, "right_panel_visible", false);
  apply_panel_layout();
  if (!local_warning.empty())
    text(document_, "settings-status", escape(local_warning));
  std::string navigation_warning;
  const auto local_navigation = state_store_.load_navigation(navigation_warning);
  if (local_navigation.as_array() && !local_navigation.as_array()->empty()) {
    std::string navigation_error;
    if (!navigation_.load(local_navigation, navigation_error))
      navigation_warning = "本地导航状态无效：" + navigation_error;
  }
  navigation_loaded_ = true;
  if (!navigation_warning.empty())
    text(document_, "daemon-status", escape(navigation_warning));

  const auto workspace_name = workspace_.root().filename().string();
  create_terminal_tab();
  text(document_, "starter-workspace-name", escape(workspace_name));
  text(document_, "composer-workspace-name", escape(workspace_name));
  text(document_, "workspace-path", escape(workspace_.root().string()));
  if (auto* branch = document_.QuerySelector(".workspace-context .branch"))
    branch->SetInnerRML("正在读取 Git…");
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
    text(document_, "session-title", escape(selected_navigation->title));
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

void DeskController::prepare_legacy_three_pane_contract() {
  sidebar_width_ = 240;
  right_panel_width_ = 440;
  sidebar_visible_ = true;
  right_panel_visible_ = true;
  apply_panel_layout();
}

void DeskController::toggle_hidden(const char* id) {
  if (auto* element = document_.GetElementById(id))
    element->SetClass("hidden", !element->IsClassSet("hidden"));
}

void DeskController::apply_panel_layout() {
  const auto pixels = [](const int value) { return std::to_string(value) + "px"; };
  if (auto* shell = document_.GetElementById("app-shell")) {
    shell->SetClass("sidebar-hidden", !sidebar_visible_);
    shell->SetClass("right-hidden", !right_panel_visible_);
  }
  if (auto* sidebar = document_.GetElementById("sidebar")) {
    sidebar->SetClass("hidden", !sidebar_visible_);
    sidebar->SetProperty("width", pixels(sidebar_width_));
  }
  if (auto* panel = document_.GetElementById("right-panel")) {
    panel->SetClass("hidden", !right_panel_visible_);
    panel->SetProperty("width", pixels(right_panel_width_));
  }
  if (auto* workspace = document_.GetElementById("workspace")) {
    workspace->SetProperty("left", pixels(sidebar_visible_ ? sidebar_width_ : 0));
    workspace->SetProperty("right", pixels(right_panel_visible_ ? right_panel_width_ : 0));
  }
  if (auto* toggle = document_.GetElementById("sidebar-toggle"))
    toggle->SetProperty("left", pixels(sidebar_visible_ ? sidebar_width_ - 8 : 6));
  if (auto* toggle = document_.GetElementById("right-toggle"))
    toggle->SetProperty("right", pixels(right_panel_visible_ ? right_panel_width_ - 8 : 110));
  if (auto* divider = document_.GetElementById("sidebar-resizer"))
    divider->SetProperty("left", pixels(std::max(0, sidebar_width_ - 5)));
  if (auto* divider = document_.GetElementById("right-resizer"))
    divider->SetProperty("right", pixels(std::max(0, right_panel_width_ - 4)));
}

void DeskController::set_sidebar_visible(const bool visible) {
  sidebar_visible_ = visible;
  set_cbor(settings_values_, "sidebar_visible", visible);
  apply_panel_layout();
}

void DeskController::set_right_panel_visible(const bool visible) {
  right_panel_visible_ = visible;
  // This value is kept in-memory for settings rendering, but the persisted
  // startup behavior remains collapsed for parity with tokmon-desktop.
  set_cbor(settings_values_, "right_panel_visible", visible);
  apply_panel_layout();
}

bool DeskController::save_local_settings(std::string& error) {
  set_cbor(settings_values_, "sidebar_visible", sidebar_visible_);
  set_cbor(settings_values_, "right_panel_visible", false);
  set_cbor(settings_values_, "sidebar_width",
           static_cast<std::int64_t>(sidebar_width_));
  set_cbor(settings_values_, "right_panel_width",
           static_cast<std::int64_t>(right_panel_width_));
  return state_store_.save_settings(select_cbor_keys(settings_values_, {
      "language", "startup", "autosave", "update_channel", "index_mode",
      "workspace_sync", "git", "notifications", "desktop_notifications",
      "message_alerts", "quiet_hours", "density", "font_scale", "ui_scale",
      "nickname", "email", "cloud_sync", "sidebar_visible",
      "right_panel_visible", "sidebar_width", "right_panel_width",
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

void DeskController::show_right_view(const char* id) {
  if (std::string_view(id) != "terminal-view" &&
      heavy_focus_ != HeavyFocus::none) {
    heavy_focus_ = HeavyFocus::none;
    pending_terminal_keydown_text_.clear();
    platform_.DeactivateKeyboard();
  }
  set_right_panel_visible(true);
  for (const char* view : {"review-view", "files-view", "terminal-view", "browser-view"})
    if (auto* element = document_.GetElementById(view))
      element->SetClass("hidden", std::string_view(view) != id);
  const std::pair<const char*, const char*> tabs[] = {
      {"review-tab", "review-view"}, {"files-tab", "files-view"},
      {"terminal-tab", "terminal-view"}, {"browser-tab", "browser-view"}};
  for (const auto& [tab, view] : tabs)
    if (auto* element = document_.GetElementById(tab))
      element->SetClass("active", std::string_view(view) == id);
  const std::pair<const char*, const char*> labels[] = {
      {"review-view", "审查"}, {"files-view", "文件"},
      {"terminal-view", "终端"}, {"browser-view", "浏览器"}};
  for (const auto& [view, label] : labels)
    if (std::string_view(view) == id)
      text(document_, "active-right-label", label);
  text(document_, "active-right-shortcut",
       std::string_view(id) == "review-view" ? "Ctrl+Shift+G" :
       std::string_view(id) == "files-view" ? "Ctrl+P" : "");
}

void DeskController::refresh_review() {
  if (review_future_.valid()) {
    review_refresh_queued_ = true;
    return;
  }
  if (auto* empty = document_.GetElementById("review-empty"))
    empty->SetInnerRML("<strong>正在刷新更改…</strong><span>Git 状态在工作线程读取</span>");
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
  if (auto* branch = document_.GetElementById("branch-button"))
    branch->SetInnerRML("⑂ " + escape(snapshot.branch.empty() ? "workspace" : snapshot.branch) + "⌄");
  if (auto* count = document_.QuerySelector(".count-pill"))
    count->SetInnerRML(std::to_string(snapshot.files.size()));
  auto* empty = document_.GetElementById("review-empty");
  if (!empty) return;
  if (!snapshot.repository) {
    empty->SetInnerRML("<strong>当前工作区不是 Git 仓库</strong><span>" + escape(snapshot.error) + "</span>");
    return;
  }
  if (snapshot.files.empty()) {
    empty->SetInnerRML("<strong>没有待审查的更改</strong><span>工作区修改会在这里显示</span>");
    return;
  }
  std::ostringstream rml;
  rml << "<div class='change-list'>";
  for (const auto& file : snapshot.files) {
    const bool worktree = file.worktree_status != ' ';
    const bool staged = file.index_status != ' ';
    rml << "<div class='change-row'><button class='change-open' data-diff-path='"
        << escape(file.path) << "' data-diff-staged='" << (worktree ? "0" : "1")
        << "'><code>" << file.index_status << file.worktree_status
        << "</code><span>" << escape(file.path) << "</span></button>";
    if (worktree)
      rml << "<button class='change-action' data-git-action='stage-file' data-git-path='"
          << escape(file.path) << "'>＋</button>";
    if (staged)
      rml << "<button class='change-action' data-git-action='unstage-file' data-git-path='"
          << escape(file.path) << "'>−</button>";
    if (worktree)
      rml << "<button class='change-action danger' data-git-action='discard-file' data-git-path='"
          << escape(file.path) << "'>↶</button>";
    rml << "</div>";
  }
  rml << "</div>";
  empty->SetInnerRML(rml.str());
  Rml::ElementList rows;
  empty->QuerySelectorAll(rows, "[data-diff-path]");
  for (auto* row : rows)
    row->AddEventListener("click", this);
  Rml::ElementList actions;
  empty->QuerySelectorAll(actions, "[data-git-action]");
  for (auto* action : actions)
    action->AddEventListener("click", this);
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
    auto* menu = document_.GetElementById("branch-menu");
    if (menu) {
      std::ostringstream rml;
      rml << "<strong>切换 Git 分支</strong>";
      for (const auto& branch : result.branches)
        rml << "<button data-git-branch='" << escape(branch) << "'"
            << (branch == result.snapshot.branch ? " class='active'" : "")
            << ">" << (branch == result.snapshot.branch ? "✓ " : "")
            << escape(branch) << "</button>";
      if (result.branches.empty())
        rml << "<span>" << escape(result.error.empty() ? "没有本地分支"
                                                        : result.error)
            << "</span>";
      menu->SetInnerRML(rml.str());
      Rml::ElementList choices;
      menu->QuerySelectorAll(choices, "[data-git-branch]");
      for (auto* choice : choices)
        choice->AddEventListener("click", this);
    }
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
    auto* view = document_.GetElementById("review-diff");
    if (view && result.diff) {
      std::ostringstream rml;
      rml << "<div class='diff-header'><button id='close-diff' title='返回更改列表'>"
             "‹ 更改列表</button><strong>" << escape(result.path)
          << "</strong><span>" << (result.staged ? "已暂存" : "工作区")
          << " · " << result.diff->hunks.size() << " 个修改块 · "
          << result.diff->patch.size() << " bytes</span></div>"
          << "<div class='diff-hunk-actions'>";
      for (const auto& hunk : result.diff->hunks) {
        rml << "<button data-git-action='"
            << (result.staged ? "unstage-hunk" : "stage-hunk")
            << "' data-git-path='" << escape(result.path)
            << "' data-git-hunk='" << hunk.index << "'>"
            << escape(hunk.header) << " · "
            << (result.staged ? "取消暂存" : "暂存") << "</button>";
        if (!result.staged)
          rml << "<button class='danger' data-git-action='discard-hunk' "
                 "data-git-path='" << escape(result.path)
              << "' data-git-hunk='" << hunk.index
              << "'>放弃修改块</button>";
      }
      rml << "</div><tokmon-diff-surface id='diff-surface' class='diff-surface' "
             "tab-index='0'></tokmon-diff-surface>";
      view->SetInnerRML(rml.str());
      view->SetClass("virtual", true);
      view->SetClass("hidden", false);
      if (auto* surface = dynamic_cast<ElementDiffSurface*>(
              document_.GetElementById("diff-surface"))) {
        surface->set_diff(*result.diff);
        surface->set_split_view(diff_split_view_);
        surface->AddEventListener("mousescroll", this);
      }
      Rml::ElementList actions;
      view->QuerySelectorAll(actions, "[data-git-action]");
      for (auto* action : actions)
        action->AddEventListener("click", this);
      listen("close-diff");
    } else if (view) {
      view->SetInnerRML("<div class='diff-error'>" + escape(result.error) +
                        "</div>");
    }
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
      if (auto* view = document_.GetElementById("review-diff")) {
        view->SetInnerRML({});
        view->SetClass("hidden", true);
      }
    }
  } else if (result.kind == ReviewTaskResult::Kind::commit) {
    if (!result.success) {
      text(document_, "review-empty", escape(result.error));
    } else {
      if (auto* overlay = document_.GetElementById("commit-overlay"))
        overlay->SetClass("hidden", true);
      if (auto* diff = document_.GetElementById("review-diff"))
        diff->SetClass("hidden", true);
      show_toast(result.push_requested ? "已提交并推送" : "已创建提交");
      render_review_snapshot(result.snapshot);
    }
  }
  if (result.snapshot.repository) {
    if (auto* branch = document_.QuerySelector(".workspace-context .branch"))
      branch->SetInnerRML(escape(result.snapshot.branch.empty()
                                     ? "无 Git" : result.snapshot.branch));
  } else if (result.kind == ReviewTaskResult::Kind::status) {
    if (auto* branch = document_.QuerySelector(".workspace-context .branch"))
      branch->SetInnerRML("无 Git");
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
  menu->SetInnerRML("<strong>正在读取分支…</strong>");
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
  const auto branch = element.GetAttribute<Rml::String>("data-git-branch", "");
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
  if (auto* menu = document_.GetElementById("branch-menu"))
    menu->SetInnerRML("<strong>正在切换分支…</strong>");
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

void DeskController::start_terminal() {
  auto& tab = active_terminal_tab();
  if (tab.started && tab.session->running()) return;
  if (!tab.launch_error.empty() || tab.launch.executable.empty()) {
    text(document_, "terminal-status",
         escape("不可用：" + (tab.launch_error.empty()
                    ? std::string("未配置可执行文件") : tab.launch_error)));
    return;
  }
  std::string error;
  auto* session = tab.session.get();
  tab.vt->set_response_sink([session](const std::string_view response) {
    std::string ignored;
    (void)session->write(response, ignored);
  });
  (void)tab.vt->resize(tab.columns, tab.rows, tab.cell_width, tab.cell_height);
  tab.started = tab.session->start_profile(tab.launch, workspace_.root(),
                                           tab.columns, tab.rows, error);
  if (!tab.started)
    text(document_, "terminal-status", escape("不可用：" + error));
  else {
    text(document_, "terminal-status", "正在运行");
    resize_terminal_to_surface();
  }
}

void DeskController::resize_terminal_to_surface() {
  auto* surface = dynamic_cast<ElementTerminal*>(
      document_.GetElementById("terminal-surface"));
  if (!surface)
    return;
  auto& tab = active_terminal_tab();
  const auto columns = std::clamp(
      static_cast<int>(surface->GetClientWidth() /
                       static_cast<float>(tab.cell_width)), 20, 400);
  const auto rows = std::clamp(
      static_cast<int>(surface->GetClientHeight() /
                       static_cast<float>(tab.cell_height)), 5, 200);
  if (columns == tab.columns && rows == tab.rows)
    return;
  tab.columns = columns;
  tab.rows = rows;
  std::string error;
  if (tab.started && !tab.session->resize(columns, rows, error))
    tab.vt->append("\r\n" + error + "\r\n");
  (void)tab.vt->resize(columns, rows, tab.cell_width, tab.cell_height);
  surface->set_snapshot(tab.vt->render_snapshot());
}

void DeskController::paste_terminal(const bool allow_unsafe) {
  if (!active_terminal_tab().started)
    start_terminal();
  if (pending_terminal_paste_.empty()) {
    Rml::String clipboard;
    platform_.GetClipboardText(clipboard);
    pending_terminal_paste_ = clipboard;
  }
  if (pending_terminal_paste_.empty())
    return;
  const auto result = terminal_vt().paste(pending_terminal_paste_, allow_unsafe);
  if (result == TerminalPasteResult::unsafe) {
    if (auto* overlay = document_.GetElementById("terminal-paste-overlay"))
      overlay->SetClass("hidden", false);
    return;
  }
  if (result == TerminalPasteResult::failed)
    terminal_vt().append("\r\nTerminal paste failed\r\n");
  pending_terminal_paste_.clear();
  if (auto* overlay = document_.GetElementById("terminal-paste-overlay"))
    overlay->SetClass("hidden", true);
}

void DeskController::send_terminal_input() {
  auto* input = control(document_, "terminal-input");
  if (!input || input->GetValue().empty()) return;
  std::string error;
  const auto command = input->GetValue() + "\r\n";
  if (!terminal_session().write(command, error))
    terminal_vt().append("\n" + error + "\n");
  input->SetValue("");
}

void DeskController::search_terminal() {
  auto* surface = dynamic_cast<ElementTerminal*>(
      document_.GetElementById("terminal-surface"));
  auto* input = control(document_, "terminal-search");
  if (!surface || !input)
    return;
  surface->set_search(input->GetValue());
  text(document_, "terminal-search-count",
       input->GetValue().empty()
           ? std::string{}
           : std::to_string(surface->search_match_count()) + " 个");
}

void DeskController::create_terminal_tab() {
  auto tab = std::make_unique<TerminalTab>();
  tab->id = "terminal-" + std::to_string(next_terminal_id_++);
  const auto profile_id = cbor_string(settings_values_, "terminal_profile", "auto");
  tab->font_size = static_cast<int>(std::clamp<std::int64_t>(
      cbor_integer(settings_values_, "terminal_font_size", 13), 9, 24));
  tab->cell_width = std::max(6, static_cast<int>(std::lround(
      static_cast<double>(tab->font_size) * 0.62)));
  tab->cell_height = tab->font_size + 4;
  if (profile_id == "custom") {
    const auto executable = std::filesystem::path(
        cbor_string(settings_values_, "terminal_executable"));
    std::error_code path_error;
    if (executable.empty() || !std::filesystem::is_regular_file(
                                  executable, path_error)) {
      tab->launch_error = "自定义终端可执行文件不存在或不是普通文件";
    } else {
      std::string argument_error;
      const auto arguments = parse_terminal_arguments(
          cbor_string(settings_values_, "terminal_arguments"), argument_error);
      if (!arguments)
        tab->launch_error = argument_error;
      else
        tab->launch = {executable, *arguments};
    }
    tab->title = "自定义 " + std::to_string(next_terminal_id_ - 1);
  } else {
    const auto profile = resolve_terminal_profile(profile_id);
    tab->title = (profile ? profile->label : "Shell") + " " +
                 std::to_string(next_terminal_id_ - 1);
    if (profile)
      tab->launch = {profile->executable, profile->arguments};
    else
      tab->launch_error = "没有可用的系统 Shell";
  }
  tab->session = std::make_unique<TerminalSession>();
  tab->vt = std::make_unique<GhosttyVt>(
      100, 28, static_cast<std::size_t>(std::clamp<std::int64_t>(
                   cbor_integer(settings_values_, "terminal_scrollback", 10000),
                   1000, 100000)));
  terminal_tabs_.push_back(std::move(tab));
  active_terminal_index_ = terminal_tabs_.size() - 1;
  render_terminal_tabs();
  if (auto* surface = document_.GetElementById("terminal-surface"))
    surface->SetProperty("font-size",
        std::to_string(active_terminal_tab().font_size) + "px");
}

void DeskController::close_terminal_tab() {
  if (terminal_tabs_.empty())
    return;
  terminal_tabs_[active_terminal_index_]->session->stop();
  terminal_tabs_.erase(terminal_tabs_.begin() +
                       static_cast<std::ptrdiff_t>(active_terminal_index_));
  if (terminal_tabs_.empty())
    create_terminal_tab();
  else {
    active_terminal_index_ = std::min(active_terminal_index_,
                                      terminal_tabs_.size() - 1);
    render_terminal_tabs();
    auto& tab = active_terminal_tab();
    if (auto* surface = dynamic_cast<ElementTerminal*>(
            document_.GetElementById("terminal-surface"))) {
      surface->SetProperty("font-size", std::to_string(tab.font_size) + "px");
      surface->set_snapshot(tab.vt->render_snapshot());
    }
    text(document_, "terminal-status", tab.started ? "正在运行" : "未启动");
  }
}

void DeskController::select_terminal_tab(const std::string_view id) {
  const auto found = std::ranges::find_if(terminal_tabs_, [&](const auto& tab) {
    return tab->id == id;
  });
  if (found == terminal_tabs_.end())
    return;
  active_terminal_index_ = static_cast<std::size_t>(
      std::distance(terminal_tabs_.begin(), found));
  render_terminal_tabs();
  auto& tab = active_terminal_tab();
  if (auto* surface = dynamic_cast<ElementTerminal*>(
          document_.GetElementById("terminal-surface"))) {
    surface->SetProperty("font-size", std::to_string(tab.font_size) + "px");
    surface->set_snapshot(tab.vt->render_snapshot());
  }
  text(document_, "terminal-status", tab.started ? "正在运行" : "未启动");
  search_terminal();
}

void DeskController::render_terminal_tabs() {
  auto* container = document_.GetElementById("terminal-tabs");
  if (!container)
    return;
  std::ostringstream rml;
  for (std::size_t index = 0; index < terminal_tabs_.size(); ++index)
    rml << "<button class='" << (index == active_terminal_index_ ? "active" : "")
        << "' data-terminal-tab='" << terminal_tabs_[index]->id << "'>"
        << escape(terminal_tabs_[index]->title) << "</button>";
  container->SetInnerRML(rml.str());
  Rml::ElementList buttons;
  container->QuerySelectorAll(buttons, "[data-terminal-tab]");
  for (auto* button : buttons)
    button->AddEventListener("click", this);
}

void DeskController::send_message() {
  auto* composer = control(document_, "composer");
  auto* conversation = document_.GetElementById("conversation");
  if (!composer || !conversation || composer->GetValue().empty() ||
      chat_future_.valid() || change_set_future_.valid()) return;
  const std::string prompt = composer->GetValue();
  if (auto* selected = navigation_.selected(); selected &&
      selected->kind == "session" && !selected->title_manual) {
    const auto title = automatic_title(prompt);
    if (navigation_.rename_selected(title, false)) {
      text(document_, "session-title", escape(title));
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
  const auto provider = selected_provider_;
  const auto model = selected_model_;
  const auto effort = selected_effort_;
  const auto access = selected_access_;
  const auto change_run = tokmon::make_id("desk-agent-run");
  chat_future_ = std::async(std::launch::async,
      [this, prompt, ray, provider, model, effort, access, change_run] {
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
            });
        if (tracking)
          task.changes = change_tracker_.finish(task.tracker_error);
        return task;
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
  constexpr std::size_t kWindowTurns = 80;
  constexpr std::size_t kOverscanTurns = 12;
  constexpr std::size_t kEstimatedTurnHeight = 210;
  const float previous_scroll = conversation->GetScrollTop();
  const bool followed_tail = conversation->GetScrollHeight() <= 0.f ||
      previous_scroll + conversation->GetClientHeight() >=
          conversation->GetScrollHeight() - 36.f;
  struct Turn {
    std::string user;
    std::string reasoning;
    std::string assistant;
    std::vector<std::pair<std::string, std::string>> workflow;
    bool failed{false};
  };
  std::vector<Turn> turns;
  for (const auto& photon : photons_) {
    if (photon.kind == "user.input" || photon.kind == "user.message") {
      turns.push_back({});
      turns.back().user = payload_text(photon);
      continue;
    }
    if (turns.empty())
      turns.push_back({});
    auto& turn = turns.back();
    if (photon.kind == "model.reasoning-chunk")
      turn.reasoning += payload_text(photon);
    else if (photon.kind == "model.content-chunk")
      turn.assistant += payload_text(photon);
    else if (photon.kind == "assistant.message") {
      const auto final_text = payload_text(photon);
      if (!final_text.empty())
        turn.assistant = final_text;
    } else if (photon.kind == "act.failed" || photon.kind == "model.failed") {
      turn.failed = true;
      const auto* detail = tokmon::cbor::find(photon.payload, "detail");
      if (!detail)
        detail = tokmon::cbor::find(photon.payload, "error");
      turn.workflow.emplace_back("执行失败",
          detail ? std::string(detail->as_string()) : "请检查 Photon 轨迹");
    } else if (photon.kind.starts_with("tool.") ||
               photon.kind.starts_with("fs.") ||
               photon.kind.starts_with("task.")) {
      auto detail = payload_text(photon);
      if (detail.empty())
        detail = tokmon::cbor::diagnostic(photon.payload);
      if (detail.size() > 400)
        detail.resize(400);
      turn.workflow.emplace_back(photon.kind, std::move(detail));
    }
  }
  conversation_total_turns_ = turns.size();
  const auto maximum_start = turns.size() > kWindowTurns
      ? turns.size() - kWindowTurns : 0;
  if (followed_tail)
    conversation_window_start_ = maximum_start;
  else
    conversation_window_start_ = std::min(conversation_window_start_, maximum_start);
  const auto first = conversation_window_start_ > kOverscanTurns
      ? conversation_window_start_ - kOverscanTurns : 0;
  const auto last = std::min(turns.size(),
      conversation_window_start_ + kWindowTurns + kOverscanTurns);
  std::ostringstream rml;
  if (first > 0)
    rml << "<div class='conversation-spacer' style='height: "
        << static_cast<std::size_t>(first * kEstimatedTurnHeight)
        << "px'></div>";
  for (std::size_t index = first; index < last; ++index) {
    const auto& turn = turns[index];
    if (!turn.user.empty())
      rml << "<div class='user-message-row'><article class='message user-message'>"
          << markdown_to_safe_rml(markdown_.parse(turn.user))
          << "</article></div>";
    if (!turn.reasoning.empty())
      rml << "<article class='thought-card'><div class='card-title'><span>●</span> 思考过程</div>"
          << "<div class='thought-content'>"
          << markdown_to_safe_rml(markdown_.parse(turn.reasoning))
          << "</div></article>";
    if (!turn.workflow.empty()) {
      rml << "<article class='workflow-card'><div class='card-title'><span class='status-dot'></span> 工作流执行"
          << "<span class='complete-pill'>" << turn.workflow.size()
          << " 项</span></div><div class='workflow-content'>";
      for (const auto& [kind, detail] : turn.workflow)
        rml << "<div class='workflow-step" << (turn.failed ? " failed" : "")
            << "'><span class='step-icon'>" << (turn.failed ? "!" : "✓")
            << "</span><div><strong>" << escape(kind) << "</strong><small>"
            << escape(detail) << "</small></div></div>";
      rml << "</div></article>";
    }
    if (!turn.assistant.empty())
      rml << "<article class='message assistant-message'>"
          << markdown_to_safe_rml(markdown_.parse(turn.assistant))
          << "</article>";
  }
  if (last < turns.size())
    rml << "<div class='conversation-spacer' style='height: "
        << static_cast<std::size_t>((turns.size() - last) * kEstimatedTurnHeight)
        << "px'></div>";
  conversation->SetInnerRML(rml.str());
  const bool empty = turns.empty() ||
      std::ranges::all_of(turns, [](const Turn& turn) {
        return turn.user.empty() && turn.reasoning.empty() &&
               turn.assistant.empty() && turn.workflow.empty();
      });
  conversation->SetClass("hidden", empty);
  if (auto* initial = document_.GetElementById("initial-session"))
    initial->SetClass("hidden", !empty);
  if (!empty && followed_tail)
    conversation->SetScrollTop(conversation->GetScrollHeight());
  else if (!empty)
    conversation->SetScrollTop(previous_scroll);
  render_trajectory();
}

void DeskController::render_trajectory() {
  auto* trajectory = document_.GetElementById("trajectory");
  if (!trajectory)
    return;
  if (photons_.empty()) {
    trajectory->SetInnerRML(
        "<div class='trajectory-empty'><strong>尚无执行轨迹</strong>"
        "<span>提交请求后，真实 Photon 事件会在这里按因果顺序显示</span></div>");
    return;
  }
  std::ostringstream rml;
  rml << "<div class='trajectory-summary'><div><strong>"
      << photons_.size() << "</strong><span>Photon</span></div><div><strong>"
      << escape(active_ray_.empty() ? "未绑定" : active_ray_)
      << "</strong><span>当前 Ray</span></div><div><strong>"
      << snow_cursor_ << "</strong><span>Snow Cursor</span></div>"
      << "<button id='export-trajectory'><svg src='../../assets/figma/icon-42.svg'></svg>导出</button></div>"
      << "<div class='trajectory-list'>";
  constexpr std::size_t kMaximumRenderedPhotons = 200;
  const auto first = photons_.size() > kMaximumRenderedPhotons
      ? photons_.size() - kMaximumRenderedPhotons : 0;
  if (first > 0)
    rml << "<div class='trajectory-window-notice'>较早的 " << first
        << " 个 Photon 已虚拟化；滚动会话或导出轨迹可查看完整历史</div>";
  for (auto iterator = photons_.begin() + static_cast<std::ptrdiff_t>(first);
       iterator != photons_.end(); ++iterator) {
    const auto& photon = *iterator;
    auto detail = tokmon::cbor::diagnostic(photon.payload);
    if (detail.size() > 700)
      detail.resize(700);
    rml << "<article class='trajectory-row'><span class='trajectory-sequence'>#"
        << photon.sequence << "</span><span class='trajectory-dot'></span><div>"
        << "<strong>" << escape(photon.kind) << "</strong><small>"
        << escape(photon.schema) << " · " << photon.committed_at_ms
        << "</small><code>" << escape(detail) << "</code></div></article>";
  }
  rml << "</div>";
  trajectory->SetInnerRML(rml.str());
  listen("export-trajectory");
}

void DeskController::export_trajectory() {
  if (photons_.empty()) {
    show_toast("当前没有可导出的轨迹");
    return;
  }
  std::error_code error;
  const auto directory = workspace_.root() / "exports";
  std::filesystem::create_directories(directory, error);
  if (error) {
    show_toast("导出失败：" + error.message());
    return;
  }
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto path = directory /
      ("tokmon-trace-" + std::to_string(milliseconds) + ".json");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    show_toast("导出失败：无法创建文件");
    return;
  }
  output << "{\n  \"ray\": \"" << escape_json(active_ray_)
         << "\",\n  \"snowCursor\": " << snow_cursor_
         << ",\n  \"events\": [\n";
  for (std::size_t index = 0; index < photons_.size(); ++index) {
    const auto& photon = photons_[index];
    output << "    {\"sequence\": " << photon.sequence
           << ", \"kind\": \"" << escape_json(photon.kind)
           << "\", \"schema\": \"" << escape_json(photon.schema)
           << "\", \"committedAtMs\": " << photon.committed_at_ms
           << ", \"payload\": \""
           << escape_json(tokmon::cbor::diagnostic(photon.payload)) << "\"}"
           << (index + 1 == photons_.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  if (!output) {
    show_toast("导出失败：写入未完成");
    return;
  }
  show_toast("轨迹已导出到 " + path.generic_string());
}

void DeskController::seed_acceptance_conversation(const std::size_t turns) {
  photons_.clear();
  photons_.reserve(turns);
  for (std::size_t index = 0; index < turns; ++index) {
    photons_.push_back(tokmon::Photon{
        .sequence = index + 1,
        .id = "acceptance-photon-" + std::to_string(index),
        .ray = "acceptance-ray",
        .kind = "user.message",
        .schema = "tokmon.user.message.v1",
        .payload = tokmon::cbor::object(
            {{"text", "验收会话消息 " + std::to_string(index)}})});
  }
  snow_cursor_ = turns;
  active_ray_ = "acceptance-ray";
  conversation_window_start_ = 0;
  conversation_dirty_ = true;
  render_conversation();
}

void DeskController::render_navigation() {
  auto* tree = document_.GetElementById("navigation-tree");
  if (!tree)
    return;
  const auto* query = control(document_, "navigation-search");
  const auto visible = navigation_.visible(query ? query->GetValue() : "");
  std::ostringstream rml;
  for (const auto index : visible) {
    const auto& item = navigation_.items()[index];
    rml << "<button class='tree-row " << item.kind
        << (item.selected ? " selected" : "")
        << "' style='padding-left:" << (10 + item.indent * 18)
        << "px' data-navigation-id='" << escape(item.id) << "'>"
        << "<span class='chevron'>"
        << (item.kind == "session" ? "" : item.expanded ? "⌄" : "›")
        << "</span><svg src='../../assets/figma/"
        << (item.kind == "session" ? "icon-23.svg" : "icon-18.svg")
        << "'></svg><span>" << escape(item.title)
        << "</span></button>";
  }
  if (visible.empty())
    rml << "<div class='navigation-empty'>没有匹配的会话、项目或分组</div>";
  tree->SetInnerRML(rml.str());
  Rml::ElementList rows;
  tree->QuerySelectorAll(rows, "[data-navigation-id]");
  for (auto* row : rows)
    row->AddEventListener("click", this);

  if (auto* projects = control(document_, "new-session-project")) {
    std::ostringstream options;
    const auto active_workspace = navigation_.selected_workspace();
    for (const auto& item : navigation_.items())
      if (item.kind == "project")
        options << "<option value='" << escape(item.id) << "'>"
                << escape(item.title) << "</option>";
    projects->SetInnerRML(options.str());
    for (const auto& item : navigation_.items())
      if (item.kind == "project" &&
          same_workspace(item.workspace.empty() ? active_workspace
                                                : item.workspace,
                         active_workspace)) {
        projects->SetValue(item.id);
        break;
      }
  }
}

void DeskController::apply_settings(const tokmon::cbor::Value& payload) {
  const auto* values = tokmon::cbor::find(payload, "values");
  if (!values || !values->as_map())
    return;
  // Only shared agent/project settings are imported from the daemon. Desktop
  // layout, navigation, appearance and terminal preferences remain local.
  merge_cbor_map(settings_values_, select_cbor_keys(*values, {
      "main_model", "reasoning", "access_mode", "file_access",
      "command_approval", "network", "high_risk_confirmation"}));
  normalize_legacy_settings(settings_values_);
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
    text(document_, "session-title", escape(selected->title));
  }
  selected_model_ = cbor_string(*values, "main_model", selected_model_);
  selected_effort_ = cbor_string(*values, "reasoning", selected_effort_);
  selected_access_ = cbor_string(*values, "access_mode", selected_access_);
  text(document_, "active-model",
       escape(selected_model_.empty() ? "选择模型⌄" : selected_model_ + "⌄"));
  text(document_, "effort-button", "♙ " + escape(selected_effort_) + "⌄");
  text(document_, "access-button",
       selected_access_ == "full" ? "♢ 完全访问⌄" : "♢ 受限访问⌄");
  render_navigation();
  render_settings_page(settings_page_);
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
  const auto id = element.GetAttribute<Rml::String>("data-navigation-id", "");
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
    text(document_, "session-title", escape(selected->title));
    photons_.clear();
    conversation_dirty_ = true;
    request_selected_surface();
  } else if (selected && selected->kind == "project") {
    auto& created = navigation_.create_session("新会话");
    active_ray_.clear();
    photons_.clear();
    conversation_dirty_ = true;
    text(document_, "session-title", escape(created.title));
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
  current_diff_path_.clear();
  photons_.clear();
  active_ray_.clear();
  snow_cursor_ = 0;
  startup_loaded_ = false;
  conversation_dirty_ = true;
  for (auto& tab : terminal_tabs_)
    tab->session->stop();
  terminal_tabs_.clear();
  active_terminal_index_ = 0;
  create_terminal_tab();
  const auto name = workspace_.root().filename().string();
  text(document_, "starter-workspace-name", escape(name));
  text(document_, "composer-workspace-name", escape(name));
  text(document_, "workspace-path", escape(workspace_.root().string()));
  text(document_, "file-path", "选择文件以预览或编辑");
  if (auto* editor = dynamic_cast<ElementCodeSurface*>(
          document_.GetElementById("file-preview")))
    editor->set_document({}, {}, 0, false);
  if (auto* branch = document_.QuerySelector(".workspace-context .branch"))
    branch->SetInnerRML("正在读取 Git…");
  text(document_, "daemon-status",
       result.started ? "目标工作空间后台服务已启动" : "目标工作空间已连接");
  refresh_files();
  refresh_review();
  if (create_session_after_workspace_switch_) {
    create_session_after_workspace_switch_ = false;
    (void)navigation_.create_session("新会话");
    text(document_, "session-title", "新会话");
    render_navigation();
    save_navigation();
  } else if (const auto* selected = navigation_.selected();
             selected && selected->kind == "session") {
    active_ray_ = selected->ray;
    text(document_, "session-title", escape(selected->title));
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
  const auto kind = card.GetAttribute<Rml::String>("data-starter", "");
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
}

void DeskController::render_slash_commands() {
  auto* composer = control(document_, "composer");
  auto* popover = document_.GetElementById("composer-popover");
  if (!composer || !popover)
    return;
  const std::string value = composer->GetValue();
  if (value.empty() || value.front() != '/' || value.find_first_of(" \t\r\n") !=
          std::string::npos) {
    slash_command_count_ = 0;
    slash_command_index_ = 0;
    popover->SetClass("hidden", true);
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
  if (matches.empty()) {
    slash_command_index_ = 0;
    popover->SetInnerRML(
        "<div class='popover-card choice-popover'><strong>命令面板</strong>"
        "<span class='popover-empty'>没有匹配命令</span></div>");
    popover->SetClass("hidden", false);
    return;
  }
  slash_command_index_ = std::min(slash_command_index_, matches.size() - 1);
  std::ostringstream markup;
  markup << "<div class='popover-card choice-popover'><strong>命令面板</strong>";
  for (std::size_t index = 0; index < matches.size(); ++index)
    markup << "<button class='" << (index == slash_command_index_ ? "selected" : "")
           << "' data-command='" << matches[index].first << "'>"
           << matches[index].first << "<small>" << matches[index].second
           << "</small></button>";
  markup << "</div>";
  popover->SetInnerRML(markup.str());
  popover->SetClass("hidden", false);
  Rml::ElementList commands;
  popover->QuerySelectorAll(commands, "[data-command]");
  for (auto* command : commands)
    command->AddEventListener("click", this);
}

void DeskController::select_slash_command(const std::size_t index) {
  auto* popover = document_.GetElementById("composer-popover");
  auto* composer = control(document_, "composer");
  if (!popover || !composer)
    return;
  Rml::ElementList commands;
  popover->QuerySelectorAll(commands, "[data-command]");
  if (commands.empty())
    return;
  const auto selected = std::min(index, commands.size() - 1);
  const auto command = commands[selected]->GetAttribute<Rml::String>(
      "data-command", "");
  if (!command.empty()) {
    composer->SetValue(command + " ");
    update_composer_placeholder();
    composer->Focus();
  }
  slash_command_count_ = 0;
  slash_command_index_ = 0;
  popover->SetClass("hidden", true);
}

void DeskController::launch_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  const auto candidates = browser_.discover();
  auto* placeholder = document_.GetElementById("browser-status");
  if (!placeholder) return;
  if (candidates.empty()) {
    placeholder->SetInnerRML("<strong>未找到 Chrome / Chromium</strong><span>可在设置中指定浏览器路径</span>");
    return;
  }
  const auto executable = candidates.front().executable;
  auto* url_input = control(document_, "browser-url");
  const std::string url = url_input && !url_input->GetValue().empty()
      ? url_input->GetValue() : Rml::String("about:blank");
  placeholder->SetInnerRML("<strong>正在准备 Agent Browser…</strong><span>首次使用会安装并校验固定版本 Runtime</span>");
  browser_future_ = std::async(std::launch::async,
      [this, executable, url] {
        std::string error;
        if (!browser_.install_runtime(error)) {
          BrowserSessionState state;
          state.error = std::move(error);
          return state;
        }
        return browser_.open(executable, browser_session_, url, true);
      });
}

void DeskController::refresh_browser() {
  if (browser_future_.valid())
    return;
  browser_future_ = std::async(std::launch::async,
      [this] { return browser_.refresh(browser_session_); });
}

void DeskController::back_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  browser_future_ = std::async(std::launch::async,
      [this] { return browser_.back(browser_session_); });
}

void DeskController::forward_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  browser_future_ = std::async(std::launch::async,
      [this] { return browser_.forward(browser_session_); });
}

void DeskController::reload_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  browser_future_ = std::async(std::launch::async,
      [this] { return browser_.reload(browser_session_); });
}

void DeskController::toggle_browser_takeover() {
  browser_takeover_ = !browser_takeover_;
  if (auto* bar = document_.GetElementById("browser-permission")) {
    bar->SetClass("takeover", browser_takeover_);
    if (auto* message = bar->QuerySelector("span"))
      message->SetInnerRML(browser_takeover_
          ? "用户已接管外部浏览器 · Agent 网页操作已暂停"
          : "独立 Tokmon Profile · 仅显式操作网页");
  }
  text(document_, "browser-takeover",
       browser_takeover_ ? "恢复 Agent" : "用户接管");
}

void DeskController::stop_browser() {
  if (browser_future_.valid())
    return;
  browser_future_ = std::async(std::launch::async, [this] {
    BrowserSessionState state;
    state.session = browser_session_;
    std::string error;
    if (!browser_.close(browser_session_, error)) state.error = std::move(error);
    return state;
  });
}

void DeskController::click_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  auto* selector = control(document_, "browser-selector");
  if (!selector || selector->GetValue().empty())
    return;
  const std::string value = selector->GetValue();
  browser_future_ = std::async(std::launch::async, [this, value] {
    BrowserSessionState state;
    state.session = browser_session_;
    std::string error;
    if (!browser_.click(browser_session_, value, error)) {
      state.error = std::move(error);
      return state;
    }
    return browser_.refresh(browser_session_);
  });
}

void DeskController::fill_browser() {
  if (browser_future_.valid() || browser_takeover_)
    return;
  auto* selector = control(document_, "browser-selector");
  auto* value = control(document_, "browser-value");
  if (!selector || selector->GetValue().empty() || !value)
    return;
  const std::string selector_text = selector->GetValue();
  const std::string fill_text = value->GetValue();
  browser_future_ = std::async(std::launch::async,
      [this, selector_text, fill_text] {
        BrowserSessionState state;
        state.session = browser_session_;
        std::string error;
        if (!browser_.fill(browser_session_, selector_text, fill_text, error)) {
          state.error = std::move(error);
          return state;
        }
        return browser_.refresh(browser_session_);
      });
}

void DeskController::render_browser_state(const BrowserSessionState& state) {
  auto* status = document_.GetElementById("browser-status");
  if (!status)
    return;
  if (!state.error.empty()) {
    status->SetClass("compact", false);
    status->SetInnerRML("<strong>Agent Browser 操作失败</strong><span>" +
                        escape(state.error) + "</span>");
    return;
  }
  if (!state.running) {
    status->SetClass("compact", false);
    status->SetInnerRML("<strong>Agent Browser 已停止</strong><span>会话进程与私有 Profile 已安全关闭</span>");
    return;
  }
  status->SetClass("compact", true);
  status->SetInnerRML("<strong>" + escape(state.title) + "</strong><span>" +
                      escape(state.url) + "</span>");
  if (auto* input = control(document_, "browser-url"))
    input->SetValue(state.url);
  if (auto* image = document_.GetElementById("browser-preview")) {
    image->SetAttribute("src", state.preview_image.generic_string());
    image->SetClass("hidden", false);
  }
  text(document_, "browser-snapshot", escape(state.accessibility_snapshot));
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
  text(document_, "session-title", escape(created.title));
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
  current_file_.clear();
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
  const auto path = row.GetAttribute<Rml::String>("data-diff-path", "");
  if (path.empty()) return;
  const bool staged = row.GetAttribute<Rml::String>("data-diff-staged", "0") == "1";
  auto* view = document_.GetElementById("review-diff");
  if (!view) return;
  if (review_future_.valid()) {
    show_toast("另一个 Git 操作仍在执行");
    return;
  }
  current_diff_path_ = path;
  current_diff_staged_ = staged;
  view->SetInnerRML("<div class='diff-error'>正在工作线程生成 Diff…</div>");
  view->SetClass("hidden", false);
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
  const auto action = element.GetAttribute<Rml::String>("data-git-action", "");
  const auto path = element.GetAttribute<Rml::String>("data-git-path", "");
  const auto hunk_text = element.GetAttribute<Rml::String>("data-git-hunk", "0");
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
  const std::string commit_message = message && !message->GetValue().empty()
      ? message->GetValue() : Rml::String("Update from tokmon-desk");
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
        result.success = service.commit(commit_message, result.error) &&
            (!push_after_commit || service.push(result.error));
        result.snapshot = service.status();
        return result;
      });
  text(document_, "review-empty", push_after_commit
      ? "正在提交并推送…" : "正在创建提交…");
}

void DeskController::show_settings_page(Rml::Element& navigation) {
  const auto page = navigation.GetAttribute<Rml::String>("data-page", "general");
  capture_settings_page();
  Rml::ElementList items;
  document_.QuerySelectorAll(items, "[data-page]");
  for (auto* item : items)
    item->SetClass("active", item == &navigation);
  render_settings_page(page);
}

void DeskController::render_settings_page(std::string page) {
  auto* body = document_.GetElementById("settings-body");
  if (!body)
    return;
  settings_page_ = std::move(page);
  const auto input = [this](const char* id, const char* key,
                            const std::string& fallback,
                            const char* type = "text") {
    return "<input id='" + std::string(id) + "' type='" + type +
           "' value='" + escape(cbor_string(settings_values_, key, fallback)) +
           "'/>";
  };
  const auto select = [this](const char* id, const char* key,
                             const std::vector<std::string>& values,
                             const std::string& fallback) {
    const auto current = cbor_string(settings_values_, key, fallback);
    std::string rml = "<select id='" + std::string(id) + "' class='setting-select'>";
    for (const auto& value : values)
      rml += "<option value='" + escape(value) + "'" +
             (value == current ? " selected=''" : "") + ">" +
             escape(value) + "</option>";
    return rml + "</select>";
  };
  const auto boolean = [this](const char* id, const char* key,
                              const bool fallback) {
    const bool current = cbor_bool(settings_values_, key, fallback);
    return "<select id='" + std::string(id) + "' class='setting-select bool-select'>"
           "<option value='true'" + std::string(current ? " selected=''" : "") +
           ">已启用</option><option value='false'" +
           std::string(!current ? " selected=''" : "") +
           ">已禁用</option></select>";
  };
  const auto card = [](std::string title, std::string description,
                       std::string editor) {
    return "<div class='setting-card'><div class='setting-copy'><strong>" + std::move(title) +
           "</strong><p>" + std::move(description) + "</p></div>" +
           std::move(editor) + "</div>";
  };
  const auto model_card = [](std::string title, std::string description,
                             std::string editor) {
    return "<div class='setting-card model-setting-card'><div class='setting-copy'><strong>" +
           std::move(title) + "</strong><p>" + std::move(description) +
           "</p></div><div class='model-control'>" + std::move(editor) +
           "</div></div>";
  };

  if (settings_page_ == "general") {
    text(document_, "settings-title", "通用");
    text(document_, "settings-description", "管理语言、启动、自动保存和更新偏好");
    body->SetInnerRML(
        card("界面语言", "设置 tokmon-desk 使用的语言",
             select("setting-language", "language", {"简体中文", "English"}, "简体中文")) +
        card("启动页面", "启动后进入上次会话或首页",
             select("setting-startup", "startup", {"首页", "上次打开的会话"}, "首页")) +
        card("自动保存", "编辑器未保存缓冲的自动保存间隔",
             select("setting-autosave", "autosave", {"关闭", "1 分钟", "5 分钟", "15 分钟"}, "5 分钟")) +
        card("更新通道", "选择稳定版本或预览版本",
             select("setting-channel", "update_channel", {"稳定版", "测试版"}, "稳定版")));
  } else if (settings_page_ == "model") {
    text(document_, "settings-title", "智能体与模型");
    text(document_, "settings-description", "配置现有 tokmon-daemon 模型 Provider");
    std::string providers = "<div class='provider-list model-provider-list'>";
    const auto* encoded = tokmon::cbor::find(providers_payload_, "providers");
    if (encoded && encoded->as_array()) {
      for (const auto& provider : *encoded->as_array()) {
        const auto name = cbor_string(provider, "name");
        const auto model = cbor_string(provider, "model");
        const auto source = cbor_string(provider, "credential_source", "missing");
        providers += "<button class='provider-row" +
            std::string(name == selected_provider_ ? " selected" : "") +
            "' data-provider-use='" + escape(name) + "'><span><strong>" +
            escape(name) + "</strong><small>" + escape(model) +
            "</small></span><em>" + escape(source) + "</em></button>";
      }
    }
    providers += "</div>";
    body->SetInnerRML(
        model_card("模型平台", "来自当前项目配置；凭据只进入系统安全存储", providers) +
        model_card("主模型", "发送请求时使用的模型名称",
                   input("setting-main-model", "main_model", selected_model_)) +
        model_card("推理强度", "与旧版标准、高和最高选项一致",
                   select("setting-reasoning", "reasoning", {"标准", "高", "最高"}, "高")) +
        "<div class='provider-config-grid'><label>协议" +
        select("provider-protocol", "provider_protocol",
               {"openai-compatible", "anthropic", "gemini", "local"},
               "openai-compatible") + "</label><label>Endpoint" +
        input("provider-endpoint", "provider_endpoint", "") +
        "</label><label>认证" +
        select("provider-auth", "provider_auth", {"bearer", "x-api-key", "none"}, "bearer") +
        "</label><label>Secret 环境变量" +
        input("provider-secret-env", "provider_secret_env", "") +
        "</label><label>API Key" +
        input("provider-secret", "provider_secret", "", "password") +
        "</label><div class='provider-actions'><button id='configure-provider' class='secondary-button'>保存 Provider</button><button id='test-provider' class='secondary-button'>测试连接</button><button id='store-provider-secret' class='primary-button'>安全保存 Key</button></div></div>");
  } else if (settings_page_ == "access") {
    text(document_, "settings-title", "权限与安全");
    text(document_, "settings-description", "控制文件、命令、网络和高风险操作");
    body->SetInnerRML(
        card("文件访问", "限定 Agent 可访问的文件范围",
             select("setting-file-access", "file_access", {"受信路径", "仅工作区", "全部"}, "受信路径")) +
        card("命令审批", "设置执行本地命令前的确认策略",
             select("setting-command-approval", "command_approval", {"自动执行", "按需确认", "禁止执行"}, "按需确认")) +
        card("联网能力", "允许模型使用已配置的网络工具",
             boolean("setting-network", "network", true)) +
        card("高风险二次确认", "放弃修改、上传和敏感浏览器操作必须确认",
             boolean("setting-high-risk", "high_risk_confirmation", true)));
  } else if (settings_page_ == "workspace") {
    text(document_, "settings-title", "工作区");
    text(document_, "settings-description", "管理当前工作区和索引行为");
    body->SetInnerRML(
        card("当前路径", "项目文件的真实工作目录",
             "<code class='path-value'>" + escape(workspace_.root().string()) + "</code>") +
        card("索引模式", "控制文件搜索和语法索引范围",
             select("setting-index-mode", "index_mode", {"标准", "智能索引", "深度索引"}, "标准")) +
        card("自动同步", "监控文件系统变化并更新工作区视图",
             boolean("setting-workspace-sync", "workspace_sync", true)) +
        card("Git 集成", "显示审查、暂存、提交和推送操作",
             boolean("setting-git", "git", true)));
  } else if (settings_page_ == "notifications") {
    text(document_, "settings-title", "通知");
    text(document_, "settings-description", "配置任务完成和消息提醒");
    body->SetInnerRML(
        card("通知", "总通知开关", boolean("setting-notifications", "notifications", true)) +
        card("桌面通知", "允许系统通知中心显示完成状态",
             boolean("setting-desktop-notifications", "desktop_notifications", true)) +
        card("消息提醒", "新回复到达时发出提醒",
             boolean("setting-message-alerts", "message_alerts", true)) +
        card("免打扰时段", "在指定时段静默通知",
             select("setting-quiet-hours", "quiet_hours", {"关闭", "22:00 - 08:00", "23:00 - 07:00"}, "关闭")));
  } else if (settings_page_ == "appearance") {
    text(document_, "settings-title", "外观");
    text(document_, "settings-description", "保留旧版主题、颜色、密度与缩放设计");
    body->SetInnerRML(
        card("主题与颜色", "技术重写不修改旧版 Tokmon 主题和配色",
             "<span class='locked-value'>旧版浅色主题 · 已锁定</span>") +
        card("界面密度", "调整内容间距，不改变组件风格",
             select("setting-density", "density", {"紧凑", "舒适", "宽松"}, "舒适")) +
        card("界面缩放", "70%–200%；自动跟随系统显示缩放",
             "<input id='setting-ui-scale' type='number' min='70' max='200' step='5' value='" +
             std::to_string(std::clamp<std::int64_t>(cbor_integer(settings_values_, "ui_scale", 125), 70, 200)) + "'/>") +
        card("字体缩放", "保持内容字体可读性",
             "<input id='setting-font-scale' type='number' min='70' max='200' step='5' value='" +
             std::to_string(std::clamp<std::int64_t>(cbor_integer(settings_values_, "font_scale", 100), 70, 200)) + "'/>") );
  } else if (settings_page_ == "shortcuts") {
    text(document_, "settings-title", "快捷键");
    text(document_, "settings-description", "与旧版一致的工作台快捷键");
    body->SetInnerRML(
        "<div class='shortcut-table'>"
        "<div class='shortcut-row'><span>发送消息</span><kbd>Ctrl + Enter</kbd></div>"
        "<div class='shortcut-row'><span>保存文件</span><kbd>Ctrl + S</kbd></div>"
        "<div class='shortcut-row'><span>打开文件</span><kbd>Ctrl + P</kbd></div>"
        "<div class='shortcut-row'><span>打开审查</span><kbd>Ctrl + Shift + G</kbd></div>"
        "<div class='shortcut-row'><span>终端复制 / 粘贴</span><kbd>Ctrl + Shift + C / V</kbd></div>"
        "<div class='shortcut-row'><span>释放编辑器或终端焦点</span><kbd>Esc</kbd></div>"
        "</div>");
  } else if (settings_page_ == "account") {
    text(document_, "settings-title", "账户");
    text(document_, "settings-description", "管理本机显示资料和云同步偏好");
    body->SetInnerRML(
        card("昵称", "在本机 UI 中显示的名称",
             input("setting-nickname", "nickname", "")) +
        card("邮箱", "可选账户联系邮箱",
             input("setting-email", "email", "", "email")) +
        card("云同步", "同步前仍以当前 Tokmon 配置能力为准",
             boolean("setting-cloud-sync", "cloud_sync", false)));
  } else if (settings_page_ == "terminal") {
    text(document_, "settings-title", "终端");
    text(document_, "settings-description", "配置 tokmon-desk 的跨平台本地终端");
    std::string profile_select = "<select id='setting-terminal-profile' class='setting-select'>";
    const auto selected_profile = cbor_string(settings_values_, "terminal_profile", "auto");
    for (const auto& profile : discover_terminal_profiles()) {
      if (!profile.available)
        continue;
      profile_select += "<option value='" + escape(profile.id) + "'" +
          (profile.id == selected_profile ? " selected=''" : "") + ">" +
          escape(profile.label) + "</option>";
    }
    profile_select += "<option value='custom'" +
        std::string(selected_profile == "custom" ? " selected=''" : "") +
        ">自定义可执行文件</option>";
    profile_select += "</select>";
    body->SetInnerRML(
        card("默认 Shell", "Windows 支持 PowerShell/cmd/WSL；macOS 与 Linux 支持登录 Shell/POSIX PTY",
             profile_select) +
        card("自定义可执行文件", "仅在默认 Shell 选择“自定义可执行文件”时使用",
             "<input id='setting-terminal-executable' type='text' value='" +
             escape(cbor_string(settings_values_, "terminal_executable")) +
             "' placeholder='C:\\\\Program Files\\\\PowerShell\\\\7\\\\pwsh.exe 或 /bin/zsh'/>") +
        card("自定义参数", "支持空格、单引号、双引号和反斜杠转义",
             "<input id='setting-terminal-arguments' type='text' value='" +
             escape(cbor_string(settings_values_, "terminal_arguments")) +
             "' placeholder='-NoLogo 或 -l'/>") +
        card("终端字号", "新建终端标签时应用，并同步调整 PTY 行列网格",
             "<input id='setting-terminal-font-size' type='number' min='9' max='24' value='" +
             std::to_string(std::clamp<std::int64_t>(cbor_integer(
                 settings_values_, "terminal_font_size", 13), 9, 24)) + "'/>") +
        card("滚动缓冲", "libghostty-vt 保存的最大可回看行数",
             "<input id='setting-terminal-scrollback' type='number' min='1000' max='100000' value='" +
             std::to_string(cbor_integer(settings_values_, "terminal_scrollback", 10000)) + "'/>") +
        card("数据边界", "终端进程、历史和设置仅属于 tokmon-desk",
             "<span class='locked-value'>Desktop 本地</span>"));
  } else if (settings_page_ == "browser") {
    text(document_, "settings-title", "浏览器");
    text(document_, "settings-description", "Agent Browser 与系统 Chrome/Chromium");
    const auto candidates = browser_.discover();
    body->SetInnerRML(
        card("系统浏览器", "优先复用已安装的 Chrome/Chromium 可执行文件",
             "<code class='path-value'>" + escape(candidates.empty() ? "未自动发现" : candidates.front().executable.string()) + "</code>") +
        card("独立 Profile", "不会修改用户日常浏览器 Profile",
             "<span class='locked-value'>强制隔离</span>") +
        card("高风险操作", "上传、下载、权限、支付和密码操作进入用户确认",
             boolean("setting-browser-confirm", "browser_high_risk_confirmation", true)));
  } else {
    settings_page_ = "about";
    text(document_, "settings-title", "关于");
    text(document_, "settings-description", "tokmon-desk 技术栈与开源许可");
    body->SetInnerRML(
        card("tokmon-desk", "SDL3 + RmlUi + Skia 原生桌面技术重写",
             "<span class='locked-value'>开发版</span>") +
        card("开源组件", "RmlUi · SDL3 · Skia · HarfBuzz · FreeType · MD4C · Zep · libgit2 · tree-sitter · libghostty-vt", "") +
        card("Browser", "本轮状态：DEFERRED-BROWSER；基础安装包不包含浏览器运行时", "") +
        card("许可声明", "完整版权与许可文本随发行包的 THIRD_PARTY_NOTICES.md 提供；HarfBuzz 无需在关于页单独弹出免责声明。", ""));
  }

  Rml::ElementList providers;
  body->QuerySelectorAll(providers, "[data-provider-use]");
  for (auto* provider : providers)
    provider->AddEventListener("click", this);
  for (const char* id : {"configure-provider", "test-provider", "store-provider-secret"})
    listen(id);
}

void DeskController::capture_settings_page() {
  const auto string_control = [this](const char* id, const char* key) {
    if (auto* item = control(document_, id))
      set_cbor(settings_values_, key, std::string(item->GetValue()));
  };
  const auto bool_control = [this](const char* id, const char* key) {
    if (auto* item = control(document_, id))
      set_cbor(settings_values_, key, item->GetValue() == "true");
  };
  const auto int_control = [this](const char* id, const char* key,
                                  std::int64_t minimum, std::int64_t maximum) {
    if (auto* item = control(document_, id)) {
      try {
        set_cbor(settings_values_, key, std::clamp<std::int64_t>(
            std::stoll(item->GetValue()), minimum, maximum));
      } catch (...) {}
    }
  };
  string_control("setting-language", "language");
  string_control("setting-startup", "startup");
  string_control("setting-autosave", "autosave");
  string_control("setting-channel", "update_channel");
  string_control("setting-main-model", "main_model");
  string_control("setting-reasoning", "reasoning");
  string_control("setting-file-access", "file_access");
  string_control("setting-command-approval", "command_approval");
  string_control("setting-index-mode", "index_mode");
  string_control("setting-quiet-hours", "quiet_hours");
  string_control("setting-density", "density");
  string_control("setting-nickname", "nickname");
  string_control("setting-email", "email");
  string_control("setting-terminal-profile", "terminal_profile");
  string_control("setting-terminal-executable", "terminal_executable");
  string_control("setting-terminal-arguments", "terminal_arguments");
  bool_control("setting-network", "network");
  bool_control("setting-high-risk", "high_risk_confirmation");
  bool_control("setting-workspace-sync", "workspace_sync");
  bool_control("setting-git", "git");
  bool_control("setting-notifications", "notifications");
  bool_control("setting-desktop-notifications", "desktop_notifications");
  bool_control("setting-message-alerts", "message_alerts");
  bool_control("setting-cloud-sync", "cloud_sync");
  bool_control("setting-browser-confirm", "browser_high_risk_confirmation");
  int_control("setting-ui-scale", "ui_scale", 70, 200);
  int_control("setting-font-scale", "font_scale", 70, 200);
  int_control("setting-terminal-scrollback", "terminal_scrollback", 1000, 100000);
  int_control("setting-terminal-font-size", "terminal_font_size", 9, 24);
}

void DeskController::save_settings() {
  capture_settings_page();
  selected_model_ = cbor_string(settings_values_, "main_model", selected_model_);
  selected_effort_ = cbor_string(settings_values_, "reasoning", selected_effort_);
  set_cbor(settings_values_, "access_mode", selected_access_);
  std::string error;
  if (!save_local_settings(error)) {
    text(document_, "settings-status", escape("本地设置保存失败：" + error));
    return;
  }
  const auto shared = select_cbor_keys(settings_values_, {
      "main_model", "reasoning", "access_mode", "file_access",
      "command_approval", "network", "high_risk_confirmation"});
  enqueue_intent("settings.save", tokmon::cbor::object({{"values", shared}}));
  text(document_, "settings-status",
       "Desktop 偏好已本地保存；正在保存共享 Agent 配置…");
}

void DeskController::reset_settings() {
  const auto shared = select_cbor_keys(settings_values_, {
      "main_model", "reasoning", "access_mode", "file_access",
      "command_approval", "network", "high_risk_confirmation"});
  settings_values_ = default_desktop_settings(
      platform_.default_content_scale_percent());
  merge_cbor_map(settings_values_, shared);
  merge_cbor_map(settings_values_, tokmon::cbor::object({
      {"main_model", selected_model_}, {"reasoning", "高"},
      {"command_approval", "按需确认"}, {"network", true},
      {"high_risk_confirmation", true}, {"workspace", workspace_.root().generic_string()},
      {"file_access", "受信路径"}}));
  render_settings_page(settings_page_);
  text(document_, "settings-status", "已恢复默认值；点击“保存更改”后写入");
}

void DeskController::apply_providers(const tokmon::cbor::Value& payload) {
  providers_payload_ = payload;
  selected_provider_ = cbor_string(payload, "default", selected_provider_);
  if (const auto* providers = tokmon::cbor::find(payload, "providers");
      providers && providers->as_array()) {
    for (const auto& provider : *providers->as_array()) {
      if (cbor_string(provider, "name") != selected_provider_)
        continue;
      selected_model_ = cbor_string(provider, "model", selected_model_);
      break;
    }
  }
  if (!selected_model_.empty())
    text(document_, "active-model", escape(selected_model_) + "⌄");
  if (settings_page_ == "model")
    render_settings_page("model");
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
    text(document_, "settings-status", escape("操作失败：" + result.error));
  } else if (intent_action_ == "model.providers") {
    apply_providers(result.payload);
  } else if (intent_action_ == "settings.save") {
    text(document_, "settings-status",
         "Desktop 偏好已保存到本地；共享 Agent 配置已保存到当前项目");
  } else if (intent_action_ == "model.provider.use") {
    text(document_, "settings-status", "默认模型平台已切换；后台校验中");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.configure") {
    text(document_, "settings-status", "Provider 配置已原子保存；后台校验中");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.secret.set") {
    text(document_, "settings-status", "API Key 已写入系统安全存储");
    enqueue_intent("model.providers", tokmon::cbor::object({}));
  } else if (intent_action_ == "model.provider.test") {
    text(document_, "settings-status", "Provider 测试已完成");
  }
  intent_action_.clear();
  start_next_intent();
}

bool DeskController::handle_raw_event(const SDL_Event& event) {
  const auto event_x = [this, &event]() {
    const float coordinate = event.type == SDL_EVENT_MOUSE_MOTION
        ? event.motion.x : event.button.x;
    return coordinate * platform_.input_coordinate_scale();
  };
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      event.button.button == SDL_BUTTON_LEFT) {
    const float x = event_x();
    const float y = event.button.y * platform_.input_coordinate_scale();
    const float viewport_width = document_.GetClientWidth();
    if (y >= 46.f && sidebar_visible_ &&
        std::abs(x - static_cast<float>(sidebar_width_)) <= 6.f) {
      panel_resize_ = PanelResize::sidebar;
      panel_resize_anchor_x_ = x;
      panel_resize_start_width_ = sidebar_width_;
      SDL_CaptureMouse(true);
      return true;
    }
    if (y >= 46.f && right_panel_visible_ &&
        std::abs(x - (viewport_width - static_cast<float>(right_panel_width_))) <= 6.f) {
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
    const int viewport_width = static_cast<int>(std::lround(
        document_.GetClientWidth()));
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
      if (next <= 214) {
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
      render_settings_page(settings_page_);
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
      auto& tab = active_terminal_tab();
      if (tab.vt->mouse_tracking()) {
        const auto button = event.wheel.y > 0
            ? TerminalMouseButton::wheel_up
            : TerminalMouseButton::wheel_down;
        const auto bytes = tab.vt->encode_mouse(
            TerminalMouseAction::press, button, event.wheel.mouse_x,
            event.wheel.mouse_y, tab.columns * tab.cell_width,
            tab.rows * tab.cell_height, tab.cell_width, tab.cell_height,
            terminal_modifiers(SDL_GetModState()));
        std::string error;
        if (!bytes.empty() && !tab.session->write(bytes, error))
          tab.vt->append("\r\n" + error + "\r\n");
      } else {
        tab.vt->scroll_viewport(event.wheel.y > 0 ? -3 : 3);
        if (auto* surface = dynamic_cast<ElementTerminal*>(
                document_.GetElementById("terminal-surface")))
          surface->set_snapshot(tab.vt->render_snapshot());
      }
      return true;
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
      text(document_, "terminal-status", "正在运行 · 已接收输入");
      const std::string incoming(event.text.text);
      if (!pending_terminal_keydown_text_.empty() &&
          pending_terminal_keydown_text_.starts_with(incoming))
        pending_terminal_keydown_text_.erase(0, incoming.size());
      const auto bytes = terminal_vt().encode_key(
          TerminalKey::unidentified, incoming,
          terminal_modifiers(SDL_GetModState()));
      std::string error;
      if (!bytes.empty() && !terminal_session().write(bytes, error))
        terminal_vt().append("\r\n" + error + "\r\n");
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
    pending_terminal_keydown_text_.clear();
    platform_.DeactivateKeyboard();
    text(document_, "terminal-hint",
         "点击终端后直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Ctrl+单击打开 OSC 8 链接 · Esc 释放焦点");
    return true;
  }

  if (heavy_focus_ == HeavyFocus::terminal) {
    if (control_key && shift_key && event.key.key == SDLK_C) {
      if (auto* surface = dynamic_cast<ElementTerminal*>(
              document_.GetElementById("terminal-surface"))) {
        const auto selected = surface->selected_text();
        if (!selected.empty())
          platform_.SetClipboardText(selected);
      }
      return true;
    }
    if (control_key && shift_key && event.key.key == SDLK_V) {
      paste_terminal(false);
      return true;
    }
    const auto key = terminal_key(event.key.key);
    const auto printable = printable_key_text(event.key.key, modifiers);
    if (!printable.empty() && !control_key &&
        !(modifiers & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
      pending_terminal_keydown_text_ += printable;
      return true;
    }
    const auto bytes = terminal_vt().encode_key(
        key, key_text(event.key.key), terminal_modifiers(modifiers),
        event.key.repeat);
    std::string error;
    if (!bytes.empty() && !terminal_session().write(bytes, error))
      terminal_vt().append("\r\n" + error + "\r\n");
    return true;
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
  auto* element = event.GetCurrentElement();
  if (!element) return;
  const auto& id = element->GetId();
  if (id == "conversation" && event.GetType() == "mousescroll") {
    constexpr float kEstimatedTurnHeight = 210.f;
    constexpr std::size_t kLeadTurns = 12;
    const auto visible_turn = static_cast<std::size_t>(std::max(
        0.f, element->GetScrollTop()) / kEstimatedTurnHeight);
    const auto desired = visible_turn > kLeadTurns
        ? visible_turn - kLeadTurns : 0;
    const auto maximum = conversation_total_turns_ > 80
        ? conversation_total_turns_ - 80 : 0;
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
       event.GetType() == "mouseup")) {
    auto* terminal_surface = dynamic_cast<ElementTerminal*>(element);
    if (!terminal_surface)
      return;
    const auto absolute = element->GetAbsoluteOffset(Rml::BoxArea::Content);
    if (event.GetType() == "mousedown") {
      // Acquire the raw SDL keyboard route on pointer-down. Waiting for RmlUi's
      // synthetic click is unreliable when the same element also owns drag
      // selection and mouse-reporting handlers.
      heavy_focus_ = HeavyFocus::terminal;
      element->Focus();
      start_terminal();
      resize_terminal_to_surface();
      platform_.ActivateKeyboard(absolute, 17.f);
      if (active_terminal_tab().started) {
        text(document_, "terminal-status", "正在运行 · 已聚焦");
        text(document_, "terminal-hint",
             "终端已聚焦 · 直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Esc 释放焦点");
      }
    }
    const float local_x = static_cast<float>(
        event.GetParameter<int>("mouse_x", 0)) - absolute.x;
    const float local_y = static_cast<float>(
        event.GetParameter<int>("mouse_y", 0)) - absolute.y;
    const bool force_selection = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (terminal_vt().mouse_tracking() && !force_selection) {
      TerminalMouseAction action = TerminalMouseAction::motion;
      if (event.GetType() == "mousedown") {
        action = TerminalMouseAction::press;
        terminal_mouse_down_ = true;
      } else if (event.GetType() == "mouseup") {
        action = TerminalMouseAction::release;
        terminal_mouse_down_ = false;
      }
      const auto bytes = terminal_vt().encode_mouse(
          action, event.GetType() == "mousemove" ? TerminalMouseButton::none
                                                   : TerminalMouseButton::left,
          local_x, local_y, static_cast<int>(element->GetClientWidth()),
          static_cast<int>(element->GetClientHeight()),
          active_terminal_tab().cell_width, active_terminal_tab().cell_height,
          terminal_modifiers(SDL_GetModState()), terminal_mouse_down_);
      std::string error;
      if (!bytes.empty() && !terminal_session().write(bytes, error))
        terminal_vt().append("\r\n" + error + "\r\n");
    } else if (event.GetType() == "mousedown") {
      terminal_surface->begin_selection(local_x, local_y);
    } else if (event.GetType() == "mousemove") {
      terminal_surface->update_selection(local_x, local_y);
    } else {
      terminal_surface->end_selection();
    }
    return;
  }
  if (event.GetType() == "click" && id == "terminal-surface") {
    heavy_focus_ = HeavyFocus::terminal;
    element->Focus();
    start_terminal();
    resize_terminal_to_surface();
    platform_.ActivateKeyboard(
        element->GetAbsoluteOffset(Rml::BoxArea::Content), 17.f);
    if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0) {
      const auto absolute = element->GetAbsoluteOffset(Rml::BoxArea::Content);
      if (auto* terminal_surface = dynamic_cast<ElementTerminal*>(element)) {
        const auto link = terminal_surface->hyperlink_at(
            static_cast<float>(event.GetParameter<int>("mouse_x", 0)) - absolute.x,
            static_cast<float>(event.GetParameter<int>("mouse_y", 0)) - absolute.y);
        if (terminal_safe_hyperlink(link))
          (void)SDL_OpenURL(link.c_str());
      }
    }
    return;
  }
  if (event.GetType() == "keydown") {
    const auto key = event.GetParameter<int>("key_identifier", 0);
    if (id == "composer") {
      auto* popover = document_.GetElementById("composer-popover");
      const bool slash_open = popover && !popover->IsClassSet("hidden") &&
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
      if (key == Rml::Input::KI_ESCAPE && popover &&
          !popover->IsClassSet("hidden")) {
        slash_command_count_ = 0;
        slash_command_index_ = 0;
        popover->SetClass("hidden", true);
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
      if (id == "terminal-input") send_terminal_input();
      else if (id == "composer") send_message();
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
    search_files();
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "navigation-search") {
    filter_navigation();
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "settings-search") {
    const auto query = control(document_, "settings-search")->GetValue();
    const std::pair<std::string_view, std::string_view> pages[] = {
        {"general", "语言启动自动保存更新通用"}, {"model", "智能体模型provider平台推理"},
        {"access", "文件访问命令审批联网高风险权限安全"}, {"workspace", "工作区索引同步git"},
        {"notifications", "通知桌面消息免打扰"}, {"appearance", "外观主题颜色密度缩放字体"},
        {"shortcuts", "快捷键键盘"}, {"account", "账户昵称邮箱云同步"},
        {"terminal", "终端shell滚动"}, {"browser", "浏览器chrome chromium profile"},
        {"about", "关于许可版本"}};
    if (!query.empty()) {
      for (const auto& [page, keywords] : pages)
        if (keywords.find(query) != std::string_view::npos) {
          settings_page_ = std::string(page);
          render_settings_page(settings_page_);
          Rml::ElementList items;
          document_.QuerySelectorAll(items, "[data-page]");
          for (auto* item : items)
            item->SetClass("active",
                item->GetAttribute<Rml::String>("data-page", "") == page);
          break;
        }
    }
    return;
  }
  if ((event.GetType() == "input" || event.GetType() == "change" ||
       event.GetType() == "keyup") &&
      id == "terminal-search") {
    search_terminal();
    return;
  }
  if (event.GetType() == "click" && id == "terminal-search") {
    heavy_focus_ = HeavyFocus::none;
    pending_terminal_keydown_text_.clear();
    return;
  }
  if (element->GetAttribute<Rml::String>("data-navigation-id", "").size()) {
    handle_navigation(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-git-branch", "").size()) {
    switch_branch(*element);
    return;
  }
  if (const auto terminal_tab =
          element->GetAttribute<Rml::String>("data-terminal-tab", "");
      !terminal_tab.empty()) {
    select_terminal_tab(terminal_tab);
    return;
  }
  if (const auto provider = element->GetAttribute<Rml::String>("data-provider-use", "");
      !provider.empty()) {
    selected_provider_ = provider;
    enqueue_intent("model.provider.use", tokmon::cbor::object({{"name", selected_provider_}}));
    render_settings_page("model");
    return;
  }
  if (const auto kind = element->GetAttribute<Rml::String>("data-choice-kind", "");
      !kind.empty()) {
    const auto value = element->GetAttribute<Rml::String>("data-choice-value", "");
    if (kind == "effort") {
      selected_effort_ = value;
      set_cbor(settings_values_, "reasoning", selected_effort_);
      text(document_, "effort-button", "♙ " + escape(selected_effort_) + "⌄");
    } else if (kind == "access") {
      selected_access_ = value;
      text(document_, "access-button",
           selected_access_ == "full" ? "♢ 完全访问⌄" : "♢ 受限访问⌄");
    } else if (kind == "provider") {
      selected_provider_ = value;
      if (const auto* providers = tokmon::cbor::find(providers_payload_, "providers");
          providers && providers->as_array())
        for (const auto& provider : *providers->as_array())
          if (cbor_string(provider, "name") == selected_provider_) {
            selected_model_ = cbor_string(provider, "model", selected_model_);
            break;
          }
      text(document_, "active-model", escape(selected_model_) + "⌄");
      enqueue_intent("model.provider.use",
                     tokmon::cbor::object({{"name", selected_provider_}}));
    }
    if (auto* popover = document_.GetElementById("composer-popover"))
      popover->SetClass("hidden", true);
    return;
  }
  if (const auto command = element->GetAttribute<Rml::String>("data-command", "");
      !command.empty()) {
    if (auto* composer = control(document_, "composer")) {
      composer->SetValue(command + " ");
      update_composer_placeholder();
      composer->Focus();
    }
    if (auto* popover = document_.GetElementById("composer-popover"))
      popover->SetClass("hidden", true);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-path", "").size()) {
    preview_file(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-git-action", "").size()) {
    handle_git_action(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-diff-path", "").size()) {
    preview_diff(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-page", "").size()) {
    show_settings_page(*element);
    return;
  }
  if (element->GetAttribute<Rml::String>("data-starter", "").size()) {
    choose_starter(*element);
    return;
  }
  if (id == "new-session-button") {
    render_navigation();
    toggle_hidden("new-session-overlay");
  }
  else if (id == "settings-button") {
    render_settings_page(settings_page_);
    toggle_hidden("settings-overlay");
  }
  else if (id == "close-settings") toggle_hidden("settings-overlay");
  else if (id == "save-settings") { save_settings(); toggle_hidden("settings-overlay"); }
  else if (id == "reset-settings") reset_settings();
  else if (id == "close-new-session" || id == "cancel-new-session") toggle_hidden("new-session-overlay");
  else if (id == "confirm-new-session") create_session();
  else if (id == "environment-toggle") toggle_hidden("environment-panel");
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
  else if (id == "title-edit") {
    auto* popover = document_.GetElementById("composer-popover");
    if (popover) {
      const auto* selected = navigation_.selected();
      popover->SetInnerRML(
          "<div class='popover-card title-popover'><strong>重命名会话</strong>"
          "<input id='rename-title-input' type='text' value='" +
          escape(selected ? selected->title : "新会话") +
          "'/><button id='confirm-title-rename' class='primary-button'>保存</button></div>");
      popover->SetClass("hidden", false);
      listen("confirm-title-rename");
    }
  }
  else if (id == "confirm-title-rename") {
    auto* title = control(document_, "rename-title-input");
    if (title && navigation_.rename_selected(title->GetValue(), true)) {
      text(document_, "session-title", escape(title->GetValue()));
      render_navigation();
      save_navigation();
      if (!active_ray_.empty())
        enqueue_intent("command.execute", tokmon::cbor::object({
            {"text", "/rename " + std::string(title->GetValue())},
            {"ray", active_ray_}, {"surface", "desktop-ui"}}));
    }
    document_.GetElementById("composer-popover")->SetClass("hidden", true);
  }
  else if (id == "network-toggle") {
    auto* button = document_.GetElementById("network-toggle");
    const bool enabled = !button->IsClassSet("active");
    button->SetClass("active", enabled);
    set_cbor(settings_values_, "network", enabled);
  }
  else if (id == "attach-button") choose_attachment();
  else if (id == "access-button" || id == "active-model" || id == "effort-button") {
    auto* popover = document_.GetElementById("composer-popover");
    if (!popover) return;
    std::ostringstream choices;
    choices << "<div class='popover-card choice-popover'>";
    if (id == "access-button") {
      choices << "<strong>访问权限</strong><button data-choice-kind='access' data-choice-value='full'>完全访问</button>"
                 "<button data-choice-kind='access' data-choice-value='restricted'>受限访问</button>";
    } else if (id == "effort-button") {
      choices << "<strong>推理强度</strong>";
      for (const char* value : {"标准", "高", "最高"})
        choices << "<button data-choice-kind='effort' data-choice-value='" << value << "'>" << value << "</button>";
    } else {
      choices << "<strong>模型平台</strong>";
      if (const auto* providers = tokmon::cbor::find(providers_payload_, "providers");
          providers && providers->as_array())
        for (const auto& provider : *providers->as_array())
          choices << "<button data-choice-kind='provider' data-choice-value='"
                  << escape(cbor_string(provider, "name")) << "'><span>"
                  << escape(cbor_string(provider, "name")) << "</span><small>"
                  << escape(cbor_string(provider, "model")) << "</small></button>";
    }
    choices << "</div>";
    popover->SetInnerRML(choices.str());
    popover->SetClass("hidden", false);
    Rml::ElementList choice_rows;
    popover->QuerySelectorAll(choice_rows, "[data-choice-kind]");
    for (auto* choice : choice_rows) choice->AddEventListener("click", this);
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
  }
  else if (id == "right-collapse") {
    set_right_panel_visible(false);
  }
  else if (id == "right-fullscreen") {
    if (auto* body = document_.GetElementById("app-shell"))
      body->SetClass("right-fullscreen", !body->IsClassSet("right-fullscreen"));
  }
  else if (id == "review-tab") show_right_view("review-view");
  else if (id == "files-tab") { show_right_view("files-view"); refresh_files(); }
  else if (id == "terminal-tab") {
    show_right_view("terminal-view");
    start_terminal();
    if (auto* surface = document_.GetElementById("terminal-surface")) {
      heavy_focus_ = HeavyFocus::terminal;
      surface->Focus();
      resize_terminal_to_surface();
      platform_.ActivateKeyboard(
          surface->GetAbsoluteOffset(Rml::BoxArea::Content), 17.f);
      if (active_terminal_tab().started) {
        text(document_, "terminal-status", "正在运行 · 已聚焦");
        text(document_, "terminal-hint",
             "终端已聚焦 · 直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Esc 释放焦点");
      }
    }
  }
  else if (id == "terminal-new-tab") {
    create_terminal_tab();
    start_terminal();
  }
  else if (id == "terminal-close-tab") close_terminal_tab();
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
  else if (id == "add-tab-button") show_toast("已打开全部标签页");
  else if (id == "branch-button") toggle_branch_menu();
  else if (id == "close-diff") {
    current_diff_path_.clear();
    if (auto* diff = document_.GetElementById("review-diff")) {
      diff->SetInnerRML({});
      diff->SetClass("hidden", true);
    }
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
  else if (id == "browser-launch") launch_browser();
  else if (id == "browser-go") launch_browser();
  else if (id == "browser-refresh") refresh_browser();
  else if (id == "browser-back") back_browser();
  else if (id == "browser-forward") forward_browser();
  else if (id == "browser-reload") reload_browser();
  else if (id == "browser-takeover") toggle_browser_takeover();
  else if (id == "browser-stop") stop_browser();
  else if (id == "browser-click") click_browser();
  else if (id == "browser-fill") fill_browser();
  else if (id == "configure-provider") {
    auto value = [](Rml::ElementFormControl* item) {
      return item ? std::string(item->GetValue()) : std::string{};
    };
    enqueue_intent("model.provider.configure", tokmon::cbor::object({
        {"name", selected_provider_}, {"protocol", value(control(document_, "provider-protocol"))},
        {"endpoint", value(control(document_, "provider-endpoint"))},
        {"model", value(control(document_, "setting-main-model"))},
        {"auth", value(control(document_, "provider-auth"))},
        {"secret_env", value(control(document_, "provider-secret-env"))},
        {"thinking", true}, {"default", true}, {"reasoning_effort", "high"},
        {"max_output_tokens", static_cast<std::int64_t>(4096)},
        {"max_attempts", static_cast<std::int64_t>(6)},
        {"retry_backoff_ms", static_cast<std::int64_t>(5000)}}));
    text(document_, "settings-status", "正在保存 Provider 配置…");
  }
  else if (id == "store-provider-secret") {
    auto* secret = control(document_, "provider-secret");
    if (secret && !secret->GetValue().empty()) {
      enqueue_intent("model.provider.secret.set", tokmon::cbor::object({
          {"name", selected_provider_}, {"secret", std::string(secret->GetValue())}}));
      secret->SetValue("");
      text(document_, "settings-status", "正在写入系统安全存储…");
    }
  }
  else if (id == "test-provider") {
    enqueue_intent("model.provider.test", tokmon::cbor::object({
        {"name", selected_provider_}, {"text", "Reply with TOKMON_PROVIDER_OK"},
        {"surface", "desktop-ui"}}));
    text(document_, "settings-status", "正在通过真实模型光路测试…");
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
    pending_terminal_paste_.clear();
    toggle_hidden("terminal-paste-overlay");
  }
  else if (id == "confirm-terminal-paste") paste_terminal(true);
  else if (id == "terminal-clear-search") {
    if (auto* search = control(document_, "terminal-search"))
      search->SetValue("");
    search_terminal();
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
      if (!current_file_.empty() && change.path == current_file_)
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
  if (browser_future_.valid() &&
      browser_future_.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    changed = true;
    render_browser_state(browser_future_.get());
  }
  if (terminal_tabs_.empty())
    return changed;
  if (!pending_terminal_keydown_text_.empty() &&
      heavy_focus_ == HeavyFocus::terminal) {
    const auto pending = std::move(pending_terminal_keydown_text_);
    pending_terminal_keydown_text_.clear();
    const auto bytes = terminal_vt().encode_key(
        TerminalKey::unidentified, pending, 0);
    std::string error;
    if (!bytes.empty() && !terminal_session().write(bytes, error))
      terminal_vt().append("\r\n" + error + "\r\n");
    changed = true;
  }
  std::size_t terminal_frame_budget = 256u * 1024u;
  for (std::size_t index = 0;
       index < terminal_tabs_.size() && terminal_frame_budget > 0; ++index) {
    auto& tab = *terminal_tabs_[index];
    if (!tab.started)
      continue;
    const auto output = tab.session->take_output(
        std::min<std::size_t>(64u * 1024u, terminal_frame_budget));
    if (output.empty())
      continue;
    changed = true;
    terminal_frame_budget -= output.size();
    if (index == active_terminal_index_)
      text(document_, "terminal-status", "正在运行 · 已更新");
    tab.vt->append(output);
    if (index == active_terminal_index_)
      if (auto* surface = dynamic_cast<ElementTerminal*>(
              document_.GetElementById("terminal-surface"))) {
        surface->set_snapshot(tab.vt->render_snapshot());
        search_terminal();
      }
  }
  return changed;
}

int DeskController::update_poll_interval_ms() const noexcept {
  if (chat_future_.valid() || startup_future_.valid() ||
      browser_future_.valid() || intent_future_.valid() ||
      file_search_future_.valid() || file_tree_future_.valid() ||
      file_load_future_.valid() || workspace_switch_future_.valid() ||
      syntax_future_.valid() || change_set_future_.valid())
    return 16;
  if (!terminal_tabs_.empty() &&
      active_terminal_index_ < terminal_tabs_.size() &&
      terminal_tabs_[active_terminal_index_]->started)
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
