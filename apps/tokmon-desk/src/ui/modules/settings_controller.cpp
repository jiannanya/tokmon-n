#include "ui/modules/settings_controller.hpp"

#include "ui/desk_view_model.hpp"
#include "ui/modules/browser_controller.hpp"
#include "terminal/terminal_service.hpp"

#include <algorithm>
#include <map>

namespace tokmon::desk {
namespace {

std::string get_string(const tokmon::cbor::Value& values,
                       const std::string_view key,
                       const std::string_view fallback = {}) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::string>(value->data)
      ? std::string(value->as_string()) : std::string(fallback);
}

bool get_bool(const tokmon::cbor::Value& values, const std::string_view key,
              const bool fallback) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<bool>(value->data)
      ? value->as_bool() : fallback;
}

std::int64_t get_integer(const tokmon::cbor::Value& values,
                         const std::string_view key,
                         const std::int64_t fallback) {
  const auto* value = tokmon::cbor::find(values, key);
  return value && std::holds_alternative<std::int64_t>(value->data)
      ? value->as_integer() : fallback;
}

void set_value(tokmon::cbor::Value& values, std::string key,
               tokmon::cbor::Value value) {
  if (!values.as_map())
    values = tokmon::cbor::Value::Map{};
  (*values.as_map())[std::move(key)] = std::move(value);
}

void merge(tokmon::cbor::Value& destination,
           const tokmon::cbor::Value& source) {
  if (!destination.as_map())
    destination = tokmon::cbor::Value::Map{};
  if (source.as_map())
    for (const auto& [key, value] : *source.as_map())
      destination.as_map()->insert_or_assign(key, value);
}

tokmon::cbor::Value select(const tokmon::cbor::Value& source,
                           const std::initializer_list<std::string_view> keys) {
  tokmon::cbor::Value::Map result;
  for (const auto key : keys)
    if (const auto* value = tokmon::cbor::find(source, key))
      result.emplace(std::string(key), *value);
  return result;
}

tokmon::cbor::Value defaults(const int ui_scale) {
  return tokmon::cbor::object({
      {"language", "简体中文"}, {"startup", "首页"},
      {"autosave", "5 分钟"}, {"update_channel", "稳定版"},
      {"index_mode", "标准"}, {"workspace_sync", true}, {"git", true},
      {"notifications", true}, {"desktop_notifications", true},
      {"message_alerts", true}, {"quiet_hours", "关闭"},
      {"density", "舒适"}, {"font_scale", static_cast<std::int64_t>(100)},
      {"ui_scale", static_cast<std::int64_t>(ui_scale)}, {"nickname", ""},
      {"email", ""}, {"cloud_sync", false}, {"sidebar_visible", true},
      {"right_panel_visible", true},
      {"sidebar_width", static_cast<std::int64_t>(240)},
      {"right_panel_width", static_cast<std::int64_t>(214)},
      {"layout_revision", static_cast<std::int64_t>(2)},
      {"navigation_revision", static_cast<std::int64_t>(2)},
      {"last_workspace", ""}, {"terminal_profile", "auto"},
      {"terminal_executable", ""}, {"terminal_arguments", ""},
      {"terminal_font_size", static_cast<std::int64_t>(13)},
      {"terminal_scrollback", static_cast<std::int64_t>(10000)},
      {"network", true}, {"high_risk_confirmation", true},
      {"browser_high_risk_confirmation", true}});
}

