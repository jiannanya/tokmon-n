#include "ui/elements/element_terminal.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/TextShapingContext.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace tokmon::desk {
namespace {

Rml::ColourbPremultiplied colour(const TerminalColor& value) {
  return Rml::Colourb(value.red, value.green, value.blue, 255).ToPremultiplied();
}

bool same_style(const TerminalCell& left, const TerminalCell& right) {
  return left.foreground.red == right.foreground.red &&
         left.foreground.green == right.foreground.green &&
         left.foreground.blue == right.foreground.blue &&
         left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline &&
         left.strikethrough == right.strikethrough &&
         left.hyperlink == right.hyperlink;
}

} // namespace

ElementTerminal::ElementTerminal(const Rml::String& tag) : Rml::Element(tag) {
#if defined(_WIN32)
  SetProperty("font-family", "Consolas");
#elif defined(__APPLE__)
  SetProperty("font-family", "Menlo");
#else
  SetProperty("font-family", "DejaVu Sans Mono");
#endif
}

void ElementTerminal::set_snapshot(TerminalRenderSnapshot snapshot) {
  snapshot_ = std::move(snapshot);
  if (!search_query_.empty())
    set_search(search_query_);
  else {
    apply_selection();
    ++revision_;
  }
}

std::size_t ElementTerminal::cell_at(const float x, const float y) {
  if (snapshot_.columns == 0 || snapshot_.rows == 0)
    return 0;
  const float width = std::max(GetClientWidth(), 1.f);
  const float height = std::max(GetClientHeight(), 1.f);
  const auto column = static_cast<std::size_t>(std::clamp(
      static_cast<int>(x / (width / snapshot_.columns)), 0,
      static_cast<int>(snapshot_.columns) - 1));
  const auto row = static_cast<std::size_t>(std::clamp(
      static_cast<int>(y / (height / snapshot_.rows)), 0,
      static_cast<int>(snapshot_.rows) - 1));
  return row * snapshot_.columns + column;
}

void ElementTerminal::apply_selection() {
  for (auto& cell : snapshot_.cells)
    cell.selected = false;
  if (!has_selection_ || snapshot_.cells.empty())
    return;
  const auto first = std::min(selection_anchor_, selection_active_);
  const auto last = std::min(std::max(selection_anchor_, selection_active_),
                             snapshot_.cells.size() - 1);
  for (auto index = first; index <= last; ++index)
    snapshot_.cells[index].selected = true;
}

void ElementTerminal::begin_selection(const float x, const float y) {
  selection_anchor_ = selection_active_ = cell_at(x, y);
  has_selection_ = true;
  selecting_ = true;
  apply_selection();
  ++revision_;
}

void ElementTerminal::update_selection(const float x, const float y) {
  if (!selecting_)
    return;
  selection_active_ = cell_at(x, y);
  apply_selection();
  ++revision_;
}

void ElementTerminal::end_selection() { selecting_ = false; }

void ElementTerminal::clear_selection() {
  has_selection_ = false;
  selecting_ = false;
  apply_selection();
  ++revision_;
}

std::string ElementTerminal::selected_text() const {
  if (!has_selection_ || snapshot_.cells.empty() || snapshot_.columns == 0)
    return {};
  const auto first = std::min(selection_anchor_, selection_active_);
  const auto last = std::min(std::max(selection_anchor_, selection_active_),
                             snapshot_.cells.size() - 1);
  return terminal_selection_text(snapshot_, first, last);
}

std::string ElementTerminal::hyperlink_at(const float x, const float y) {
  if (snapshot_.cells.empty())
    return {};
  const auto index = std::min(cell_at(x, y), snapshot_.cells.size() - 1);
  return snapshot_.cells[index].hyperlink;
}

void ElementTerminal::set_search(std::string query) {
  search_query_ = std::move(query);
  search_match_count_ = 0;
  apply_selection();
  if (search_query_.empty() || snapshot_.columns == 0) {
    ++revision_;
    return;
  }
  for (std::size_t row = 0; row < snapshot_.rows; ++row) {
    std::string text;
    std::vector<std::size_t> cell_offsets;
    const auto first = row * snapshot_.columns;
    const auto last = std::min(first + snapshot_.columns, snapshot_.cells.size());
    for (auto index = first; index < last; ++index) {
      cell_offsets.push_back(text.size());
      text += snapshot_.cells[index].grapheme.empty()
                  ? " " : snapshot_.cells[index].grapheme;
    }
    for (std::size_t position = 0;
         (position = text.find(search_query_, position)) != std::string::npos;
         position += std::max<std::size_t>(1, search_query_.size())) {
      ++search_match_count_;
      const auto begin = std::upper_bound(cell_offsets.begin(),
                                          cell_offsets.end(), position);
      const auto end = std::lower_bound(
          cell_offsets.begin(), cell_offsets.end(), position + search_query_.size());
      const auto cell_begin = begin == cell_offsets.begin()
          ? 0 : static_cast<std::size_t>(begin - cell_offsets.begin() - 1);
      const auto cell_end = std::max(cell_begin + 1,
          static_cast<std::size_t>(end - cell_offsets.begin()));
      for (auto cell = cell_begin; cell < cell_end && first + cell < last; ++cell)
        snapshot_.cells[first + cell].selected = true;
    }
  }
  ++revision_;
}

