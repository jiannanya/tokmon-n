#pragma once

#include "tokmon/cbor.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tokmon::desk {

struct DeskNavigationItem {
  std::string id;
  std::string ray;
  std::filesystem::path workspace;
  std::string kind;
  std::string title;
  int indent{0};
  bool selected{false};
  bool expanded{true};
  bool title_manual{true};
};

class NavigationModel final {
 public:
  explicit NavigationModel(std::filesystem::path default_workspace = {});

  [[nodiscard]] bool load(const tokmon::cbor::Value& encoded,
                          std::string& error);
  [[nodiscard]] tokmon::cbor::Value encode() const;
  [[nodiscard]] std::vector<std::size_t> visible(std::string query) const;
  [[nodiscard]] bool select(std::string_view id);
  [[nodiscard]] DeskNavigationItem* selected();
  [[nodiscard]] const DeskNavigationItem* selected() const;
  [[nodiscard]] DeskNavigationItem& ensure_workspace_project(
      const std::filesystem::path& workspace);
  [[nodiscard]] DeskNavigationItem& create_session(std::string title);
  [[nodiscard]] bool bind_selected_ray(std::string ray);
  [[nodiscard]] bool rename_selected(std::string title, bool manual);
  [[nodiscard]] std::filesystem::path selected_workspace() const;
  [[nodiscard]] const std::vector<DeskNavigationItem>& items() const noexcept;
  [[nodiscard]] std::vector<DeskNavigationItem>& items() noexcept;

 private:
  [[nodiscard]] std::size_t project_ancestor(std::size_t index) const;
  [[nodiscard]] std::filesystem::path inherited_workspace(
      std::size_t index) const;

  std::filesystem::path default_workspace_;
  std::vector<DeskNavigationItem> items_;
};

} // namespace tokmon::desk