void normalize_legacy(tokmon::cbor::Value& values) {
  const auto normalize_boolean = [&values](const char* key,
                                           const char* enabled,
                                           const char* disabled) {
    const auto* value = tokmon::cbor::find(values, key);
    if (value && std::holds_alternative<bool>(value->data))
      set_value(values, key, value->as_bool() ? enabled : disabled);
  };
  normalize_boolean("autosave", "5 分钟", "关闭");
  normalize_boolean("quiet_hours", "22:00 - 08:00", "关闭");
  const auto normalize_alias = [&values](const char* key,
                                         const std::string_view legacy,
                                         const char* current) {
    const auto* value = tokmon::cbor::find(values, key);
    if (value && value->as_string() == legacy)
      set_value(values, key, current);
  };
  normalize_alias("startup", "恢复上次会话", "上次打开的会话");
  normalize_alias("command_approval", "高风险操作时询问", "按需确认");
  normalize_alias("file_access", "工作区", "仅工作区");
}

} // namespace

SettingsController::SettingsController(DeskViewModel& view_model,
                                       BrowserController& browser,
                                       const int default_ui_scale)
    : view_model_(view_model), browser_(browser),
      default_ui_scale_(default_ui_scale), values_(defaults(default_ui_scale)) {}

void SettingsController::load(const tokmon::cbor::Value& stored) {
  values_ = defaults(default_ui_scale_);
  merge(values_, stored);
  normalize_legacy(values_);
  model_ = get_string(values_, "main_model");
  effort_ = get_string(values_, "reasoning", "高");
  access_ = get_string(values_, "access_mode", "full");
  values_to_view({});
}

void SettingsController::show(std::string page,
                              const std::filesystem::path& workspace) {
  static const std::map<std::string, std::pair<const char*, const char*>> copy{
      {"general", {"通用", "管理语言、启动、自动保存和更新偏好"}},
      {"model", {"智能体与模型", "配置现有 tokmon-daemon 模型 Provider"}},
      {"access", {"权限与安全", "控制文件、命令、网络和高风险操作"}},
      {"workspace", {"工作区", "管理当前工作区和索引行为"}},
      {"notifications", {"通知", "配置任务完成和消息提醒"}},
      {"appearance", {"外观", "保留旧版主题、颜色、密度与缩放设计"}},
      {"shortcuts", {"快捷键", "与旧版一致的工作台快捷键"}},
      {"account", {"账户", "管理本机显示资料和云同步偏好"}},
      {"terminal", {"终端", "配置 tokmon-desk 的跨平台本地终端"}},
      {"browser", {"浏览器", "Agent Browser 与系统 Chrome/Chromium"}},
      {"about", {"关于", "tokmon-desk 技术栈与开源许可"}},
  };
  if (!copy.contains(page))
    page = "about";
  page_ = std::move(page);
  values_to_view(workspace);
  auto& settings = view_model_.state().settings;
  settings.page = page_;
  settings.title = copy.at(page_).first;
  settings.description = copy.at(page_).second;
  view_model_.dirty();
}

void SettingsController::values_to_view(
    const std::filesystem::path& workspace) {
  auto& view = view_model_.state().settings;
  view.language = string("language", "简体中文");
  view.startup = string("startup", "首页");
  view.autosave = string("autosave", "5 分钟");
  view.update_channel = string("update_channel", "稳定版");
  view.main_model = model_;
  view.reasoning = effort_;
  view.file_access = string("file_access", "受信路径");
  view.command_approval = string("command_approval", "按需确认");
  view.index_mode = string("index_mode", "标准");
  view.quiet_hours = string("quiet_hours", "关闭");
  view.density = string("density", "舒适");
  view.nickname = string("nickname");
  view.email = string("email");
  view.terminal_profile = string("terminal_profile", "auto");
  view.terminal_executable = string("terminal_executable");
  view.terminal_arguments = string("terminal_arguments");
  view.network = boolean("network", true);
  view.high_risk_confirmation = boolean("high_risk_confirmation", true);
  view.workspace_sync = boolean("workspace_sync", true);
  view.git = boolean("git", true);
  view.notifications = boolean("notifications", true);
  view.desktop_notifications = boolean("desktop_notifications", true);
  view.message_alerts = boolean("message_alerts", true);
  view.cloud_sync = boolean("cloud_sync", false);
  view.browser_high_risk_confirmation =
      boolean("browser_high_risk_confirmation", true);
  view.ui_scale = static_cast<int>(std::clamp<std::int64_t>(
      integer("ui_scale", default_ui_scale_), 70, 200));
  view.font_scale = static_cast<int>(std::clamp<std::int64_t>(
      integer("font_scale", 100), 70, 200));
  view.terminal_font_size = static_cast<int>(std::clamp<std::int64_t>(
      integer("terminal_font_size", 13), 9, 24));
  view.terminal_scrollback = static_cast<int>(std::clamp<std::int64_t>(
      integer("terminal_scrollback", 10000), 1000, 100000));
  if (!workspace.empty())
    view.workspace_path = workspace.generic_string();
  if (const auto executable = browser_.discovered_executable(); !executable.empty())
    view.browser_executable = executable.generic_string();
  else
    view.browser_executable = "未自动发现";
  view.terminal_profiles.clear();
  for (const auto& profile : discover_terminal_profiles())
    if (profile.available)
      view.terminal_profiles.push_back({profile.id, profile.label,
                                        profile.id == view.terminal_profile});
  view.terminal_profiles.push_back(
      {"custom", "自定义可执行文件", view.terminal_profile == "custom"});
  sync_shell();
}

