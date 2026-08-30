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
      {"theme_mode", "浅色"}, {"default_agent", "代码助手"},
      {"global_rules", "优先使用 TypeScript 严格模式；遵循项目代码规范；代码注释使用中文。"},
      {"mcp_approval", "高风险时询问"}, {"mcp_timeout", "60 秒"},
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
      {"browser_high_risk_confirmation", true},
      {"agent_autonomous", true}, {"agent_show_thoughts", true},
      {"agent_code_enabled", true}, {"agent_architect_enabled", true},
      {"agent_translator_enabled", true}, {"agent_analyst_enabled", true},
      {"skills_enabled", true}, {"skills_auto_invoke", true},
      {"skill_customizations_enabled", true},
      {"skill_generative_ui_enabled", true},
      {"skill_refactor_enabled", true}, {"skill_diagrams_enabled", true},
      {"rules_enabled", true}, {"prefer_project_rules", true},
      {"mcp_auto_start", true}});
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
      {"model", {"模型", "配置现有 tokmon-daemon 模型 Provider"}},
      {"agents", {"智能体", "配置默认角色、任务派发和可用智能体"}},
      {"skills", {"Skill 技能", "管理技能系统、自动唤起与已安装技能"}},
      {"rules", {"规则", "管理项目规则优先级与全局任务偏好"}},
      {"mcp", {"MCP 服务", "管理工具协议的启动、审批和超时策略"}},
      {"access", {"权限与安全", "控制文件、命令、网络和高风险操作"}},
      {"workspace", {"工作区", "管理当前工作区和索引行为"}},
      {"notifications", {"通知", "配置任务完成和消息提醒"}},
      {"appearance", {"外观", "选择雅绿主题并调整密度与界面缩放"}},
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
  view.theme_mode = string("theme_mode", "浅色");
  view.default_agent = string("default_agent", "代码助手");
  view.global_rules = string(
      "global_rules",
      "优先使用 TypeScript 严格模式；遵循项目代码规范；代码注释使用中文。");
  view.mcp_approval = string("mcp_approval", "高风险时询问");
  view.mcp_timeout = string("mcp_timeout", "60 秒");
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
  view.agent_autonomous = boolean("agent_autonomous", true);
  view.agent_show_thoughts = boolean("agent_show_thoughts", true);
  view.agent_code_enabled = boolean("agent_code_enabled", true);
  view.agent_architect_enabled = boolean("agent_architect_enabled", true);
  view.agent_translator_enabled = boolean("agent_translator_enabled", true);
  view.agent_analyst_enabled = boolean("agent_analyst_enabled", true);
  view.skills_enabled = boolean("skills_enabled", true);
  view.skills_auto_invoke = boolean("skills_auto_invoke", true);
  view.skill_customizations_enabled =
      boolean("skill_customizations_enabled", true);
  view.skill_generative_ui_enabled =
      boolean("skill_generative_ui_enabled", true);
  view.skill_refactor_enabled = boolean("skill_refactor_enabled", true);
  view.skill_diagrams_enabled = boolean("skill_diagrams_enabled", true);
  view.rules_enabled = boolean("rules_enabled", true);
  view.prefer_project_rules = boolean("prefer_project_rules", true);
  view.mcp_auto_start = boolean("mcp_auto_start", true);
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
  set("density", view.density); set("theme_mode", view.theme_mode);
  set("default_agent", view.default_agent); set("global_rules", view.global_rules);
  set("mcp_approval", view.mcp_approval); set("mcp_timeout", view.mcp_timeout);
  set("nickname", view.nickname);
  set("email", view.email); set("terminal_profile", view.terminal_profile);
  set("terminal_executable", view.terminal_executable);
  set("terminal_arguments", view.terminal_arguments);
  set("network", view.network); set("high_risk_confirmation", view.high_risk_confirmation);
  set("workspace_sync", view.workspace_sync); set("git", view.git);
  set("notifications", view.notifications);
  set("desktop_notifications", view.desktop_notifications);
  set("message_alerts", view.message_alerts); set("cloud_sync", view.cloud_sync);
  set("browser_high_risk_confirmation", view.browser_high_risk_confirmation);
  set("agent_autonomous", view.agent_autonomous);
  set("agent_show_thoughts", view.agent_show_thoughts);
  set("agent_code_enabled", view.agent_code_enabled);
  set("agent_architect_enabled", view.agent_architect_enabled);
  set("agent_translator_enabled", view.agent_translator_enabled);
  set("agent_analyst_enabled", view.agent_analyst_enabled);
  set("skills_enabled", view.skills_enabled);
  set("skills_auto_invoke", view.skills_auto_invoke);
  set("skill_customizations_enabled", view.skill_customizations_enabled);
  set("skill_generative_ui_enabled", view.skill_generative_ui_enabled);
  set("skill_refactor_enabled", view.skill_refactor_enabled);
  set("skill_diagrams_enabled", view.skill_diagrams_enabled);
  set("rules_enabled", view.rules_enabled);
  set("prefer_project_rules", view.prefer_project_rules);
  set("mcp_auto_start", view.mcp_auto_start);
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

void SettingsController::toggle(const std::string_view key) {
  auto& view = view_model_.state().settings;
#define TOKMON_TOGGLE_SETTING(name) \
  if (key == #name) { view.name = !view.name; set(#name, view.name); view_model_.dirty(); return; }
  TOKMON_TOGGLE_SETTING(network)
  TOKMON_TOGGLE_SETTING(high_risk_confirmation)
  TOKMON_TOGGLE_SETTING(workspace_sync)
  TOKMON_TOGGLE_SETTING(git)
  TOKMON_TOGGLE_SETTING(notifications)
  TOKMON_TOGGLE_SETTING(desktop_notifications)
  TOKMON_TOGGLE_SETTING(message_alerts)
  TOKMON_TOGGLE_SETTING(cloud_sync)
  TOKMON_TOGGLE_SETTING(browser_high_risk_confirmation)
  TOKMON_TOGGLE_SETTING(agent_autonomous)
  TOKMON_TOGGLE_SETTING(agent_show_thoughts)
  TOKMON_TOGGLE_SETTING(agent_code_enabled)
  TOKMON_TOGGLE_SETTING(agent_architect_enabled)
  TOKMON_TOGGLE_SETTING(agent_translator_enabled)
  TOKMON_TOGGLE_SETTING(agent_analyst_enabled)
  TOKMON_TOGGLE_SETTING(skills_enabled)
  TOKMON_TOGGLE_SETTING(skills_auto_invoke)
  TOKMON_TOGGLE_SETTING(skill_customizations_enabled)
  TOKMON_TOGGLE_SETTING(skill_generative_ui_enabled)
  TOKMON_TOGGLE_SETTING(skill_refactor_enabled)
  TOKMON_TOGGLE_SETTING(skill_diagrams_enabled)
  TOKMON_TOGGLE_SETTING(rules_enabled)
  TOKMON_TOGGLE_SETTING(prefer_project_rules)
  TOKMON_TOGGLE_SETTING(mcp_auto_start)
#undef TOKMON_TOGGLE_SETTING
}

void SettingsController::choose(const std::string_view key, std::string value) {
  auto& view = view_model_.state().settings;
  if (key == "startup") view.startup = value;
  else if (key == "theme_mode") view.theme_mode = value;
  else if (key == "mcp_approval") view.mcp_approval = value;
  else return;
  set(std::string(key), std::move(value));
  view_model_.dirty();
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
