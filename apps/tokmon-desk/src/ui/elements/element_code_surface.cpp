#include "ui/elements/element_code_surface.hpp"

#include "editor/grapheme.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/TextShapingContext.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace tokmon::desk {
namespace {

Rml::ColourbPremultiplied syntax_colour(const SyntaxKind kind) {
  switch (kind) {
    case SyntaxKind::comment: return Rml::Colourb(106, 153, 85).ToPremultiplied();
    case SyntaxKind::keyword: return Rml::Colourb(197, 134, 192).ToPremultiplied();
    case SyntaxKind::type: return Rml::Colourb(78, 201, 176).ToPremultiplied();
    case SyntaxKind::string: return Rml::Colourb(206, 145, 120).ToPremultiplied();
    case SyntaxKind::number: return Rml::Colourb(181, 206, 168).ToPremultiplied();
    case SyntaxKind::preprocessor: return Rml::Colourb(86, 156, 214).ToPremultiplied();
    default: return Rml::Colourb(212, 212, 212).ToPremultiplied();
  }
}

} // namespace

ElementCodeSurface::ElementCodeSurface(const Rml::String& tag)
    : Rml::Element(tag) {
#if defined(_WIN32)
  SetProperty("font-family", "Consolas");
#elif defined(__APPLE__)
  SetProperty("font-family", "Menlo");
#else
  SetProperty("font-family", "DejaVu Sans Mono");
#endif
}

void ElementCodeSurface::set_document(std::string text,
                                      std::vector<SyntaxSpan> spans,
                                      const std::uint64_t version,
                                      const bool preserve_caret) {
  text_ = std::move(text);
  line_starts_.assign(1, 0);
  line_starts_.reserve(1 + static_cast<std::size_t>(
      std::count(text_.begin(), text_.end(), '\n')));
  for (std::size_t index = 0; index < text_.size(); ++index)
    if (text_[index] == '\n')
      line_starts_.push_back(index + 1);
  spans_ = std::move(spans);
  version_ = version;
  composition_text_.clear();
  composition_cursor_ = 0;
  composition_selection_length_ = 0;
  if (!preserve_caret) {
    caret_ = 0;
    anchor_ = 0;
    first_line_ = 0;
  } else {
    caret_ = std::min(caret_, text_.size());
    anchor_ = std::min(anchor_, text_.size());
  }
  ++revision_;
  reveal_caret();
}

std::pair<std::size_t, std::size_t> ElementCodeSurface::selection() const {
  return std::minmax(caret_, anchor_);
}

std::optional<CodeEditIntent> ElementCodeSurface::insert_text(
    std::string value) const {
  if (value.empty())
    return std::nullopt;
  const auto [begin, end] = selection();
  const auto size = value.size();
  return CodeEditIntent{begin, end - begin, std::move(value), begin + size};
}

std::optional<CodeEditIntent> ElementCodeSurface::erase_backward() const {
  const auto [begin, end] = selection();
  if (begin != end)
    return CodeEditIntent{begin, end - begin, {}, begin};
  if (caret_ == 0)
    return std::nullopt;
  const auto previous = previous_grapheme_boundary(text_, caret_);
  return CodeEditIntent{previous, caret_ - previous, {}, previous};
}

std::optional<CodeEditIntent> ElementCodeSurface::erase_forward() const {
  const auto [begin, end] = selection();
  if (begin != end)
    return CodeEditIntent{begin, end - begin, {}, begin};
  if (caret_ >= text_.size())
    return std::nullopt;
  const auto next = next_grapheme_boundary(text_, caret_);
  return CodeEditIntent{caret_, next - caret_, {}, caret_};
}

std::pair<std::size_t, std::size_t> ElementCodeSurface::line_column(
    const std::size_t offset) const {
  const auto found = std::upper_bound(line_starts_.begin(), line_starts_.end(),
                                      std::min(offset, text_.size()));
  const auto line = found == line_starts_.begin()
      ? 0 : static_cast<std::size_t>(
          std::distance(line_starts_.begin(), found) - 1);
  return {line, std::min(offset, text_.size()) - line_starts_[line]};
}

