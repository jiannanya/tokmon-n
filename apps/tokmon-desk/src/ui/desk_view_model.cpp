#include "ui/desk_view_model.hpp"

#include <RmlUi/Core/Context.h>

namespace tokmon::desk {
bool DeskViewModel::bind(Rml::Context& context, std::string asset_root) {
  state_.asset_root = std::move(asset_root);
  auto model = context.CreateDataModel("desk");
  if (!model)
    return false;

  auto choice = model.RegisterStruct<ChoiceView>();
  if (!choice || !choice.RegisterMember("value", &ChoiceView::value) ||
      !choice.RegisterMember("label", &ChoiceView::label) ||
      !choice.RegisterMember("selected", &ChoiceView::selected) ||
      !model.RegisterArray<std::vector<ChoiceView>>())
    return false;

  auto composer_choice = model.RegisterStruct<ComposerChoiceView>();
  if (!composer_choice ||
      !composer_choice.RegisterMember("kind", &ComposerChoiceView::kind) ||
      !composer_choice.RegisterMember("value", &ComposerChoiceView::value) ||
      !composer_choice.RegisterMember("label", &ComposerChoiceView::label) ||
      !composer_choice.RegisterMember("detail", &ComposerChoiceView::detail) ||
      !model.RegisterArray<std::vector<ComposerChoiceView>>())
    return false;

  auto navigation = model.RegisterStruct<NavigationRowView>();
  if (!navigation ||
      !navigation.RegisterMember("id", &NavigationRowView::id) ||
      !navigation.RegisterMember("title", &NavigationRowView::title) ||
      !navigation.RegisterMember("classes", &NavigationRowView::classes) ||
      !navigation.RegisterMember("padding", &NavigationRowView::padding) ||
      !navigation.RegisterMember("chevron", &NavigationRowView::chevron) ||
      !navigation.RegisterMember("icon", &NavigationRowView::icon) ||
      !navigation.RegisterMember("expandable", &NavigationRowView::expandable) ||
      !navigation.RegisterMember("expanded", &NavigationRowView::expanded) ||
      !navigation.RegisterMember("group", &NavigationRowView::group) ||
      !navigation.RegisterMember("project", &NavigationRowView::project) ||
      !navigation.RegisterMember("session", &NavigationRowView::session) ||
      !model.RegisterArray<std::vector<NavigationRowView>>())
    return false;

  auto terminal = model.RegisterStruct<TerminalTabView>();
  if (!terminal || !terminal.RegisterMember("id", &TerminalTabView::id) ||
      !terminal.RegisterMember("title", &TerminalTabView::title) ||
      !terminal.RegisterMember("active", &TerminalTabView::active) ||
      !model.RegisterArray<std::vector<TerminalTabView>>())
    return false;

  auto review = model.RegisterStruct<ReviewFileView>();
  if (!review || !review.RegisterMember("path", &ReviewFileView::path) ||
      !review.RegisterMember("status", &ReviewFileView::status) ||
      !review.RegisterMember("worktree", &ReviewFileView::worktree) ||
      !review.RegisterMember("staged", &ReviewFileView::staged) ||
      !review.RegisterMember("worktree_hidden", &ReviewFileView::worktree_hidden) ||
      !review.RegisterMember("staged_hidden", &ReviewFileView::staged_hidden) ||
      !model.RegisterArray<std::vector<ReviewFileView>>())
    return false;

  auto hunk = model.RegisterStruct<DiffHunkView>();
  if (!hunk || !hunk.RegisterMember("path", &DiffHunkView::path) ||
      !hunk.RegisterMember("index", &DiffHunkView::index) ||
      !hunk.RegisterMember("header", &DiffHunkView::header) ||
      !hunk.RegisterMember("action", &DiffHunkView::action) ||
      !hunk.RegisterMember("action_label", &DiffHunkView::action_label) ||
      !hunk.RegisterMember("discardable", &DiffHunkView::discardable) ||
      !model.RegisterArray<std::vector<DiffHunkView>>())
    return false;

  auto workflow = model.RegisterStruct<WorkflowStepView>();
  if (!workflow ||
      !workflow.RegisterMember("kind", &WorkflowStepView::kind) ||
      !workflow.RegisterMember("detail", &WorkflowStepView::detail) ||
      !workflow.RegisterMember("failed", &WorkflowStepView::failed) ||
      !model.RegisterArray<std::vector<WorkflowStepView>>())
    return false;

  auto turn = model.RegisterStruct<ConversationTurnView>();
  if (!turn ||
      !turn.RegisterMember("user_rml", &ConversationTurnView::user_rml) ||
      !turn.RegisterMember("reasoning_rml", &ConversationTurnView::reasoning_rml) ||
      !turn.RegisterMember("assistant_rml", &ConversationTurnView::assistant_rml) ||
      !turn.RegisterMember("user_copy_id", &ConversationTurnView::user_copy_id) ||
      !turn.RegisterMember("reasoning_copy_id", &ConversationTurnView::reasoning_copy_id) ||
      !turn.RegisterMember("assistant_copy_id", &ConversationTurnView::assistant_copy_id) ||
      !turn.RegisterMember("workflow", &ConversationTurnView::workflow) ||
      !turn.RegisterMember("has_user", &ConversationTurnView::has_user) ||
      !turn.RegisterMember("has_reasoning", &ConversationTurnView::has_reasoning) ||
      !turn.RegisterMember("has_assistant", &ConversationTurnView::has_assistant) ||
      !turn.RegisterMember("has_workflow", &ConversationTurnView::has_workflow) ||
      !model.RegisterArray<std::vector<ConversationTurnView>>())
    return false;

  auto trajectory = model.RegisterStruct<TrajectoryRowView>();
  if (!trajectory ||
      !trajectory.RegisterMember("sequence", &TrajectoryRowView::sequence) ||
      !trajectory.RegisterMember("kind", &TrajectoryRowView::kind) ||
      !trajectory.RegisterMember("metadata", &TrajectoryRowView::metadata) ||
      !trajectory.RegisterMember("detail", &TrajectoryRowView::detail) ||
      !trajectory.RegisterMember("classes", &TrajectoryRowView::classes) ||
      !trajectory.RegisterMember("time", &TrajectoryRowView::time) ||
      !trajectory.RegisterMember("tone", &TrajectoryRowView::tone) ||
      !trajectory.RegisterMember("role", &TrajectoryRowView::role) ||
      !trajectory.RegisterMember("duration", &TrajectoryRowView::duration) ||
      !trajectory.RegisterMember("tokens", &TrajectoryRowView::tokens) ||
      !trajectory.RegisterMember("turn_label", &TrajectoryRowView::turn_label) ||
      !trajectory.RegisterMember("request_label", &TrajectoryRowView::request_label) ||
      !trajectory.RegisterMember("wrapper_classes", &TrajectoryRowView::wrapper_classes) ||
      !trajectory.RegisterMember("turn_start", &TrajectoryRowView::turn_start) ||
      !trajectory.RegisterMember("row_visible", &TrajectoryRowView::row_visible) ||
      !trajectory.RegisterMember("selected", &TrajectoryRowView::selected) ||
      !trajectory.RegisterMember("failed", &TrajectoryRowView::failed) ||
      !model.RegisterArray<std::vector<TrajectoryRowView>>())
    return false;

  auto trajectory_segment = model.RegisterStruct<TrajectorySegmentView>();
  if (!trajectory_segment ||
      !trajectory_segment.RegisterMember("sequence", &TrajectorySegmentView::sequence) ||
      !trajectory_segment.RegisterMember("classes", &TrajectorySegmentView::classes) ||
      !trajectory_segment.RegisterMember("left", &TrajectorySegmentView::left) ||
      !trajectory_segment.RegisterMember("width", &TrajectorySegmentView::width) ||
      !trajectory_segment.RegisterMember("top", &TrajectorySegmentView::top) ||
      !model.RegisterArray<std::vector<TrajectorySegmentView>>())
    return false;

  auto slash = model.RegisterStruct<SlashCommandView>();
  if (!slash || !slash.RegisterMember("command", &SlashCommandView::command) ||
      !slash.RegisterMember("description", &SlashCommandView::description) ||
      !slash.RegisterMember("selected", &SlashCommandView::selected) ||
      !model.RegisterArray<std::vector<SlashCommandView>>())
    return false;

  auto provider = model.RegisterStruct<ProviderView>();
  if (!provider || !provider.RegisterMember("name", &ProviderView::name) ||
      !provider.RegisterMember("model", &ProviderView::model) ||
      !provider.RegisterMember("credential_source", &ProviderView::credential_source) ||
      !provider.RegisterMember("selected", &ProviderView::selected) ||
      !model.RegisterArray<std::vector<ProviderView>>())
    return false;

  auto settings = model.RegisterStruct<SettingsView>();
  if (!settings)
    return false;
#define TOKMON_BIND_SETTING(name) \
  if (!settings.RegisterMember(#name, &SettingsView::name)) return false
  TOKMON_BIND_SETTING(page); TOKMON_BIND_SETTING(title);
  TOKMON_BIND_SETTING(description); TOKMON_BIND_SETTING(status);
  TOKMON_BIND_SETTING(language); TOKMON_BIND_SETTING(startup);
  TOKMON_BIND_SETTING(autosave); TOKMON_BIND_SETTING(update_channel);
  TOKMON_BIND_SETTING(main_model); TOKMON_BIND_SETTING(reasoning);
  TOKMON_BIND_SETTING(file_access); TOKMON_BIND_SETTING(command_approval);
  TOKMON_BIND_SETTING(index_mode); TOKMON_BIND_SETTING(quiet_hours);
  TOKMON_BIND_SETTING(density); TOKMON_BIND_SETTING(nickname);
  TOKMON_BIND_SETTING(email); TOKMON_BIND_SETTING(terminal_profile);
  TOKMON_BIND_SETTING(terminal_executable); TOKMON_BIND_SETTING(terminal_arguments);
  TOKMON_BIND_SETTING(provider_protocol); TOKMON_BIND_SETTING(provider_endpoint);
  TOKMON_BIND_SETTING(provider_auth); TOKMON_BIND_SETTING(provider_secret_env);
  TOKMON_BIND_SETTING(provider_secret); TOKMON_BIND_SETTING(browser_executable);
  TOKMON_BIND_SETTING(workspace_path); TOKMON_BIND_SETTING(network);
  TOKMON_BIND_SETTING(high_risk_confirmation); TOKMON_BIND_SETTING(workspace_sync);
  TOKMON_BIND_SETTING(git); TOKMON_BIND_SETTING(notifications);
  TOKMON_BIND_SETTING(desktop_notifications); TOKMON_BIND_SETTING(message_alerts);
  TOKMON_BIND_SETTING(cloud_sync); TOKMON_BIND_SETTING(browser_high_risk_confirmation);
  TOKMON_BIND_SETTING(ui_scale); TOKMON_BIND_SETTING(font_scale);
  TOKMON_BIND_SETTING(terminal_font_size); TOKMON_BIND_SETTING(terminal_scrollback);
  TOKMON_BIND_SETTING(providers); TOKMON_BIND_SETTING(terminal_profiles);
#undef TOKMON_BIND_SETTING

  auto state = model.RegisterStruct<DeskViewState>();
  if (!state)
    return false;
#define TOKMON_BIND_STATE(name) \
  if (!state.RegisterMember(#name, &DeskViewState::name)) return false
  TOKMON_BIND_STATE(asset_root); TOKMON_BIND_STATE(workspace_path);
  TOKMON_BIND_STATE(workspace_name); TOKMON_BIND_STATE(session_title);
  TOKMON_BIND_STATE(active_model); TOKMON_BIND_STATE(effort);
  TOKMON_BIND_STATE(access_label); TOKMON_BIND_STATE(branch);
  TOKMON_BIND_STATE(navigation); TOKMON_BIND_STATE(projects);
  TOKMON_BIND_STATE(terminal_tabs); TOKMON_BIND_STATE(review_files);
  TOKMON_BIND_STATE(branches); TOKMON_BIND_STATE(diff_hunks);
  TOKMON_BIND_STATE(conversation); TOKMON_BIND_STATE(trajectory);
  TOKMON_BIND_STATE(trajectory_segments);
  TOKMON_BIND_STATE(slash_commands); TOKMON_BIND_STATE(composer_choices);
  TOKMON_BIND_STATE(review_title);
  TOKMON_BIND_STATE(review_detail); TOKMON_BIND_STATE(review_count);
  TOKMON_BIND_STATE(diff_path); TOKMON_BIND_STATE(diff_summary);
  TOKMON_BIND_STATE(slash_empty); TOKMON_BIND_STATE(trajectory_count);
  TOKMON_BIND_STATE(trajectory_ray); TOKMON_BIND_STATE(trajectory_cursor);
  TOKMON_BIND_STATE(trajectory_window_notice);
  TOKMON_BIND_STATE(trajectory_duration); TOKMON_BIND_STATE(trajectory_turns);
  TOKMON_BIND_STATE(trajectory_calls); TOKMON_BIND_STATE(trajectory_tokens);
  TOKMON_BIND_STATE(trajectory_filter); TOKMON_BIND_STATE(trajectory_match_count);
  TOKMON_BIND_STATE(trajectory_visible_range);
  TOKMON_BIND_STATE(trajectory_zoom);
  TOKMON_BIND_STATE(trajectory_selection_left);
  TOKMON_BIND_STATE(trajectory_selection_width);
  TOKMON_BIND_STATE(trajectory_detail_title);
  TOKMON_BIND_STATE(trajectory_detail_location);
  TOKMON_BIND_STATE(trajectory_detail_status);
  TOKMON_BIND_STATE(trajectory_detail_tab);
  TOKMON_BIND_STATE(trajectory_detail_summary);
  TOKMON_BIND_STATE(trajectory_detail_payload);
  TOKMON_BIND_STATE(trajectory_detail_result);
  TOKMON_BIND_STATE(trajectory_detail_schema);
  TOKMON_BIND_STATE(trajectory_detail_timing);
  TOKMON_BIND_STATE(conversation_top_spacer);
  TOKMON_BIND_STATE(conversation_bottom_spacer);
  TOKMON_BIND_STATE(file_open);
  TOKMON_BIND_STATE(navigation_empty); TOKMON_BIND_STATE(review_loading);
  TOKMON_BIND_STATE(review_has_files); TOKMON_BIND_STATE(branch_menu_loading);
  TOKMON_BIND_STATE(branch_menu_empty); TOKMON_BIND_STATE(diff_visible);
  TOKMON_BIND_STATE(diff_error_visible); TOKMON_BIND_STATE(diff_error);
  TOKMON_BIND_STATE(conversation_empty);
  TOKMON_BIND_STATE(conversation_has_top_spacer);
  TOKMON_BIND_STATE(conversation_has_bottom_spacer);
  TOKMON_BIND_STATE(trajectory_empty); TOKMON_BIND_STATE(trajectory_has_notice);
  TOKMON_BIND_STATE(trajectory_filtered_empty);
  TOKMON_BIND_STATE(trajectory_detail_visible);
  TOKMON_BIND_STATE(trajectory_detail_failed);
  TOKMON_BIND_STATE(trajectory_actual_duration);
  TOKMON_BIND_STATE(trajectory_turns_collapsed);
  TOKMON_BIND_STATE(trajectory_calls_collapsed);
  TOKMON_BIND_STATE(trajectory_follow_tail);
  TOKMON_BIND_STATE(trajectory_zoomed);
  TOKMON_BIND_STATE(trajectory_selection_visible);
  TOKMON_BIND_STATE(trajectory_timeline_dragging);
  TOKMON_BIND_STATE(slash_visible); TOKMON_BIND_STATE(slash_has_matches);
  TOKMON_BIND_STATE(chat_running); TOKMON_BIND_STATE(chat_stopping);
  TOKMON_BIND_STATE(navigation_context_visible);
  TOKMON_BIND_STATE(navigation_context_top);
  TOKMON_BIND_STATE(composer_popover_visible);
  TOKMON_BIND_STATE(rename_popover_visible);
  TOKMON_BIND_STATE(choice_popover_visible); TOKMON_BIND_STATE(rename_title);
  TOKMON_BIND_STATE(choice_title);
  TOKMON_BIND_STATE(browser_title); TOKMON_BIND_STATE(browser_detail);
  TOKMON_BIND_STATE(browser_url); TOKMON_BIND_STATE(browser_preview);
  TOKMON_BIND_STATE(browser_snapshot); TOKMON_BIND_STATE(browser_permission);
  TOKMON_BIND_STATE(browser_takeover_label); TOKMON_BIND_STATE(browser_running);
  TOKMON_BIND_STATE(browser_takeover); TOKMON_BIND_STATE(browser_preview_visible);
  TOKMON_BIND_STATE(settings);
#undef TOKMON_BIND_STATE

  if (!model.Bind("view", &state_))
    return false;
  handle_ = model.GetModelHandle();
  return true;
}

void DeskViewModel::dirty() {
  if (handle_)
    handle_.DirtyVariable("view");
}

} // namespace tokmon::desk
