#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tokmon::desk {

enum class RightPanelTab { review, files, terminal, browser };

struct NavigationItem {
  std::string id;
  std::string title;
  std::string kind;
  int indent{0};
  bool expanded{true};
  bool selected{false};
};

struct ChatMessage {
  enum class Role { user, assistant, system };
  Role role{Role::assistant};
  std::string markdown;
  std::string timestamp;
  bool streaming{false};
};

struct AppState {
  std::filesystem::path workspace;
  std::string session_title{"新会话"};
  std::string project_name;
  std::string branch{"main"};
  std::string model_name;
  std::string daemon_status{"正在连接后台服务"};
  bool sidebar_visible{true};
  bool right_panel_visible{false};
  bool settings_open{false};
  bool thought_expanded{true};
  bool workflow_expanded{true};
  bool environment_open{false};
  RightPanelTab right_tab{RightPanelTab::review};
  std::vector<NavigationItem> navigation;
  std::vector<ChatMessage> messages;

  static AppState make_initial(std::filesystem::path workspace);
};

} // namespace tokmon::desk