std::size_t ElementCodeSurface::offset_at(const std::size_t line,
                                          const std::size_t byte_column) const {
  if (line_starts_.empty())
    return 0;
  const auto selected_line = std::min(line, line_starts_.size() - 1);
  const auto end = selected_line + 1 < line_starts_.size()
      ? line_starts_[selected_line + 1] - 1 : text_.size();
  return std::min(line_starts_[selected_line] + byte_column, end);
}

void ElementCodeSurface::set_caret(const std::size_t value,
                                   const bool selecting) {
  caret_ = clamp_grapheme_boundary(text_, std::min(value, text_.size()));
  if (!selecting)
    anchor_ = caret_;
  preferred_column_ = line_column(caret_).second;
  ++revision_;
  reveal_caret();
}

void ElementCodeSurface::move_horizontal(const int direction,
                                         const bool selecting) {
  if (!selecting && caret_ != anchor_) {
    const auto [begin, end] = selection();
    set_caret(direction < 0 ? begin : end, false);
    return;
  }
  set_caret(direction < 0 ? previous_grapheme_boundary(text_, caret_)
                          : next_grapheme_boundary(text_, caret_), selecting);
}

void ElementCodeSurface::move_vertical(const int direction,
                                       const bool selecting) {
  const auto [line, column] = line_column(caret_);
  const auto target = direction < 0
      ? (line == 0 ? 0 : line - 1)
      : std::min(line + 1, line_starts_.size() - 1);
  const auto saved = preferred_column_ ? preferred_column_ : column;
  set_caret(offset_at(target, saved), selecting);
  preferred_column_ = saved;
}

void ElementCodeSurface::move_line_edge(const bool end,
                                        const bool selecting) {
  const auto [line, column] = line_column(caret_);
  (void)column;
  const auto offset = end
      ? (line + 1 < line_starts_.size()
             ? line_starts_[line + 1] - 1 : text_.size())
      : line_starts_[line];
  set_caret(offset, selecting);
}

void ElementCodeSurface::page(const int direction, const bool selecting) {
  const auto rows = std::max(1, static_cast<int>(GetClientHeight() / 17.f) - 1);
  for (int count = 0; count < rows; ++count)
    move_vertical(direction, selecting);
}

void ElementCodeSurface::select_all() {
  anchor_ = 0;
  caret_ = text_.size();
  ++revision_;
  reveal_caret();
}

void ElementCodeSurface::set_caret_offset(const std::size_t value) {
  set_caret(value, false);
}

bool ElementCodeSurface::find(const std::string_view query,
                              const bool backwards) {
  if (query.empty() || text_.empty())
    return false;
  const auto [selected_begin, selected_end] = selection();
  std::size_t found = std::string::npos;
  if (backwards) {
    const auto before = selected_begin == 0 ? text_.size() : selected_begin - 1;
    found = text_.rfind(query, before);
    if (found == std::string::npos && before != text_.size())
      found = text_.rfind(query);
  } else {
    found = text_.find(query, selected_end);
    if (found == std::string::npos && selected_end != 0)
      found = text_.find(query);
  }
  if (found == std::string::npos)
    return false;
  anchor_ = found;
  caret_ = found + query.size();
  preferred_column_ = line_column(caret_).second;
  ++revision_;
  reveal_caret();
  return true;
}

bool ElementCodeSurface::go_to_line(const std::size_t one_based_line) {
  if (one_based_line == 0 || one_based_line > line_starts_.size())
    return false;
  set_caret(line_starts_[one_based_line - 1], false);
  return true;
}

bool ElementCodeSurface::jump_to_matching_bracket() {
  if (text_.empty())
    return false;
  auto origin = std::min(caret_, text_.size() - 1);
  const std::string_view opening = "([{<";
  const std::string_view closing = ")]}>";
  if (opening.find(text_[origin]) == std::string_view::npos &&
      closing.find(text_[origin]) == std::string_view::npos) {
    if (origin == 0)
      return false;
    --origin;
  }
  const auto open_index = opening.find(text_[origin]);
  const auto close_index = closing.find(text_[origin]);
  if (open_index == std::string_view::npos && close_index == std::string_view::npos)
    return false;
  const bool forward = open_index != std::string_view::npos;
  const auto pair_index = forward ? open_index : close_index;
  const char open = opening[pair_index];
  const char close = closing[pair_index];
  int depth = 0;
  if (forward) {
    for (auto index = origin; index < text_.size(); ++index) {
      if (text_[index] == open) ++depth;
      else if (text_[index] == close && --depth == 0) {
        set_caret(index, false);
        return true;
      }
    }
  } else {
    for (auto index = origin + 1; index > 0; ) {
      --index;
      if (text_[index] == close) ++depth;
      else if (text_[index] == open && --depth == 0) {
        set_caret(index, false);
        return true;
      }
    }
  }
  return false;
}

