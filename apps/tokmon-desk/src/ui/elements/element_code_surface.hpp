#pragma once

#include "editor/syntax_service.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Texture.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tokmon::desk {

struct CodeEditIntent {
  std::size_t offset{0};
  std::size_t erase_count{0};
  std::string replacement;
  std::size_t caret_after{0};
};

// One RmlUi replaced element for the complete editor viewport. Only visible
// lines are shaped and rendered, so large files never become a DOM tree.
class ElementCodeSurface final : public Rml::Element {
 public:
  explicit ElementCodeSurface(const Rml::String& tag);

  void set_document(std::string text, std::vector<SyntaxSpan> spans,
                    std::uint64_t version, bool preserve_caret = false);
  [[nodiscard]] std::optional<CodeEditIntent> insert_text(std::string text) const;
  [[nodiscard]] std::optional<CodeEditIntent> erase_backward() const;
  [[nodiscard]] std::optional<CodeEditIntent> erase_forward() const;
  void move_horizontal(int direction, bool selecting);
  void move_vertical(int direction, bool selecting);
  void move_line_edge(bool end, bool selecting);
  void page(int direction, bool selecting);
  void select_all();
  void set_caret_offset(std::size_t value);
  [[nodiscard]] bool find(std::string_view query, bool backwards = false);
  [[nodiscard]] bool go_to_line(std::size_t one_based_line);
  [[nodiscard]] bool jump_to_matching_bracket();
  void set_composition(std::string text, std::size_t cursor,
                       std::size_t selection_length);
  [[nodiscard]] const std::string& composition_text() const noexcept {
    return composition_text_;
  }
  [[nodiscard]] std::string selected_text() const;
  void click(float local_x, float local_y, bool selecting);
  void scroll_lines(int lines);
  void scroll_columns(float pixels);
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  [[nodiscard]] std::size_t caret_offset() const noexcept { return caret_; }
  [[nodiscard]] std::size_t first_visible_line() const noexcept {
    return first_line_;
  }
  [[nodiscard]] float horizontal_offset() const noexcept {
    return horizontal_offset_;
  }
  [[nodiscard]] std::size_t line_count() const noexcept {
    return line_starts_.size();
  }
  [[nodiscard]] std::size_t rendered_line_count() const noexcept {
    return rendered_lines_;
  }

  void OnRender() override;

 private:
  struct TextGeometry {
    Rml::Geometry geometry;
    Rml::Texture texture;
  };

  [[nodiscard]] std::pair<std::size_t, std::size_t> selection() const;
  [[nodiscard]] std::pair<std::size_t, std::size_t> line_column(
      std::size_t offset) const;
  [[nodiscard]] std::size_t offset_at(std::size_t line,
                                      std::size_t byte_column) const;
  void set_caret(std::size_t value, bool selecting);
  void reveal_caret();
  void rebuild_geometry(Rml::Vector2f size);

  std::string text_;
  // Rebuilt once per document revision. Cursor movement, hit testing and
  // viewport rendering must not rescan a 100k-line document on every call.
  std::vector<std::size_t> line_starts_{0};
  std::vector<SyntaxSpan> spans_;
  std::uint64_t version_{0};
  std::size_t caret_{0};
  std::size_t anchor_{0};
  std::size_t first_line_{0};
  std::size_t preferred_column_{0};
  std::size_t rendered_lines_{0};
  float horizontal_offset_{0.f};
  std::string composition_text_;
  std::size_t composition_cursor_{0};
  std::size_t composition_selection_length_{0};
  Rml::Geometry decoration_geometry_;
  std::vector<TextGeometry> text_geometry_;
  Rml::Vector2f geometry_size_{};
  std::uint64_t revision_{0};
  std::uint64_t theme_revision_{0};
  std::uint64_t geometry_revision_{~std::uint64_t{0}};
};

void register_code_surface_element();

} // namespace tokmon::desk
