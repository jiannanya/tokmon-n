#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <vector>

namespace Rml { class Context; }

namespace tokmon::desk {

struct ChoiceView {
  Rml::String value;
  Rml::String label;
  bool selected{false};
};

struct ComposerChoiceView {
  Rml::String kind;
  Rml::String value;
  Rml::String label;
  Rml::String detail;
};

struct NavigationRowView {
  Rml::String id;
  Rml::String title;
  Rml::String classes;
  Rml::String padding;
  Rml::String chevron;
  Rml::String icon;
};

struct TerminalTabView {
  Rml::String id;
  Rml::String title;
  bool active{false};
};

struct ReviewFileView {
  Rml::String path;
  Rml::String status;
  bool worktree{false};
  bool staged{false};
};

struct DiffHunkView {
  Rml::String path;
  Rml::String index;
  Rml::String header;
  Rml::String action;
  Rml::String action_label;
  bool discardable{false};
};

struct WorkflowStepView {
  Rml::String kind;
  Rml::String detail;
  bool failed{false};
};

struct ConversationTurnView {
  Rml::String user_rml;
  Rml::String reasoning_rml;
  Rml::String assistant_rml;
  std::vector<WorkflowStepView> workflow;
  bool has_user{false};
  bool has_reasoning{false};
  bool has_assistant{false};
  bool has_workflow{false};
};

struct TrajectoryRowView {
  Rml::String sequence;
  Rml::String kind;
  Rml::String metadata;
  Rml::String detail;
};

struct SlashCommandView {
  Rml::String command;
  Rml::String description;
  bool selected{false};
};

struct ProviderView {
  Rml::String name;
  Rml::String model;
  Rml::String credential_source;
  bool selected{false};
};

struct SettingsView {
  Rml::String page{"general"};
  Rml::String title{"通用"};
  Rml::String description{"管理语言、启动、自动保存和更新偏好"};
  Rml::String status{"已从当前工作区配置载入"};

  Rml::String language{"简体中文"};
  Rml::String startup{"首页"};
  Rml::String autosave{"5 分钟"};
  Rml::String update_channel{"稳定版"};
  Rml::String main_model;
  Rml::String reasoning{"高"};
  Rml::String file_access{"受信路径"};
  Rml::String command_approval{"按需确认"};
  Rml::String index_mode{"标准"};
  Rml::String quiet_hours{"关闭"};
  Rml::String density{"舒适"};
  Rml::String nickname;
  Rml::String email;
  Rml::String terminal_profile{"auto"};
  Rml::String terminal_executable;
  Rml::String terminal_arguments;
  Rml::String provider_protocol{"openai-compatible"};
  Rml::String provider_endpoint;
  Rml::String provider_auth{"bearer"};
  Rml::String provider_secret_env;
  Rml::String provider_secret;
  Rml::String browser_executable{"未自动发现"};
  Rml::String workspace_path;

  bool network{true};
  bool high_risk_confirmation{true};
  bool workspace_sync{true};
  bool git{true};
  bool notifications{true};
  bool desktop_notifications{true};
  bool message_alerts{true};
  bool cloud_sync{false};
  bool browser_high_risk_confirmation{true};
  int ui_scale{125};
  int font_scale{100};
  int terminal_font_size{13};
  int terminal_scrollback{10000};

  std::vector<ProviderView> providers;
  std::vector<ChoiceView> terminal_profiles;
};

struct DeskViewState {
  Rml::String asset_root;
  Rml::String workspace_path;
  Rml::String workspace_name;
  Rml::String session_title{"新会话"};
  Rml::String active_model{"选择模型⌄"};
  Rml::String effort{"高"};
  Rml::String access_label{"完全访问"};
  Rml::String branch{"无 Git"};

  std::vector<NavigationRowView> navigation;
  std::vector<ChoiceView> projects;
  std::vector<TerminalTabView> terminal_tabs;
  std::vector<ReviewFileView> review_files;
  std::vector<ChoiceView> branches;
  std::vector<DiffHunkView> diff_hunks;
  std::vector<ConversationTurnView> conversation;
  std::vector<TrajectoryRowView> trajectory;
  std::vector<SlashCommandView> slash_commands;
  std::vector<ComposerChoiceView> composer_choices;

  Rml::String review_title{"没有待审查的更改"};
  Rml::String review_detail{"工作区修改会在这里显示"};
  Rml::String review_count{"0"};
  Rml::String diff_path;
  Rml::String diff_summary;
  Rml::String slash_empty{"没有匹配命令"};
  Rml::String trajectory_count{"0"};
  Rml::String trajectory_ray{"未绑定"};
  Rml::String trajectory_cursor{"0"};
  Rml::String trajectory_window_notice;
  Rml::String conversation_top_spacer{"0px"};
  Rml::String conversation_bottom_spacer{"0px"};

  bool navigation_empty{false};
  bool review_loading{false};
  bool review_has_files{false};
  bool branch_menu_loading{false};
  bool branch_menu_empty{false};
  bool diff_visible{false};
  bool diff_error_visible{false};
  Rml::String diff_error;
  bool conversation_empty{true};
  bool conversation_has_top_spacer{false};
  bool conversation_has_bottom_spacer{false};
  bool trajectory_empty{true};
  bool trajectory_has_notice{false};
  bool slash_visible{false};
  bool slash_has_matches{false};
  bool composer_popover_visible{false};
  bool rename_popover_visible{false};
  bool choice_popover_visible{false};
  Rml::String rename_title;
  Rml::String choice_title;

  Rml::String browser_title{"Agent Browser"};
  Rml::String browser_detail{"优先使用系统 Chrome/Chromium 与独立 Tokmon Profile"};
  Rml::String browser_url{"https://example.com"};
  Rml::String browser_preview;
  Rml::String browser_snapshot;
  Rml::String browser_permission{"独立 Tokmon Profile · 仅显式操作网页"};
  Rml::String browser_takeover_label{"用户接管"};
  bool browser_running{false};
  bool browser_takeover{false};
  bool browser_preview_visible{false};

  SettingsView settings;
};

class DeskViewModel {
public:
  [[nodiscard]] bool bind(Rml::Context& context, std::string asset_root);
  void dirty();

  [[nodiscard]] DeskViewState& state() noexcept { return state_; }
  [[nodiscard]] const DeskViewState& state() const noexcept { return state_; }

private:
  DeskViewState state_;
  Rml::DataModelHandle handle_;
};

} // namespace tokmon::desk
