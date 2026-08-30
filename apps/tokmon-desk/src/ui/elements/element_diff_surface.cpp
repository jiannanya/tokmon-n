#include "ui/elements/element_diff_surface.hpp"

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
#include <iomanip>
#include <sstream>

namespace tokmon::desk {

namespace {

float density(const Rml::Element* element) {
  return Rml::ElementUtilities::GetDensityIndependentPixelRatio(
      const_cast<Rml::Element*>(element));
}

Rml::Colourb raw_colour(const legacy_theme::Color value) {
  return {value.red, value.green, value.blue, value.alpha};
}

Rml::ColourbPremultiplied colour(const legacy_theme::Color value) {
  return raw_colour(value).ToPremultiplied();
}

} // namespace

ElementDiffSurface::ElementDiffSurface(const Rml::String& tag)
    : Rml::Element(tag) {
#if defined(_WIN32)
  SetProperty("font-family", "Consolas");
#elif defined(__APPLE__)
  SetProperty("font-family", "Menlo");
#else
  SetProperty("font-family", "DejaVu Sans Mono");
#endif
}

void ElementDiffSurface::set_diff(GitFileDiff diff) {
  lines_.clear();
  for (const auto& hunk : diff.hunks) {
    lines_.push_back({'@', -1, -1, hunk.header, true});
    for (const auto& line : hunk.lines)
      lines_.push_back({line.origin, line.old_line, line.new_line,
                        line.content, false});
  }
  if (diff.binary)
    lines_.push_back({'!', -1, -1, "二进制文件不支持行级审查", true});
  rebuild_split_lines();
  const auto count = visual_line_count();
  first_line_ = std::min(first_line_, count == 0 ? 0u : count - 1u);
  ++revision_;
}

void ElementDiffSurface::set_split_view(const bool split) {
  if (split_view_ == split)
    return;
  split_view_ = split;
  const auto count = visual_line_count();
  first_line_ = std::min(first_line_, count == 0 ? 0u : count - 1u);
  ++revision_;
}

void ElementDiffSurface::rebuild_split_lines() {
  split_lines_.clear();
  for (std::size_t index = 0; index < lines_.size();) {
    const auto& line = lines_[index];
    if (line.header) {
      split_lines_.push_back({.original = line, .header = true});
      ++index;
      continue;
    }
    if (line.origin == '-') {
      const auto removed_begin = index;
      while (index < lines_.size() && !lines_[index].header &&
             lines_[index].origin == '-')
        ++index;
      const auto removed_end = index;
      const auto added_begin = index;
      while (index < lines_.size() && !lines_[index].header &&
             lines_[index].origin == '+')
        ++index;
      const auto added_end = index;
      const auto rows = std::max(removed_end - removed_begin,
                                 added_end - added_begin);
      for (std::size_t row = 0; row < rows; ++row) {
        SplitLine paired;
        if (removed_begin + row < removed_end)
          paired.original = lines_[removed_begin + row];
        if (added_begin + row < added_end)
          paired.modified = lines_[added_begin + row];
        split_lines_.push_back(std::move(paired));
      }
      continue;
    }
    if (line.origin == '+') {
      split_lines_.push_back({.modified = line});
      ++index;
      continue;
    }
    split_lines_.push_back({.original = line, .modified = line});
    ++index;
  }
}

std::size_t ElementDiffSurface::visual_line_count() const noexcept {
  return split_view_ ? split_lines_.size() : lines_.size();
}

void ElementDiffSurface::scroll_lines(const int lines) {
  if (lines < 0)
    first_line_ -= std::min(first_line_, static_cast<std::size_t>(-lines));
  else
    first_line_ = std::min(first_line_ + static_cast<std::size_t>(lines),
                           visual_line_count() == 0 ? 0u
                                                    : visual_line_count() - 1u);
  ++revision_;
}

void ElementDiffSurface::rebuild_geometry(const Rml::Vector2f size) {
  decoration_geometry_ = {};
  text_geometry_.clear();
  geometry_size_ = size;
  geometry_revision_ = revision_;
  rendered_lines_ = 0;
  auto* render_manager = GetRenderManager();
  auto* font_engine = Rml::GetFontEngineInterface();
  const auto face = GetFontFaceHandle();
  if (!render_manager || !font_engine || !face)
    return;
  const float scale = density(this);
  const float row_height = 17.f * scale;
  const float unified_gutter = 62.f * scale;
  const float split_gutter = 44.f * scale;
  const auto visible = static_cast<std::size_t>(
      std::ceil(std::max(size.y, row_height) / row_height)) + 1u;
  Rml::Mesh decorations;
  static const Rml::String language;
  Rml::TextShapingContext shaping{language};
  shaping.text_direction = Rml::Style::Direction::Ltr;
  shaping.font_kerning = Rml::Style::FontKerning::None;
  const auto add_text = [&](const std::string& value, const Rml::Vector2f position,
                            const Rml::Colourb colour) {
    Rml::TexturedMeshList meshes;
    (void)font_engine->GenerateString(*render_manager, face, {}, value, position,
                                      colour.ToPremultiplied(), 1.f, shaping,
                                      meshes);
    for (auto& mesh : meshes)
      text_geometry_.push_back(
          {render_manager->MakeGeometry(std::move(mesh.mesh)),
           std::move(mesh.texture)});
  };

  if (split_view_) {
    const float half = std::max(scale, std::floor(size.x * 0.5f));
    Rml::MeshUtilities::GenerateQuad(
        decorations, {0, 0}, {size.x, size.y},
        colour(legacy_theme::white));
    Rml::MeshUtilities::GenerateQuad(
        decorations, {half - 0.5f * scale, 0}, {scale, size.y},
        colour(legacy_theme::border));
    for (std::size_t offset = 0; offset < visible; ++offset) {
      const auto index = first_line_ + offset;
      if (index >= split_lines_.size())
        break;
      const auto& row = split_lines_[index];
      const float y = static_cast<float>(offset) * row_height;
      if (row.header) {
        Rml::MeshUtilities::GenerateQuad(
            decorations, {0, y}, {size.x, row_height},
            colour(legacy_theme::diff_banner));
        add_text(row.original ? row.original->content : std::string{},
                 {6.f * scale, y + 13.f * scale},
                 raw_colour(legacy_theme::dim));
        ++rendered_lines_;
        continue;
      }
      const std::optional<DisplayLine>* cells[] = {&row.original, &row.modified};
      for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? 0.f : half;
        const float width = side == 0 ? half : size.x - half;
        auto background = colour(legacy_theme::white);
        auto gutter = colour(legacy_theme::surface_warm);
        if (*cells[side] && (*cells[side])->origin == '-')
          background = colour(legacy_theme::diff_delete_background);
        else if (*cells[side] && (*cells[side])->origin == '+')
          background = colour(legacy_theme::diff_add_background);
        else if (!*cells[side])
          background = colour(legacy_theme::diff_empty);
        if (*cells[side] && (*cells[side])->origin == '-')
          gutter = colour(legacy_theme::diff_delete_gutter);
        else if (*cells[side] && (*cells[side])->origin == '+')
          gutter = colour(legacy_theme::diff_add_gutter);
        Rml::MeshUtilities::GenerateQuad(decorations, {x, y},
                                         {width, row_height}, background);
        Rml::MeshUtilities::GenerateQuad(
            decorations, {x, y}, {split_gutter, row_height},
            gutter);
        if (!*cells[side])
          continue;
        const auto& cell = **cells[side];
        const int number = side == 0 ? cell.old_line : cell.new_line;
        const auto number_colour = cell.origin == '-'
            ? legacy_theme::red
            : cell.origin == '+' ? legacy_theme::green : legacy_theme::faint;
        const auto text_colour = cell.origin == '-'
            ? legacy_theme::red_ink
            : cell.origin == '+' ? legacy_theme::green_ink : legacy_theme::ink;
        add_text(number >= 0 ? std::to_string(number) : std::string{},
                 {x + 4.f * scale, y + 13.f * scale},
                 raw_colour(number_colour));
        add_text(std::string(1, cell.origin) + cell.content,
                 {x + split_gutter + 5.f * scale, y + 13.f * scale},
                 raw_colour(text_colour));
      }
      ++rendered_lines_;
    }
    decoration_geometry_ = render_manager->MakeGeometry(std::move(decorations));
    return;
  }

