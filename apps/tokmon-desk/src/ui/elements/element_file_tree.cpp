#include "ui/elements/element_file_tree.hpp"

#include "ui/theme_palette.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/TextShapingContext.h>

#include <algorithm>
#include <cmath>

namespace tokmon::desk {

namespace {

Rml::ColourbPremultiplied colour(const legacy_theme::Color value) {
  const auto resolved = legacy_theme::themed(value);
  return Rml::Colourb(resolved.red, resolved.green, resolved.blue,
                      resolved.alpha)
      .ToPremultiplied();
}

float density(const Rml::Element* element) {
  return Rml::ElementUtilities::GetDensityIndependentPixelRatio(
      const_cast<Rml::Element*>(element));
}

} // namespace

ElementFileTree::ElementFileTree(const Rml::String& tag) : Rml::Element(tag) {}

void ElementFileTree::set_rows(std::vector<WorkspaceEntry> rows) {
  rows_ = std::move(rows);
  first_row_ = std::min(first_row_, rows_.empty() ? 0u : rows_.size() - 1u);
  ++revision_;
}

void ElementFileTree::set_selected(std::string relative_path) {
  selected_ = std::move(relative_path);
  ++revision_;
}

std::optional<WorkspaceEntry> ElementFileTree::row_at(const float local_y) const {
  const float row_height = 27.f * density(this);
  const auto index = first_row_ + static_cast<std::size_t>(
      std::max(0.f, local_y) / row_height);
  return index < rows_.size() ? std::optional(rows_[index]) : std::nullopt;
}

std::optional<WorkspaceEntry> ElementFileTree::selected_row() const {
  const auto selected = std::ranges::find_if(rows_, [&](const auto& row) {
    return row.relative_path == selected_;
  });
  return selected == rows_.end() ? std::nullopt
                                 : std::optional<WorkspaceEntry>(*selected);
}

std::optional<WorkspaceEntry> ElementFileTree::move_selection(const int rows) {
  if (rows_.empty())
    return std::nullopt;
  const auto selected = std::ranges::find_if(rows_, [&](const auto& row) {
    return row.relative_path == selected_;
  });
  const auto current = selected == rows_.end()
      ? std::size_t{0}
      : static_cast<std::size_t>(std::distance(rows_.begin(), selected));
  const auto target = rows < 0
      ? current - std::min(current, static_cast<std::size_t>(-rows))
      : std::min(current + static_cast<std::size_t>(rows), rows_.size() - 1);
  selected_ = rows_[target].relative_path;
  const float row_height = 27.f * density(this);
  const auto visible = std::max<std::size_t>(1u, static_cast<std::size_t>(
      std::floor(std::max(GetClientHeight(), row_height) / row_height)));
  if (target < first_row_)
    first_row_ = target;
  else if (target >= first_row_ + visible)
    first_row_ = target - visible + 1;
  ++revision_;
  return rows_[target];
}

std::optional<WorkspaceEntry> ElementFileTree::select_edge(const bool last) {
  if (rows_.empty())
    return std::nullopt;
  const auto target = last ? rows_.size() - 1 : 0u;
  selected_ = rows_[target].relative_path;
  const float row_height = 27.f * density(this);
  const auto visible = std::max<std::size_t>(1u, static_cast<std::size_t>(
      std::floor(std::max(GetClientHeight(), row_height) / row_height)));
  first_row_ = last && target >= visible ? target - visible + 1 : 0u;
  ++revision_;
  return rows_[target];
}

void ElementFileTree::scroll_lines(const int lines) {
  if (lines < 0)
    first_row_ -= std::min(first_row_, static_cast<std::size_t>(-lines));
  else
    first_row_ = std::min(first_row_ + static_cast<std::size_t>(lines),
                          rows_.empty() ? 0u : rows_.size() - 1u);
  ++revision_;
}

void ElementFileTree::rebuild_geometry(const Rml::Vector2f size) {
  decoration_geometry_ = {};
  text_geometry_.clear();
  geometry_size_ = size;
  geometry_revision_ = revision_;
  rendered_rows_ = 0;
  auto* render_manager = GetRenderManager();
  auto* font_engine = Rml::GetFontEngineInterface();
  const auto face = GetFontFaceHandle();
  if (!render_manager || !font_engine || !face)
    return;

  const float scale = density(this);
  const float row_height = 27.f * scale;
  const auto visible = static_cast<std::size_t>(
      std::ceil(std::max(size.y, row_height) / row_height)) + 1u;
  Rml::Mesh decorations;
  static const Rml::String language;
  Rml::TextShapingContext shaping{language};
  shaping.text_direction = Rml::Style::Direction::Ltr;
  shaping.font_kerning = Rml::Style::FontKerning::Auto;

  for (std::size_t offset = 0; offset < visible; ++offset) {
    const auto index = first_row_ + offset;
    if (index >= rows_.size())
      break;
    const auto& row = rows_[index];
    const float y = static_cast<float>(offset) * row_height;
    if (row.relative_path == selected_)
      Rml::MeshUtilities::GenerateQuad(
          decorations, {2.f * scale, y + scale},
          {std::max(0.f, size.x - 4.f * scale), row_height - 2.f * scale},
          colour(legacy_theme::accent_background));
    const float x = (10.f + static_cast<float>(row.depth) * 14.f) * scale;
    const std::string label = std::string(row.directory
        ? (row.expanded ? "v  " : ">  ") : "   ") + row.name;
    Rml::TexturedMeshList meshes;
    (void)font_engine->GenerateString(
        *render_manager, face, {}, label, {x, y + 18.f * scale},
        colour(row.directory ? legacy_theme::strong : legacy_theme::mid),
        1.f, shaping, meshes);
    for (auto& mesh : meshes)
      text_geometry_.push_back({render_manager->MakeGeometry(std::move(mesh.mesh)),
                                std::move(mesh.texture)});
    ++rendered_rows_;
  }
  decoration_geometry_ = render_manager->MakeGeometry(std::move(decorations));
}

void ElementFileTree::OnRender() {
  const Rml::Vector2f size{GetClientWidth(), GetClientHeight()};
  if (revision_ != geometry_revision_ || size != geometry_size_ ||
      theme_revision_ != legacy_theme::theme_revision())
    rebuild_geometry(size);
  theme_revision_ = legacy_theme::theme_revision();
  const auto translation = GetAbsoluteOffset(Rml::BoxArea::Content);
  if (decoration_geometry_)
    decoration_geometry_.Render(translation);
  for (const auto& item : text_geometry_)
    item.geometry.Render(translation, item.texture);
}

void register_file_tree_element() {
  Rml::Factory::RegisterElementInstancer(
      "tokmon-file-tree", new Rml::ElementInstancerGeneric<ElementFileTree>());
}

} // namespace tokmon::desk
