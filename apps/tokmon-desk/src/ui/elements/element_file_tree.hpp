#pragma once

#include "workspace/workspace_service.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Texture.h>

#include <optional>
#include <string>
#include <vector>

namespace tokmon::desk {

// Virtual file tree viewport. The workspace model may contain thousands of
// rows, but only the rows intersecting this element are shaped and rendered.
class ElementFileTree final : public Rml::Element {
 public:
  explicit ElementFileTree(const Rml::String& tag);

  void set_rows(std::vector<WorkspaceEntry> rows);
  void set_selected(std::string relative_path);
  [[nodiscard]] std::optional<WorkspaceEntry> row_at(float local_y) const;
  [[nodiscard]] std::optional<WorkspaceEntry> selected_row() const;
  [[nodiscard]] std::optional<WorkspaceEntry> move_selection(int rows);
  [[nodiscard]] std::optional<WorkspaceEntry> select_edge(bool last);
  void scroll_lines(int lines);
  [[nodiscard]] std::size_t visible_geometry_rows() const noexcept {
    return rendered_rows_;
  }
  void OnRender() override;

 private:
  struct TextGeometry {
    Rml::Geometry geometry;
    Rml::Texture texture;
  };
  void rebuild_geometry(Rml::Vector2f size);

  std::vector<WorkspaceEntry> rows_;
  std::string selected_;
  std::size_t first_row_{0};
  std::size_t rendered_rows_{0};
  Rml::Geometry decoration_geometry_;
  std::vector<TextGeometry> text_geometry_;
  Rml::Vector2f geometry_size_{};
  std::uint64_t revision_{0};
  std::uint64_t theme_revision_{0};
  std::uint64_t geometry_revision_{~std::uint64_t{0}};
};

void register_file_tree_element();

} // namespace tokmon::desk
