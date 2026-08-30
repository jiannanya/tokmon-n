#include "ui/modules/desk_renderer.hpp"

#include "ui/desk_view_model.hpp"
#include "ui/navigation_model.hpp"

#include <algorithm>
#include <ranges>

namespace tokmon::desk {
namespace {

std::string photon_text(const tokmon::Photon& photon) {
  if (const auto* value = tokmon::cbor::find(photon.payload, "text"))
    return std::string(value->as_string());
  return {};
}

bool same_workspace(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
  std::error_code left_error, right_error;
  const auto normalized_left = std::filesystem::weakly_canonical(left, left_error);
  const auto normalized_right = std::filesystem::weakly_canonical(right, right_error);
  return !left_error && !right_error && normalized_left == normalized_right;
}

} // namespace

DeskRenderer::DeskRenderer(DeskViewModel& view_model) : view_model_(view_model) {}

void DeskRenderer::navigation(const NavigationModel& model,
                              const std::string_view query,
                              const std::filesystem::path& active_workspace) {
  auto& view = view_model_.state();
  view.navigation.clear();
  for (const auto index : model.visible(std::string(query))) {
    const auto& item = model.items()[index];
    const auto icon_name = item.kind == "group" ? "icon-06.svg"
        : item.kind == "project" ? "icon-08.svg" : "icon-09.svg";
    view.navigation.push_back({
        item.id,
        item.title,
        item.kind + (item.selected ? " selected" : ""),
        std::to_string(10 + item.indent * 18) + "px",
        item.kind == "session" ? "" : item.expanded ? "⌄" : "›",
        view.asset_root + "/figma/" + icon_name,
    });
  }
  view.navigation_empty = view.navigation.empty();
  view.projects.clear();
  for (const auto& item : model.items())
    if (item.kind == "project")
      view.projects.push_back({item.id, item.title,
          same_workspace(item.workspace.empty() ? active_workspace : item.workspace,
                         active_workspace)});
  view_model_.dirty();
}

void DeskRenderer::review_loading() {
  auto& view = view_model_.state();
  view.review_loading = true;
  view.review_has_files = false;
  view.review_title = "正在刷新更改…";
  view.review_detail = "Git 状态在工作线程读取";
  view_model_.dirty();
}

void DeskRenderer::review(const GitSnapshot& snapshot) {
  auto& view = view_model_.state();
  view.review_loading = false;
  view.branch = snapshot.branch.empty() ? "workspace" : snapshot.branch;
  view.review_count = std::to_string(snapshot.files.size());
  view.review_files.clear();
  if (!snapshot.repository) {
    view.review_title = "当前工作区不是 Git 仓库";
    view.review_detail = snapshot.error;
  } else if (snapshot.files.empty()) {
    view.review_title = "没有待审查的更改";
    view.review_detail = "工作区修改会在这里显示";
  } else {
    for (const auto& file : snapshot.files)
      view.review_files.push_back({
          file.path,
          std::string{file.index_status, file.worktree_status},
          file.worktree_status != ' ',
          file.index_status != ' ',
      });
  }
  view.review_has_files = !view.review_files.empty();
  view_model_.dirty();
}

void DeskRenderer::branch_loading() {
  auto& view = view_model_.state();
  view.branch_menu_loading = true;
  view.branch_menu_empty = false;
  view.branches.clear();
  view_model_.dirty();
}

void DeskRenderer::branches(const std::vector<std::string>& branches,
                            const std::string_view current,
                            const std::string_view error) {
  auto& view = view_model_.state();
  view.branch_menu_loading = false;
  view.branches.clear();
  for (const auto& branch : branches)
    view.branches.push_back({branch, branch, branch == current});
  view.branch_menu_empty = view.branches.empty();
  if (view.branch_menu_empty && !error.empty())
    view.review_detail = error;
  view_model_.dirty();
}

void DeskRenderer::diff(const GitFileDiff& diff, const std::string_view path,
                        const bool staged) {
  auto& view = view_model_.state();
  view.diff_path = path;
  view.diff_summary = std::string(staged ? "已暂存" : "工作区") + " · " +
      std::to_string(diff.hunks.size()) + " 个修改块 · " +
      std::to_string(diff.patch.size()) + " bytes";
  view.diff_hunks.clear();
  for (const auto& hunk : diff.hunks)
    view.diff_hunks.push_back({
        std::string(path), std::to_string(hunk.index), hunk.header,
        staged ? "unstage-hunk" : "stage-hunk",
        staged ? "取消暂存" : "暂存", !staged,
    });
  view.diff_error.clear();
  view.diff_error_visible = false;
  view.diff_visible = true;
  view_model_.dirty();
}

void DeskRenderer::diff_error(std::string error) {
  auto& view = view_model_.state();
  view.diff_error = std::move(error);
  view.diff_error_visible = true;
  view.diff_visible = true;
  view_model_.dirty();
}

void DeskRenderer::close_diff() {
  auto& view = view_model_.state();
  view.diff_visible = false;
  view.diff_error_visible = false;
  view.diff_hunks.clear();
  view_model_.dirty();
}

ConversationRenderResult DeskRenderer::conversation(
    const std::vector<tokmon::Photon>& photons, const std::size_t window_start,
    const std::size_t window_turns, const std::size_t overscan_turns) {
  struct Turn {
    std::string user;
    std::string reasoning;
    std::string assistant;
    std::vector<WorkflowStepView> workflow;
  };
  std::vector<Turn> turns;
  for (const auto& photon : photons) {
    if (photon.kind == "user.input" || photon.kind == "user.message") {
      turns.push_back({});
      turns.back().user = photon_text(photon);
      continue;
    }
    if (turns.empty())
      turns.push_back({});
    auto& turn = turns.back();
    if (photon.kind == "model.reasoning-chunk")
      turn.reasoning += photon_text(photon);
    else if (photon.kind == "model.content-chunk")
      turn.assistant += photon_text(photon);
    else if (photon.kind == "assistant.message") {
      const auto final_text = photon_text(photon);
      if (!final_text.empty())
        turn.assistant = final_text;
    } else if (photon.kind == "act.failed" || photon.kind == "model.failed") {
      const auto* detail = tokmon::cbor::find(photon.payload, "detail");
      if (!detail)
        detail = tokmon::cbor::find(photon.payload, "error");
      turn.workflow.push_back({"执行失败",
          detail ? std::string(detail->as_string()) : "请检查 Photon 轨迹", true});
    } else if (photon.kind.starts_with("tool.") ||
               photon.kind.starts_with("fs.") ||
               photon.kind.starts_with("task.")) {
      auto detail = photon_text(photon);
      if (detail.empty())
        detail = tokmon::cbor::diagnostic(photon.payload);
      if (detail.size() > 400)
        detail.resize(400);
      turn.workflow.push_back({photon.kind, std::move(detail), false});
    }
  }

  ConversationRenderResult result;
  result.total_turns = turns.size();
  const auto maximum_start = turns.size() > window_turns
      ? turns.size() - window_turns : 0;
  const auto clamped_start = std::min(window_start, maximum_start);
  result.first_turn = clamped_start > overscan_turns
      ? clamped_start - overscan_turns : 0;
  result.last_turn = std::min(turns.size(),
      clamped_start + window_turns + overscan_turns);

  auto& view = view_model_.state();
  view.conversation.clear();
  for (std::size_t index = result.first_turn; index < result.last_turn; ++index) {
    auto& turn = turns[index];
    ConversationTurnView item;
    item.has_user = !turn.user.empty();
    item.has_reasoning = !turn.reasoning.empty();
    item.has_assistant = !turn.assistant.empty();
    item.has_workflow = !turn.workflow.empty();
    if (item.has_user)
      item.user_rml = markdown_to_safe_rml(markdown_.parse(turn.user));
    if (item.has_reasoning)
      item.reasoning_rml = markdown_to_safe_rml(markdown_.parse(turn.reasoning));
    if (item.has_assistant)
      item.assistant_rml = markdown_to_safe_rml(markdown_.parse(turn.assistant));
    item.workflow = std::move(turn.workflow);
    view.conversation.push_back(std::move(item));
  }
  result.empty = std::ranges::all_of(turns, [](const Turn& turn) {
    return turn.user.empty() && turn.reasoning.empty() &&
           turn.assistant.empty() && turn.workflow.empty();
  });
  view.conversation_empty = result.empty;
  view.conversation_has_top_spacer = result.first_turn > 0;
  view.conversation_has_bottom_spacer = result.last_turn < turns.size();
  view.conversation_top_spacer =
      std::to_string(result.first_turn * 210) + "px";
  view.conversation_bottom_spacer =
      std::to_string((turns.size() - result.last_turn) * 210) + "px";
  view_model_.dirty();
  return result;
}

void DeskRenderer::trajectory(const std::vector<tokmon::Photon>& photons,
                              const std::string_view active_ray,
                              const std::uint64_t cursor) {
  auto& view = view_model_.state();
  view.trajectory.clear();
  view.trajectory_empty = photons.empty();
  view.trajectory_count = std::to_string(photons.size());
  view.trajectory_ray = active_ray.empty() ? "未绑定" : std::string(active_ray);
  view.trajectory_cursor = std::to_string(cursor);
  constexpr std::size_t maximum = 200;
  const auto first = photons.size() > maximum ? photons.size() - maximum : 0;
  view.trajectory_has_notice = first > 0;
  view.trajectory_window_notice = "较早的 " + std::to_string(first) +
      " 个 Photon 已虚拟化；滚动会话或导出轨迹可查看完整历史";
  for (auto iterator = photons.begin() + static_cast<std::ptrdiff_t>(first);
       iterator != photons.end(); ++iterator) {
    auto detail = tokmon::cbor::diagnostic(iterator->payload);
    if (detail.size() > 700)
      detail.resize(700);
    view.trajectory.push_back({std::to_string(iterator->sequence), iterator->kind,
        iterator->schema + " · " + std::to_string(iterator->committed_at_ms),
        std::move(detail)});
  }
  view_model_.dirty();
}

void DeskRenderer::slash_commands(
    const std::vector<std::pair<std::string_view, std::string_view>>& commands,
    const std::size_t selected, const bool visible) {
  auto& view = view_model_.state();
  view.slash_commands.clear();
  for (std::size_t index = 0; index < commands.size(); ++index)
    view.slash_commands.push_back({std::string(commands[index].first),
                                   std::string(commands[index].second),
                                   index == selected});
  const bool was_slash_visible = view.slash_visible;
  view.slash_visible = visible;
  view.slash_has_matches = !commands.empty();
  if (visible) {
    view.composer_popover_visible = true;
    view.rename_popover_visible = false;
    view.choice_popover_visible = false;
  } else if (was_slash_visible) {
    view.composer_popover_visible = false;
  }
  view_model_.dirty();
}

void DeskRenderer::rename_popover(std::string title) {
  auto& view = view_model_.state();
  view.rename_title = std::move(title);
  view.slash_visible = false;
  view.rename_popover_visible = true;
  view.choice_popover_visible = false;
  view.composer_popover_visible = true;
  view_model_.dirty();
}

void DeskRenderer::choice_popover(std::string title,
                                  std::vector<ComposerChoiceView> choices) {
  auto& view = view_model_.state();
  view.choice_title = std::move(title);
  view.composer_choices = std::move(choices);
  view.slash_visible = false;
  view.rename_popover_visible = false;
  view.choice_popover_visible = true;
  view.composer_popover_visible = true;
  view_model_.dirty();
}

void DeskRenderer::close_composer_popover() {
  auto& view = view_model_.state();
  view.slash_visible = false;
  view.rename_popover_visible = false;
  view.choice_popover_visible = false;
  view.composer_popover_visible = false;
  view_model_.dirty();
}

} // namespace tokmon::desk