void ElementCodeSurface::set_composition(std::string text,
                                         const std::size_t cursor,
                                         const std::size_t selection_length) {
  composition_text_ = std::move(text);
  composition_cursor_ = std::min(cursor, composition_text_.size());
  composition_selection_length_ = std::min(
      selection_length, composition_text_.size() - composition_cursor_);
  ++revision_;
}

std::string ElementCodeSurface::selected_text() const {
  const auto [begin, end] = selection();
  return text_.substr(begin, end - begin);
}

void ElementCodeSurface::click(const float local_x, const float local_y,
                               const bool selecting) {
  const auto line = std::min(first_line_ + static_cast<std::size_t>(
      std::max(0.f, local_y) / 17.f), line_starts_.size() - 1);
  const auto line_start = line_starts_[line];
  const auto line_end = line + 1 < line_starts_.size()
      ? line_starts_[line + 1] - 1 : text_.size();
  auto* font_engine = Rml::GetFontEngineInterface();
  const auto face = GetFontFaceHandle();
  static const Rml::String language;
  Rml::TextShapingContext shaping{language};
  shaping.text_direction = Rml::Style::Direction::Ltr;
  shaping.font_kerning = Rml::Style::FontKerning::None;
  const float target = std::max(0.f, local_x - 48.f) + horizontal_offset_;
  auto offset = line_start;
  while (offset < line_end && font_engine && face) {
    const auto next = std::min(next_grapheme_boundary(text_, offset), line_end);
    const auto width = static_cast<float>(font_engine->GetStringWidth(
        face, Rml::StringView(text_.data() + line_start, next - line_start),
        shaping));
    if (width >= target)
      break;
    offset = next;
  }
  set_caret(offset, selecting);
}

void ElementCodeSurface::scroll_columns(const float pixels) {
  horizontal_offset_ = std::max(0.f, horizontal_offset_ + pixels);
  ++revision_;
}

void ElementCodeSurface::scroll_lines(const int lines) {
  const auto count = line_starts_.size();
  if (lines < 0)
    first_line_ -= std::min(first_line_, static_cast<std::size_t>(-lines));
  else
    first_line_ = std::min(first_line_ + static_cast<std::size_t>(lines),
                           count > 0 ? count - 1 : 0);
  ++revision_;
}

void ElementCodeSurface::reveal_caret() {
  const auto line = line_column(caret_).first;
  const auto rows = std::max<std::size_t>(1,
      static_cast<std::size_t>(std::max(17.f, GetClientHeight()) / 17.f));
  if (line < first_line_)
    first_line_ = line;
  else if (line >= first_line_ + rows)
    first_line_ = line - rows + 1;
}

