#pragma once

#include "markdown/markdown_ast.hpp"
#include "review/git_service.hpp"
#include "tokmon/photon.hpp"
#include "ui/desk_view_model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tokmon::desk {

inline constexpr std::size_t conversation_window_turns = 40;
inline constexpr std::size_t conversation_overscan_turns = 8;

class NavigationModel;

struct ConversationRenderResult {
  std::size_t total_turns{0};
  std::size_t first_turn{0};
  std::size_t last_turn{0};
  bool empty{true};
};

// Pure presentation mapper. It converts domain data to the stable structures
// consumed by RML templates and never manipulates the DOM itself.
class DeskRenderer {
public:
  explicit DeskRenderer(DeskViewModel& view_model);

  void navigation(const NavigationModel& model, std::string_view query,
                  const std::filesystem::path& active_workspace);
  void review(const GitSnapshot& snapshot);
  void branches(const std::vector<std::string>& branches,
                std::string_view current, std::string_view error = {});
  void review_loading();
  void branch_loading();
  void diff(const GitFileDiff& diff, std::string_view path, bool staged);
  void diff_error(std::string error);
  void close_diff();
  ConversationRenderResult conversation(
      const std::vector<tokmon::Photon>& photons, std::size_t window_start,
      std::size_t window_turns = conversation_window_turns,
      std::size_t overscan_turns = conversation_overscan_turns);
  [[nodiscard]] const std::string* conversation_copy_payload(
      std::string_view id) const;
  void slash_commands(
      const std::vector<std::pair<std::string_view, std::string_view>>& commands,
      std::size_t selected, bool visible);
  void rename_popover(std::string title);
  void choice_popover(std::string title,
                      std::vector<ComposerChoiceView> choices);
  void close_composer_popover();

private:
  DeskViewModel& view_model_;
  MarkdownParser markdown_;
  std::unordered_map<std::string, std::string> conversation_copy_payloads_;
};

} // namespace tokmon::desk