  Rml::MeshUtilities::GenerateQuad(
      decorations, {0, 0}, {size.x, size.y},
      colour(legacy_theme::white));
  Rml::MeshUtilities::GenerateQuad(
      decorations, {0, 0}, {unified_gutter, size.y},
      colour(legacy_theme::surface_warm));
  for (std::size_t offset = 0; offset < visible; ++offset) {
    const auto index = first_line_ + offset;
    if (index >= lines_.size())
      break;
    const auto& line = lines_[index];
    const float y = static_cast<float>(offset) * row_height;
    auto background = colour(legacy_theme::white);
    auto gutter_background = colour(legacy_theme::surface_warm);
    auto foreground = raw_colour(legacy_theme::ink);
    auto number_colour = raw_colour(legacy_theme::faint);
    if (line.origin == '+') {
      background = colour(legacy_theme::diff_add_background);
      gutter_background = colour(legacy_theme::diff_add_gutter);
      foreground = raw_colour(legacy_theme::green_ink);
      number_colour = raw_colour(legacy_theme::green);
    }
    else if (line.origin == '-') {
      background = colour(legacy_theme::diff_delete_background);
      gutter_background = colour(legacy_theme::diff_delete_gutter);
      foreground = raw_colour(legacy_theme::red_ink);
      number_colour = raw_colour(legacy_theme::red);
    }
    else if (line.header) {
      background = colour(legacy_theme::diff_banner);
      gutter_background = background;
      foreground = raw_colour(legacy_theme::dim);
    }
    Rml::MeshUtilities::GenerateQuad(decorations, {0, y},
                                     {unified_gutter, row_height},
                                     gutter_background);
    Rml::MeshUtilities::GenerateQuad(decorations, {unified_gutter, y},
                                     {std::max(0.f, size.x - unified_gutter), row_height},
                                     background);
    std::ostringstream numbers;
    if (!line.header)
      numbers << (line.old_line >= 0 ? std::to_string(line.old_line) : "")
              << " "
              << (line.new_line >= 0 ? std::to_string(line.new_line) : "");
    add_text(numbers.str(), {4.f * scale, y + 13.f * scale}, number_colour);
    const auto text = line.header ? line.content
                                  : std::string(1, line.origin) + line.content;
    add_text(text, {unified_gutter + 5.f * scale, y + 13.f * scale}, foreground);
    ++rendered_lines_;
  }
  decoration_geometry_ = render_manager->MakeGeometry(std::move(decorations));
}

void ElementDiffSurface::OnRender() {
  const Rml::Vector2f size{GetClientWidth(), GetClientHeight()};
  if (revision_ != geometry_revision_ || size != geometry_size_)
    rebuild_geometry(size);
  const auto translation = GetAbsoluteOffset(Rml::BoxArea::Content);
  if (decoration_geometry_)
    decoration_geometry_.Render(translation);
  for (const auto& item : text_geometry_)
    item.geometry.Render(translation, item.texture);
}

void register_diff_surface_element() {
  Rml::Factory::RegisterElementInstancer(
      "tokmon-diff-surface", new Rml::ElementInstancerGeneric<ElementDiffSurface>());
}

} // namespace tokmon::desk