void SettingsController::view_to_values() {
  const auto& view = view_model_.state().settings;
  set("language", view.language); set("startup", view.startup);
  set("autosave", view.autosave); set("update_channel", view.update_channel);
  model_ = view.main_model; effort_ = view.reasoning;
  set("main_model", model_); set("reasoning", effort_);
  set("file_access", view.file_access);
  set("command_approval", view.command_approval);
  set("index_mode", view.index_mode); set("quiet_hours", view.quiet_hours);
  set("density", view.density); set("nickname", view.nickname);
  set("email", view.email); set("terminal_profile", view.terminal_profile);
  set("terminal_executable", view.terminal_executable);
  set("terminal_arguments", view.terminal_arguments);
  set("network", view.network); set("high_risk_confirmation", view.high_risk_confirmation);
  set("workspace_sync", view.workspace_sync); set("git", view.git);
  set("notifications", view.notifications);
  set("desktop_notifications", view.desktop_notifications);
  set("message_alerts", view.message_alerts); set("cloud_sync", view.cloud_sync);
  set("browser_high_risk_confirmation", view.browser_high_risk_confirmation);
  set("ui_scale", static_cast<std::int64_t>(view.ui_scale));
  set("font_scale", static_cast<std::int64_t>(view.font_scale));
  set("terminal_font_size", static_cast<std::int64_t>(view.terminal_font_size));
  set("terminal_scrollback", static_cast<std::int64_t>(view.terminal_scrollback));
  set("access_mode", access_);
  sync_shell();
}

void SettingsController::apply_shared(const tokmon::cbor::Value& payload) {
  const auto* incoming = tokmon::cbor::find(payload, "values");
  if (!incoming || !incoming->as_map())
    return;
  merge(values_, select(*incoming, {"main_model", "reasoning", "access_mode",
                                    "file_access", "command_approval", "network",
                                    "high_risk_confirmation"}));
  normalize_legacy(values_);
  model_ = get_string(*incoming, "main_model", model_);
  effort_ = get_string(*incoming, "reasoning", effort_);
  access_ = get_string(*incoming, "access_mode", access_);
  values_to_view({});
  view_model_.dirty();
}

void SettingsController::apply_providers(const tokmon::cbor::Value& payload) {
  providers_ = payload;
  provider_ = get_string(payload, "default", provider_);
  auto& rows = view_model_.state().settings.providers;
  rows.clear();
  if (const auto* encoded = tokmon::cbor::find(payload, "providers");
      encoded && encoded->as_array()) {
    for (const auto& item : *encoded->as_array()) {
      const auto name = get_string(item, "name");
      const auto item_model = get_string(item, "model");
      rows.push_back({name, item_model,
                      get_string(item, "credential_source", "missing"),
                      name == provider_});
      if (name == provider_)
        model_ = item_model;
    }
  }
  view_model_.state().settings.main_model = model_;
  sync_shell();
  view_model_.dirty();
}

