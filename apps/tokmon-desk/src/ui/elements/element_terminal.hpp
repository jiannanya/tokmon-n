#pragma once

#include "terminal/terminal_service.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Texture.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tokmon::desk {

// A single replaced RmlUi element that batch-renders libghostty-vt's grid.
// It deliberately avoids a DOM node per terminal cell.
class ElementTerminal final : public Rml::Element {
public:
  explicit ElementTerminal(const Rml::String& tag);
  void set_snapshot(TerminalRenderSnapshot snapshot);
  void begin_selection(float x, float y);
  void update_selection(float x, float y);
  void end_selection();
  void clear_selection();
  [[nodiscard]] std::string selected_text() const;
  [[nodiscard]] std::string hyperlink_at(float x, float y);
  void set_search(std::string query);
  [[nodiscard]] std::size_t search_match_count() const noexcept {
    return search_match_count_;
  }
  [[nodiscard]] bool selecting() const noexcept { return selecting_; }
  [[nodiscard]] std::uint16_t grid_columns() const noexcept {
    return snapshot_.columns;
  }
  [[nodiscard]] std::uint16_t grid_rows() const noexcept {
    return snapshot_.rows;
  }
  void OnRender() override;

private:
  struct TextGeometry {
    Rml::Geometry geometry;
    Rml::Texture texture;
  };

  void rebuild_geometry(Rml::Vector2f size);
  [[nodiscard]] std::size_t cell_at(float x, float y);
  void apply_selection();

  TerminalRenderSnapshot snapshot_;
  Rml::Geometry background_geometry_;
  std::vector<TextGeometry> text_geometry_;
  Rml::Vector2f geometry_size_{};
  std::uint64_t revision_{0};
  std::uint64_t geometry_revision_{~std::uint64_t{0}};
  std::size_t selection_anchor_{0};
  std::size_t selection_active_{0};
  bool has_selection_{false};
  bool selecting_{false};
  std::string search_query_;
  std::size_t search_match_count_{0};
};

void register_terminal_element();

} // namespace tokmon::desk