void ElementCodeSurface::rebuild_geometry(const Rml::Vector2f size) {
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

  constexpr float line_height = 17.f;
  constexpr float gutter = 44.f;
  Rml::Mesh decorations;
  Rml::MeshUtilities::GenerateQuad(decorations, {0, 0}, {gutter, size.y},
      Rml::Colourb(37, 34, 32).ToPremultiplied());
  Rml::MeshUtilities::GenerateQuad(decorations, {gutter - 1.f, 0}, {1.f, size.y},
      Rml::Colourb(68, 64, 60).ToPremultiplied());

  const auto visible_rows = static_cast<std::size_t>(std::ceil(size.y / line_height));
  const auto [selection_begin, selection_end] = selection();
  static const Rml::String language;
  Rml::TextShapingContext shaping{language};
  shaping.text_direction = Rml::Style::Direction::Ltr;
  shaping.font_kerning = Rml::Style::FontKerning::None;

  auto add_text = [&](const std::string_view value, const float x, const float y,
                      const Rml::ColourbPremultiplied color) -> float {
    if (value.empty())
      return x;
    Rml::TexturedMeshList meshes;
    const auto width = font_engine->GenerateString(
        *render_manager, face, {}, Rml::String(value), {x, y + 13.f}, color, 1.f,
        shaping, meshes);
    for (auto& mesh : meshes)
      text_geometry_.push_back(
          {render_manager->MakeGeometry(std::move(mesh.mesh)),
           std::move(mesh.texture)});
    return x + static_cast<float>(width);
  };

  for (std::size_t visible = 0; visible < visible_rows; ++visible) {
    const auto line = first_line_ + visible;
    if (line >= line_starts_.size())
      break;
    const auto line_start = line_starts_[line];
    const auto line_end = line + 1 < line_starts_.size()
        ? line_starts_[line + 1] - 1 : text_.size();
    const float y = static_cast<float>(visible) * line_height;
    std::optional<float> composition_x;
    const auto selected_start = std::max(selection_begin, line_start);
    const auto selected_end = std::min(selection_end, line_end);
    const auto width_to = [&](const std::size_t offset) {
      return static_cast<float>(font_engine->GetStringWidth(
          face, Rml::StringView(text_.data() + line_start,
                                offset - line_start), shaping));
    };
    if (selected_start < selected_end) {
      const float selection_x = width_to(selected_start);
      const float selection_width = width_to(selected_end) - selection_x;
      Rml::MeshUtilities::GenerateQuad(
          decorations,
          {gutter + 4.f + selection_x - horizontal_offset_, y},
          {std::max(1.f, selection_width), line_height},
          Rml::Colourb(38, 79, 120, 210).ToPremultiplied());
    }
    if (caret_ >= line_start && caret_ <= line_end) {
      const float caret_x = width_to(caret_);
      Rml::MeshUtilities::GenerateQuad(
          decorations,
          {gutter + 4.f + caret_x - horizontal_offset_, y + 1.f},
          {1.f, line_height - 2.f},
          Rml::Colourb(248, 250, 252).ToPremultiplied());
      if (!composition_text_.empty()) {
        const auto composition_width = static_cast<float>(
            font_engine->GetStringWidth(face, Rml::String(composition_text_),
                                        shaping));
        const float draw_x = gutter + 4.f + caret_x - horizontal_offset_;
        Rml::MeshUtilities::GenerateQuad(
            decorations, {draw_x, y + line_height - 2.f},
            {std::max(1.f, composition_width), 1.f},
            Rml::Colourb(200, 106, 40).ToPremultiplied());
        composition_x = draw_x;
      }
    }
    add_text(std::to_string(line + 1), 7.f, y,
             Rml::Colourb(120, 113, 108).ToPremultiplied());

    float x = gutter + 4.f - horizontal_offset_;
    std::size_t cursor = line_start;
    for (const auto& span : spans_) {
      if (span.byte_end <= line_start || span.byte_start >= line_end)
        continue;
      const auto begin = std::max(span.byte_start, line_start);
      const auto end = std::min(span.byte_end, line_end);
      if (begin > cursor)
        x = add_text(std::string_view(text_).substr(cursor, begin - cursor),
                     x, y, syntax_colour(SyntaxKind::plain));
      if (end > begin)
        x = add_text(std::string_view(text_).substr(begin, end - begin),
                     x, y, syntax_colour(span.kind));
      cursor = std::max(cursor, end);
    }
    if (cursor < line_end)
      (void)add_text(std::string_view(text_).substr(cursor, line_end - cursor),
                     x, y, syntax_colour(SyntaxKind::plain));
    if (composition_x)
      (void)add_text(composition_text_, *composition_x, y,
                     Rml::Colourb(248, 250, 252).ToPremultiplied());
    ++rendered_lines_;
  }
  decoration_geometry_ = render_manager->MakeGeometry(std::move(decorations));
}

void ElementCodeSurface::OnRender() {
  const Rml::Vector2f size{GetClientWidth(), GetClientHeight()};
  if (revision_ != geometry_revision_ || size != geometry_size_)
    rebuild_geometry(size);
  const auto translation = GetAbsoluteOffset(Rml::BoxArea::Content);
  if (decoration_geometry_)
    decoration_geometry_.Render(translation);
  for (const auto& item : text_geometry_)
    item.geometry.Render(translation, item.texture);
}

void register_code_surface_element() {
  Rml::Factory::RegisterElementInstancer(
      "tokmon-code-surface",
      new Rml::ElementInstancerGeneric<ElementCodeSurface>());
}

} // namespace tokmon::desk
