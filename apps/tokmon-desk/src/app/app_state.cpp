#include "app/app_state.hpp"

#include <utility>

namespace tokmon::desk {

AppState AppState::make_initial(std::filesystem::path workspace_path) {
  AppState state;
  state.workspace = std::move(workspace_path);
  if (!state.workspace.empty()) {
    const auto name = state.workspace.filename().u8string();
    if (!name.empty())
      state.project_name.assign(reinterpret_cast<const char*>(name.data()), name.size());
  }
  state.navigation = {{"workspace", state.project_name, "project", 0,
                       true, true}};
  state.messages.clear();
  return state;
}

} // namespace tokmon::desk
