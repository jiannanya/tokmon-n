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
  bool expandable{false};
  bool expanded{false};
  bool group{false};
  bool project{false};
  bool session{false};
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
  bool worktree_hidden{true};
  bool staged_hidden{true};
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
  Rml::String user_copy_id;
  Rml::String reasoning_copy_id;
  Rml::String assistant_copy_id;
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
  Rml::String classes;
  Rml::String time;
  Rml::String tone;
  Rml::String role;
  Rml::String duration{"-"};
  Rml::String tokens{"-"};
  Rml::String turn_label;
  Rml::String request_label;
  Rml::String wrapper_classes;
  bool turn_start{false};
  bool row_visible{true};
  bool selected{false};
  bool failed{false};
};

struct TrajectorySegmentView {
  Rml::String sequence;
  Rml::String classes;
  Rml::String left;
  Rml::String width;
  Rml::String top;
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
  Rml::String theme_mode{"浅色"};
  Rml::String default_agent{"代码助手"};
  Rml::String global_rules{
      "优先使用 TypeScript 严格模式；遵循项目代码规范；代码注释使用中文。"};
  Rml::String mcp_approval{"高风险时询问"};
  Rml::String mcp_timeout{"60 秒"};
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
  bool agent_autonomous{true};
  bool agent_show_thoughts{true};
  bool agent_code_enabled{true};
  bool agent_architect_enabled{true};
  bool agent_translator_enabled{true};
  bool agent_analyst_enabled{true};
  bool skills_enabled{true};
  bool skills_auto_invoke{true};
  bool skill_customizations_enabled{true};
  bool skill_generative_ui_enabled{true};
  bool skill_refactor_enabled{true};
  bool skill_diagrams_enabled{true};
  bool rules_enabled{true};
  bool prefer_project_rules{true};
  bool mcp_auto_start{true};
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
  std::vector<TrajectorySegmentView> trajectory_segments;
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
  Rml::String trajectory_duration{"0ms"};
  Rml::String trajectory_turns{"0"};
  Rml::String trajectory_calls{"0"};
  Rml::String trajectory_tokens{"0"};
  Rml::String trajectory_filter{"全部"};
  Rml::String trajectory_match_count{"0"};
  Rml::String trajectory_visible_range{"显示 0 条，共 0 条"};
  Rml::String trajectory_zoom{"1×"};
  Rml::String trajectory_selection_left{"0%"};
  Rml::String trajectory_selection_width{"0%"};
  Rml::String trajectory_detail_title;
  Rml::String trajectory_detail_location;
  Rml::String trajectory_detail_status;
  Rml::String trajectory_detail_tab{"summary"};
  Rml::String trajectory_detail_summary;
  Rml::String trajectory_detail_payload;
  Rml::String trajectory_detail_result;
  Rml::String trajectory_detail_schema;
  Rml::String trajectory_detail_timing;
  Rml::String conversation_top_spacer{"0dp"};
  Rml::String conversation_bottom_spacer{"0dp"};
  bool file_open{false};

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
  bool trajectory_filtered_empty{false};
  bool trajectory_detail_visible{false};
  bool trajectory_detail_failed{false};
  bool trajectory_actual_duration{true};
  bool trajectory_turns_collapsed{false};
  bool trajectory_calls_collapsed{false};
  bool trajectory_follow_tail{true};
  bool trajectory_zoomed{false};
  bool trajectory_selection_visible{false};
  bool trajectory_timeline_dragging{false};
  bool slash_visible{false};
  bool slash_has_matches{false};
  bool chat_running{false};
  bool chat_stopping{false};
  bool navigation_context_visible{false};
  Rml::String navigation_context_top{"220dp"};
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