void SettingsController::reset(const std::filesystem::path& workspace) {
  const auto shared = select(values_, {"main_model", "reasoning", "access_mode",
                                       "file_access", "command_approval", "network",
                                       "high_risk_confirmation"});
  values_ = defaults(default_ui_scale_);
  merge(values_, shared);
  effort_ = "高";
  set("reasoning", effort_);
  values_to_view(workspace);
  set_status("已恢复默认值；点击“保存更改”后写入");
}

void SettingsController::set_status(std::string status) {
  view_model_.state().settings.status = std::move(status);
  view_model_.dirty();
}

tokmon::cbor::Value SettingsController::shared_values() {
  view_to_values();
  return select(values_, {"main_model", "reasoning", "access_mode",
                          "file_access", "command_approval", "network",
                          "high_risk_confirmation"});
}

tokmon::cbor::Value SettingsController::provider_configuration() const {
  const auto& view = view_model_.state().settings;
  return tokmon::cbor::object({
      {"name", provider_}, {"protocol", view.provider_protocol},
      {"endpoint", view.provider_endpoint}, {"model", view.main_model},
      {"auth", view.provider_auth}, {"secret_env", view.provider_secret_env},
      {"thinking", true}, {"default", true}, {"reasoning_effort", "high"},
      {"max_output_tokens", static_cast<std::int64_t>(4096)},
      {"max_attempts", static_cast<std::int64_t>(6)},
      {"retry_backoff_ms", static_cast<std::int64_t>(5000)}});
}

tokmon::cbor::Value SettingsController::provider_test() const {
  return tokmon::cbor::object({{"name", provider_},
      {"text", "Reply with TOKMON_PROVIDER_OK"}, {"surface", "desktop-ui"}});
}

tokmon::cbor::Value SettingsController::provider_secret() const {
  return tokmon::cbor::object({{"name", provider_},
      {"secret", view_model_.state().settings.provider_secret}});
}

void SettingsController::select_provider(std::string provider) {
  provider_ = std::move(provider);
  for (auto& row : view_model_.state().settings.providers) {
    row.selected = row.name == provider_;
    if (row.selected)
      model_ = row.model;
  }
  view_model_.state().settings.main_model = model_;
  sync_shell();
  view_model_.dirty();
}

void SettingsController::select_model(std::string model) {
  model_ = std::move(model);
  view_model_.state().settings.main_model = model_;
  set("main_model", model_);
  sync_shell();
}

void SettingsController::select_effort(std::string effort) {
  effort_ = std::move(effort);
  view_model_.state().settings.reasoning = effort_;
  set("reasoning", effort_);
  sync_shell();
}

void SettingsController::select_access(std::string access) {
  access_ = std::move(access);
  set("access_mode", access_);
  sync_shell();
}

void SettingsController::sync_shell() {
  auto& view = view_model_.state();
  view.active_model = model_.empty() ? "选择模型⌄" : model_ + "⌄";
  view.effort = effort_;
  view.access_label = access_ == "full" ? "完全访问" : "受限访问";
  view_model_.dirty();
}

std::string SettingsController::string(const std::string_view key,
                                       const std::string_view fallback) const {
  return get_string(values_, key, fallback);
}

std::int64_t SettingsController::integer(const std::string_view key,
                                         const std::int64_t fallback) const {
  return get_integer(values_, key, fallback);
}

bool SettingsController::boolean(const std::string_view key,
                                 const bool fallback) const {
  return get_bool(values_, key, fallback);
}

void SettingsController::set(std::string key, tokmon::cbor::Value value) {
  set_value(values_, std::move(key), std::move(value));
}

} // namespace tokmon::desk
