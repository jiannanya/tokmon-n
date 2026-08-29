#pragma once

#include "review/git_service.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Texture.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace tokmon::desk {

class ElementDiffSurface final : public Rml::Element {
 public:
  explicit ElementDiffSurface(const Rml::String& tag);
  void set_diff(GitFileDiff diff);
  void set_split_view(bool split);
  [[nodiscard]] bool split_view() const noexcept { return split_view_; }
  void scroll_lines(int lines);
  [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
  [[nodiscard]] std::size_t rendered_line_count() const noexcept {
    return rendered_lines_;
  }
  void OnRender() override;

 private:
  struct DisplayLine {
    char origin{' '};
    int old_line{-1};
    int new_line{-1};
    std::string content;
    bool header{false};
  };
  struct TextGeometry {
    Rml::Geometry geometry;
    Rml::Texture texture;
  };
  struct SplitLine {
    std::optional<DisplayLine> original;
    std::optional<DisplayLine> modified;
    bool header{false};
  };
  void rebuild_split_lines();
  [[nodiscard]] std::size_t visual_line_count() const noexcept;
  void rebuild_geometry(Rml::Vector2f size);

  std::vector<DisplayLine> lines_;
  std::vector<SplitLine> split_lines_;
  std::size_t first_line_{0};
  std::size_t rendered_lines_{0};
  Rml::Geometry decoration_geometry_;
  std::vector<TextGeometry> text_geometry_;
  Rml::Vector2f geometry_size_{};
  std::uint64_t revision_{0};
  std::uint64_t geometry_revision_{~std::uint64_t{0}};
  bool split_view_{false};
};

void register_diff_surface_element();

} // namespace tokmon::desk