void ElementTerminal::rebuild_geometry(const Rml::Vector2f size) {
  background_geometry_ = {};
  text_geometry_.clear();
  geometry_size_ = size;
  geometry_revision_ = revision_;
  auto* render_manager = GetRenderManager();
  auto* font_engine = Rml::GetFontEngineInterface();
  const auto face = GetFontFaceHandle();
  if (!render_manager || !font_engine || !face || snapshot_.columns == 0 ||
      snapshot_.rows == 0 || snapshot_.cells.empty())
    return;

  const float cell_width = size.x / snapshot_.columns;
  const float cell_height = size.y / snapshot_.rows;
  Rml::Mesh backgrounds;
  const auto selected_background =
      Rml::Colourb(87, 83, 78, 255).ToPremultiplied();
  for (std::uint16_t row = 0; row < snapshot_.rows; ++row) {
    for (std::uint16_t column = 0; column < snapshot_.columns; ++column) {
      const auto index = static_cast<std::size_t>(row) * snapshot_.columns + column;
      if (index >= snapshot_.cells.size())
        break;
      const auto& cell = snapshot_.cells[index];
      const auto background = cell.selected ? selected_background : colour(cell.background);
      Rml::MeshUtilities::GenerateQuad(
          backgrounds, {column * cell_width, row * cell_height},
          {std::ceil(cell_width + 0.01f), std::ceil(cell_height + 0.01f)},
          background);
    }
  }

  if (snapshot_.cursor.visible) {
    const float cursor_x = snapshot_.cursor.column * cell_width;
    const float cursor_y = snapshot_.cursor.row * cell_height;
    Rml::Vector2f cursor_position{cursor_x, cursor_y};
    Rml::Vector2f cursor_size{cell_width, cell_height};
    if (snapshot_.cursor.style == TerminalCursor::Style::bar)
      cursor_size.x = std::max(1.0f, std::round(cell_width * 0.16f));
    else if (snapshot_.cursor.style == TerminalCursor::Style::underline) {
      cursor_position.y += std::max(0.0f, cell_height - 2.0f);
      cursor_size.y = 2.0f;
    }
    Rml::MeshUtilities::GenerateQuad(backgrounds, cursor_position, cursor_size,
                                     colour(snapshot_.cursor_color));
  }
  static const Rml::String language;
  Rml::TextShapingContext shaping{language};
  shaping.text_direction = Rml::Style::Direction::Ltr;
  shaping.font_kerning = Rml::Style::FontKerning::None;
  const float baseline_offset = std::max(1.0f, cell_height * 0.78f);
  for (std::uint16_t row = 0; row < snapshot_.rows; ++row) {
    std::uint16_t column = 0;
    while (column < snapshot_.columns) {
      const auto start_index = static_cast<std::size_t>(row) * snapshot_.columns + column;
      if (start_index >= snapshot_.cells.size())
        break;
      const auto& first = snapshot_.cells[start_index];
      std::string run;
      const auto run_start = column;
      do {
        const auto& cell = snapshot_.cells[static_cast<std::size_t>(row) *
                                           snapshot_.columns + column];
        run += cell.grapheme.empty() ? " " : cell.grapheme;
        ++column;
      } while (column < snapshot_.columns &&
               same_style(first, snapshot_.cells[static_cast<std::size_t>(row) *
                                                   snapshot_.columns + column]));
      Rml::TexturedMeshList meshes;
      (void)font_engine->GenerateString(
          *render_manager, face, {}, run,
          {run_start * cell_width, row * cell_height + baseline_offset},
          colour(first.foreground), 1.0f, shaping, meshes);
      for (auto& mesh : meshes)
        text_geometry_.push_back(
            {render_manager->MakeGeometry(std::move(mesh.mesh)),
             std::move(mesh.texture)});
      if (!first.hyperlink.empty()) {
        Rml::MeshUtilities::GenerateQuad(
            backgrounds,
            {run_start * cell_width,
             row * cell_height + std::max(1.f, cell_height - 2.f)},
            {std::max(cell_width,
                      static_cast<float>(column - run_start) * cell_width), 1.f},
            Rml::Colourb(96, 165, 250).ToPremultiplied());
      }
    }
  }
  background_geometry_ = render_manager->MakeGeometry(std::move(backgrounds));
}

void ElementTerminal::OnRender() {
  const Rml::Vector2f size{GetClientWidth(), GetClientHeight()};
  if (revision_ != geometry_revision_ || size != geometry_size_)
    rebuild_geometry(size);
  const auto translation = GetAbsoluteOffset(Rml::BoxArea::Content);
  if (background_geometry_)
    background_geometry_.Render(translation);
  for (const auto& item : text_geometry_)
    item.geometry.Render(translation, item.texture);
}

void register_terminal_element() {
  Rml::Factory::RegisterElementInstancer(
      "tokmon-terminal", new Rml::ElementInstancerGeneric<ElementTerminal>());
}

} // namespace tokmon::desk
