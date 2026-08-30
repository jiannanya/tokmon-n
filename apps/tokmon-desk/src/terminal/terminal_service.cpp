#include "terminal/terminal_service.hpp"

#include "ui/theme_palette.hpp"

#include <ghostty/vt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace tokmon::desk {

namespace {

std::string environment_value(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || !value)
    return {};
  std::string result(value, length > 0 ? length - 1 : 0);
  std::free(value);
  return result;
#else
  const auto* value = std::getenv(name);
  return value ? std::string(value) : std::string{};
#endif
}

std::filesystem::path environment_path(const char* name) {
  return std::filesystem::path(environment_value(name));
}

std::filesystem::path find_in_path(const std::filesystem::path& name) {
  const auto raw_path = environment_value("PATH");
  if (raw_path.empty())
    return {};
#if defined(_WIN32)
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif
  std::string_view paths(raw_path);
  while (!paths.empty()) {
    const auto end = paths.find(separator);
    const auto part = paths.substr(0, end);
    std::error_code error;
    const auto candidate = std::filesystem::path(part) / name;
    if (std::filesystem::is_regular_file(candidate, error))
      return candidate;
    if (end == std::string_view::npos)
      break;
    paths.remove_prefix(end + 1);
  }
  return {};
}

TerminalColor color(const GhosttyColorRgb value) {
  return {value.r, value.g, value.b};
}

bool formatter_write(void* userdata, const std::uint8_t* data,
                     const std::size_t length) {
  static_cast<std::string*>(userdata)->append(
      reinterpret_cast<const char*>(data), length);
  return true;
}

} // namespace

struct GhosttyVt::Impl {
  GhosttyTerminal terminal{nullptr};
  GhosttyRenderState render_state{nullptr};
  GhosttyKeyEncoder key_encoder{nullptr};
  GhosttyMouseEncoder mouse_encoder{nullptr};
  std::function<void(std::string_view)> response_sink;

  static void write_pty(GhosttyTerminal, void* userdata,
                        const std::uint8_t* data,
                        const std::size_t length) {
    auto* self = static_cast<Impl*>(userdata);
    if (self && self->response_sink)
      self->response_sink(std::string_view(
          reinterpret_cast<const char*>(data), length));
  }

  Impl(const int columns, const int rows, const std::size_t scrollback_lines) {
    const auto cols = static_cast<std::uint16_t>(std::clamp(columns, 1, 65535));
    const auto rows_value = static_cast<std::uint16_t>(std::clamp(rows, 1, 65535));
    if (ghostty_terminal_new(nullptr, &terminal, cols, rows_value) != GHOSTTY_SUCCESS)
      terminal = nullptr;
    if (terminal && ghostty_render_state_new(nullptr, &render_state) != GHOSTTY_SUCCESS)
      render_state = nullptr;
    if (terminal && ghostty_key_encoder_new(nullptr, &key_encoder) != GHOSTTY_SUCCESS)
      key_encoder = nullptr;
    if (terminal && ghostty_mouse_encoder_new(nullptr, &mouse_encoder) !=
                        GHOSTTY_SUCCESS)
      mouse_encoder = nullptr;
    if (terminal) {
      // Terminal is a first-class pane in the light legacy shell, not an
      // unrelated black embed. Ghostty still owns ANSI semantics; its palette
      // is generated against the same warm surface and foreground used by the
      // old Slint UI so explicit ANSI colours retain readable contrast.
      const GhosttyColorRgb foreground{
          legacy_theme::body.red,
          legacy_theme::body.green,
          legacy_theme::body.blue};
      const GhosttyColorRgb background{
          legacy_theme::surface_warm.red,
          legacy_theme::surface_warm.green,
          legacy_theme::surface_warm.blue};
      const GhosttyColorRgb cursor{
          legacy_theme::accent.red,
          legacy_theme::accent.green,
          legacy_theme::accent.blue};
      std::array<GhosttyColorRgb, 256> palette{};
      ghostty_color_palette_default(palette.data());
      ghostty_color_palette_generate(palette.data(), nullptr, &background,
                                     &foreground, true, palette.data());
      const auto bounded_scrollback = std::clamp<std::size_t>(
          scrollback_lines, 1000, 100000);
      (void)ghostty_terminal_set(
          terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground);
      (void)ghostty_terminal_set(
          terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background);
      (void)ghostty_terminal_set(
          terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);
      (void)ghostty_terminal_set(
          terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette.data());
      (void)ghostty_terminal_set(
          terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,
          &bounded_scrollback);
      (void)ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
      (void)ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                                reinterpret_cast<const void*>(write_pty));
    }
  }

  ~Impl() {
    ghostty_mouse_encoder_free(mouse_encoder);
    ghostty_key_encoder_free(key_encoder);
    ghostty_render_state_free(render_state);
    ghostty_terminal_free(terminal);
  }
};

namespace {

GhosttyKey ghostty_key(const TerminalKey key) {
  switch (key) {
    case TerminalKey::key_a: return GHOSTTY_KEY_A;
    case TerminalKey::key_b: return GHOSTTY_KEY_B;
    case TerminalKey::key_c: return GHOSTTY_KEY_C;
    case TerminalKey::key_d: return GHOSTTY_KEY_D;
    case TerminalKey::key_e: return GHOSTTY_KEY_E;
    case TerminalKey::key_f: return GHOSTTY_KEY_F;
    case TerminalKey::key_g: return GHOSTTY_KEY_G;
    case TerminalKey::key_h: return GHOSTTY_KEY_H;
    case TerminalKey::key_i: return GHOSTTY_KEY_I;
    case TerminalKey::key_j: return GHOSTTY_KEY_J;
    case TerminalKey::key_k: return GHOSTTY_KEY_K;
    case TerminalKey::key_l: return GHOSTTY_KEY_L;
    case TerminalKey::key_m: return GHOSTTY_KEY_M;
    case TerminalKey::key_n: return GHOSTTY_KEY_N;
    case TerminalKey::key_o: return GHOSTTY_KEY_O;
    case TerminalKey::key_p: return GHOSTTY_KEY_P;
    case TerminalKey::key_q: return GHOSTTY_KEY_Q;
    case TerminalKey::key_r: return GHOSTTY_KEY_R;
    case TerminalKey::key_s: return GHOSTTY_KEY_S;
    case TerminalKey::key_t: return GHOSTTY_KEY_T;
    case TerminalKey::key_u: return GHOSTTY_KEY_U;
    case TerminalKey::key_v: return GHOSTTY_KEY_V;
    case TerminalKey::key_w: return GHOSTTY_KEY_W;
    case TerminalKey::key_x: return GHOSTTY_KEY_X;
    case TerminalKey::key_y: return GHOSTTY_KEY_Y;
    case TerminalKey::key_z: return GHOSTTY_KEY_Z;
    case TerminalKey::digit_0: return GHOSTTY_KEY_DIGIT_0;
    case TerminalKey::digit_1: return GHOSTTY_KEY_DIGIT_1;
    case TerminalKey::digit_2: return GHOSTTY_KEY_DIGIT_2;
    case TerminalKey::digit_3: return GHOSTTY_KEY_DIGIT_3;
    case TerminalKey::digit_4: return GHOSTTY_KEY_DIGIT_4;
    case TerminalKey::digit_5: return GHOSTTY_KEY_DIGIT_5;
    case TerminalKey::digit_6: return GHOSTTY_KEY_DIGIT_6;
    case TerminalKey::digit_7: return GHOSTTY_KEY_DIGIT_7;
    case TerminalKey::digit_8: return GHOSTTY_KEY_DIGIT_8;
    case TerminalKey::digit_9: return GHOSTTY_KEY_DIGIT_9;
    case TerminalKey::enter: return GHOSTTY_KEY_ENTER;
    case TerminalKey::backspace: return GHOSTTY_KEY_BACKSPACE;
    case TerminalKey::tab: return GHOSTTY_KEY_TAB;
    case TerminalKey::escape: return GHOSTTY_KEY_ESCAPE;
    case TerminalKey::space: return GHOSTTY_KEY_SPACE;
    case TerminalKey::left: return GHOSTTY_KEY_ARROW_LEFT;
    case TerminalKey::right: return GHOSTTY_KEY_ARROW_RIGHT;
    case TerminalKey::up: return GHOSTTY_KEY_ARROW_UP;
    case TerminalKey::down: return GHOSTTY_KEY_ARROW_DOWN;
    case TerminalKey::home: return GHOSTTY_KEY_HOME;
    case TerminalKey::end: return GHOSTTY_KEY_END;
    case TerminalKey::page_up: return GHOSTTY_KEY_PAGE_UP;
    case TerminalKey::page_down: return GHOSTTY_KEY_PAGE_DOWN;
    case TerminalKey::insert_key: return GHOSTTY_KEY_INSERT;
    case TerminalKey::delete_key: return GHOSTTY_KEY_DELETE;
    case TerminalKey::f1: return GHOSTTY_KEY_F1;
    case TerminalKey::f2: return GHOSTTY_KEY_F2;
    case TerminalKey::f3: return GHOSTTY_KEY_F3;
    case TerminalKey::f4: return GHOSTTY_KEY_F4;
    case TerminalKey::f5: return GHOSTTY_KEY_F5;
    case TerminalKey::f6: return GHOSTTY_KEY_F6;
    case TerminalKey::f7: return GHOSTTY_KEY_F7;
    case TerminalKey::f8: return GHOSTTY_KEY_F8;
    case TerminalKey::f9: return GHOSTTY_KEY_F9;
    case TerminalKey::f10: return GHOSTTY_KEY_F10;
    case TerminalKey::f11: return GHOSTTY_KEY_F11;
    case TerminalKey::f12: return GHOSTTY_KEY_F12;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

GhosttyMouseAction ghostty_mouse_action(const TerminalMouseAction action) {
  switch (action) {
    case TerminalMouseAction::press: return GHOSTTY_MOUSE_ACTION_PRESS;
    case TerminalMouseAction::release: return GHOSTTY_MOUSE_ACTION_RELEASE;
    case TerminalMouseAction::motion: return GHOSTTY_MOUSE_ACTION_MOTION;
  }
  return GHOSTTY_MOUSE_ACTION_MOTION;
}

GhosttyMouseButton ghostty_mouse_button(const TerminalMouseButton button) {
  switch (button) {
    case TerminalMouseButton::left: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case TerminalMouseButton::right: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case TerminalMouseButton::middle: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case TerminalMouseButton::wheel_up: return GHOSTTY_MOUSE_BUTTON_FOUR;
    case TerminalMouseButton::wheel_down: return GHOSTTY_MOUSE_BUTTON_FIVE;
    case TerminalMouseButton::none: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
  }
  return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

struct PasteSource {
  std::string_view text;
};

bool read_paste(void* userdata, const GhosttyString mime,
                const GhosttyWriter writer) {
  auto* source = static_cast<PasteSource*>(userdata);
  constexpr std::string_view plain = "text/plain";
  if (!source || mime.len != plain.size() ||
      std::memcmp(mime.ptr, plain.data(), plain.size()) != 0)
    return false;
  return writer.write(
      writer.userdata,
      reinterpret_cast<const std::uint8_t*>(source->text.data()),
      source->text.size());
}

} // namespace

GhosttyVt::GhosttyVt(const int columns, const int rows,
                     const std::size_t scrollback_lines)
    : impl_(std::make_unique<Impl>(columns, rows, scrollback_lines)) {}

GhosttyVt::~GhosttyVt() = default;

void GhosttyVt::append(const std::string_view bytes) {
  if (!impl_->terminal || bytes.empty())
    return;
  ghostty_terminal_vt_write(
      impl_->terminal, reinterpret_cast<const std::uint8_t*>(bytes.data()),
      bytes.size());
}

std::string GhosttyVt::plain_text() const {
  std::string result;
  if (!impl_->terminal)
    return result;
  auto options = GHOSTTY_INIT_SIZED(GhosttyFormatterTerminalOptions);
  options.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
  options.trim = true;
  GhosttyFormatter formatter = nullptr;
  if (ghostty_formatter_terminal_new(nullptr, &formatter, impl_->terminal,
                                     options) != GHOSTTY_SUCCESS)
    return result;
  const GhosttyWriter writer{formatter_write, &result};
  (void)ghostty_formatter_format(formatter, writer);
  ghostty_formatter_free(formatter);
  return result;
}

TerminalRenderSnapshot GhosttyVt::render_snapshot() const {
  TerminalRenderSnapshot snapshot;
  if (!impl_->terminal || !impl_->render_state ||
      ghostty_render_state_update(impl_->render_state, impl_->terminal) !=
          GHOSTTY_SUCCESS)
    return snapshot;

  (void)ghostty_render_state_get(impl_->render_state,
                                GHOSTTY_RENDER_STATE_DATA_COLS,
                                &snapshot.columns);
  (void)ghostty_render_state_get(impl_->render_state,
                                GHOSTTY_RENDER_STATE_DATA_ROWS,
                                &snapshot.rows);
  auto colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
  if (ghostty_render_state_get(impl_->render_state,
                              GHOSTTY_RENDER_STATE_DATA_COLORS,
                              &colors) == GHOSTTY_SUCCESS) {
    snapshot.default_foreground = color(colors.foreground);
    snapshot.default_background = color(colors.background);
    snapshot.cursor_color = color(colors.cursor_has_value ? colors.cursor
                                                           : colors.foreground);
  }
  auto cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
  if (ghostty_render_state_get(impl_->render_state,
                              GHOSTTY_RENDER_STATE_DATA_CURSOR,
                              &cursor) == GHOSTTY_SUCCESS) {
    snapshot.cursor.column = cursor.viewport_has_value ? cursor.viewport_x : 0;
    snapshot.cursor.row = cursor.viewport_has_value ? cursor.viewport_y : 0;
    snapshot.cursor.visible = cursor.visible && cursor.viewport_has_value;
    snapshot.cursor.blinking = cursor.blinking;
    switch (cursor.visual_style) {
      case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        snapshot.cursor.style = TerminalCursor::Style::bar;
        break;
      case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        snapshot.cursor.style = TerminalCursor::Style::underline;
        break;
      case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        snapshot.cursor.style = TerminalCursor::Style::hollow_block;
        break;
      default:
        snapshot.cursor.style = TerminalCursor::Style::block;
        break;
    }
  }

  snapshot.cells.reserve(static_cast<std::size_t>(snapshot.columns) *
                         snapshot.rows);
  GhosttyRenderStateRowIterator rows = nullptr;
  GhosttyRenderStateRowCells cells = nullptr;
  if (ghostty_render_state_row_iterator_new(nullptr, &rows) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_cells_new(nullptr, &cells) != GHOSTTY_SUCCESS ||
      ghostty_render_state_get(impl_->render_state,
                              GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                              &rows) != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
    return snapshot;
  }
  std::uint32_t row_index = 0;
  while (ghostty_render_state_row_iterator_next(rows)) {
    if (ghostty_render_state_row_get(rows,
                                    GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                    &cells) != GHOSTTY_SUCCESS)
      continue;
    std::uint16_t column_index = 0;
    while (ghostty_render_state_row_cells_next(cells)) {
      TerminalCell output;
      output.foreground = snapshot.default_foreground;
      output.background = snapshot.default_background;
      GhosttyColorRgb cell_color{};
      if (ghostty_render_state_row_cells_get(
              cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
              &cell_color) == GHOSTTY_SUCCESS)
        output.foreground = color(cell_color);
      if (ghostty_render_state_row_cells_get(
              cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
              &cell_color) == GHOSTTY_SUCCESS)
        output.background = color(cell_color);
      auto style = GHOSTTY_INIT_SIZED(GhosttyStyle);
      if (ghostty_render_state_row_cells_get(
              cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
              &style) == GHOSTTY_SUCCESS) {
        output.bold = style.bold;
        output.italic = style.italic;
        output.underline = style.underline != 0;
        output.strikethrough = style.strikethrough;
        if (style.inverse)
          std::swap(output.foreground, output.background);
      }
      (void)ghostty_render_state_row_cells_get(
          cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED,
          &output.selected);
      std::array<std::uint8_t, 64> grapheme{};
      GhosttyBuffer buffer{grapheme.data(), grapheme.size(), 0};
      if (ghostty_render_state_row_cells_get(
              cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
              &buffer) == GHOSTTY_SUCCESS && buffer.len > 0)
        output.grapheme.assign(reinterpret_cast<const char*>(grapheme.data()),
                               buffer.len);
      GhosttyCell raw{};
      bool has_hyperlink = false;
      if (ghostty_render_state_row_cells_get(
              cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) ==
              GHOSTTY_SUCCESS &&
          ghostty_cell_get(raw, GHOSTTY_CELL_DATA_HAS_HYPERLINK,
                           &has_hyperlink) == GHOSTTY_SUCCESS &&
          has_hyperlink) {
        GhosttyPoint point{};
        point.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        point.value.coordinate = {column_index, row_index};
        auto reference = GHOSTTY_INIT_SIZED(GhosttyGridRef);
        if (ghostty_terminal_grid_ref(impl_->terminal, point, &reference) ==
            GHOSTTY_SUCCESS) {
          std::size_t length = 0;
          const auto measured = ghostty_grid_ref_hyperlink_uri(
              &reference, nullptr, 0, &length);
          if (measured == GHOSTTY_OUT_OF_SPACE && length > 0 &&
              length <= 64u * 1024u) {
            output.hyperlink.resize(length);
            if (ghostty_grid_ref_hyperlink_uri(
                    &reference,
                    reinterpret_cast<std::uint8_t*>(output.hyperlink.data()),
                    output.hyperlink.size(), &length) == GHOSTTY_SUCCESS)
              output.hyperlink.resize(length);
            else
              output.hyperlink.clear();
          }
        }
      }
      snapshot.cells.push_back(std::move(output));
      ++column_index;
    }
    ++row_index;
  }
  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(rows);
  (void)ghostty_render_state_clean(impl_->render_state);
  return snapshot;
}

bool GhosttyVt::resize(const int columns, const int rows,
                       const int cell_width_px, const int cell_height_px) {
  return impl_->terminal &&
         ghostty_terminal_resize(
             impl_->terminal,
             static_cast<std::uint16_t>(std::clamp(columns, 1, 65535)),
             static_cast<std::uint16_t>(std::clamp(rows, 1, 65535)),
             static_cast<std::uint32_t>(std::max(cell_width_px, 1)),
             static_cast<std::uint32_t>(std::max(cell_height_px, 1))) ==
             GHOSTTY_SUCCESS;
}

void GhosttyVt::set_response_sink(
    std::function<void(std::string_view)> sink) {
  impl_->response_sink = std::move(sink);
}

std::string GhosttyVt::encode_key(const TerminalKey key,
                                  const std::string_view utf8,
                                  const std::uint16_t modifiers,
                                  const bool repeat) {
  std::string output;
  if (!impl_->terminal || !impl_->key_encoder)
    return output;
  ghostty_key_encoder_setopt_from_terminal(impl_->key_encoder, impl_->terminal);
  GhosttyKeyEvent event = nullptr;
  if (ghostty_key_event_new(nullptr, &event) != GHOSTTY_SUCCESS)
    return output;
  ghostty_key_event_set_action(
      event, repeat ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
  ghostty_key_event_set_key(event, ghostty_key(key));
  ghostty_key_event_set_mods(event, static_cast<GhosttyMods>(modifiers));
  if (!utf8.empty())
    ghostty_key_event_set_utf8(event, utf8.data(), utf8.size());
  std::array<char, 256> buffer{};
  std::size_t written = 0;
  auto result = ghostty_key_encoder_encode(impl_->key_encoder, event,
                                            buffer.data(), buffer.size(),
                                            &written);
  if (result == GHOSTTY_SUCCESS) {
    output.assign(buffer.data(), written);
  } else if (result == GHOSTTY_OUT_OF_SPACE && written > 0) {
    output.resize(written);
    if (ghostty_key_encoder_encode(impl_->key_encoder, event, output.data(),
                                   output.size(), &written) == GHOSTTY_SUCCESS)
      output.resize(written);
    else
      output.clear();
  }
  ghostty_key_event_free(event);
  return output;
}

std::string GhosttyVt::encode_mouse(
    const TerminalMouseAction action, const TerminalMouseButton button,
    const float x, const float y, const int screen_width,
    const int screen_height, const int cell_width, const int cell_height,
    const std::uint16_t modifiers, const bool any_button_pressed) {
  std::string output;
  if (!impl_->terminal || !impl_->mouse_encoder)
    return output;
  ghostty_mouse_encoder_setopt_from_terminal(impl_->mouse_encoder,
                                              impl_->terminal);
  auto size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
  size.screen_width = static_cast<std::uint32_t>(std::max(screen_width, 1));
  size.screen_height = static_cast<std::uint32_t>(std::max(screen_height, 1));
  size.cell_width = static_cast<std::uint32_t>(std::max(cell_width, 1));
  size.cell_height = static_cast<std::uint32_t>(std::max(cell_height, 1));
  ghostty_mouse_encoder_setopt(impl_->mouse_encoder,
                               GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
  ghostty_mouse_encoder_setopt(impl_->mouse_encoder,
                               GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                               &any_button_pressed);
  GhosttyMouseEvent event = nullptr;
  if (ghostty_mouse_event_new(nullptr, &event) != GHOSTTY_SUCCESS)
    return output;
  ghostty_mouse_event_set_action(event, ghostty_mouse_action(action));
  if (button == TerminalMouseButton::none)
    ghostty_mouse_event_clear_button(event);
  else
    ghostty_mouse_event_set_button(event, ghostty_mouse_button(button));
  ghostty_mouse_event_set_mods(event, static_cast<GhosttyMods>(modifiers));
  ghostty_mouse_event_set_position(event, GhosttyMousePosition{x, y});
  std::array<char, 128> buffer{};
  std::size_t written = 0;
  const auto result = ghostty_mouse_encoder_encode(
      impl_->mouse_encoder, event, buffer.data(), buffer.size(), &written);
  if (result == GHOSTTY_SUCCESS)
    output.assign(buffer.data(), written);
  ghostty_mouse_event_free(event);
  return output;
}

bool GhosttyVt::mouse_tracking() const {
  bool enabled = false;
  return impl_->terminal &&
         ghostty_terminal_get(impl_->terminal,
                              GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                              &enabled) == GHOSTTY_SUCCESS &&
         enabled;
}

TerminalPasteResult GhosttyVt::paste(const std::string_view text,
                                     const bool allow_unsafe) {
  if (!impl_->terminal || text.empty())
    return text.empty() ? TerminalPasteResult::empty
                        : TerminalPasteResult::failed;
  PasteSource source{text};
  constexpr std::string_view plain = "text/plain";
  const GhosttyString mime{
      reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()};
  auto paste = GHOSTTY_INIT_SIZED(GhosttyPaste);
  paste.location = GHOSTTY_CLIPBOARD_LOCATION_STANDARD;
  paste.source = GHOSTTY_PASTE_SOURCE_CLIPBOARD;
  paste.mimes = &mime;
  paste.mimes_len = 1;
  paste.reader = GhosttyMimeReader{read_paste, &source};
  paste.allow_unsafe = allow_unsafe;
  bool written = false;
  const auto result = ghostty_terminal_paste(impl_->terminal, &paste, &written);
  if (result == GHOSTTY_REJECTED)
    return TerminalPasteResult::unsafe;
  if (result != GHOSTTY_SUCCESS)
    return TerminalPasteResult::failed;
  return written ? TerminalPasteResult::written : TerminalPasteResult::empty;
}

void GhosttyVt::scroll_viewport(const int rows) {
  if (!impl_->terminal || rows == 0)
    return;
  GhosttyTerminalScrollViewport behavior{};
  behavior.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
  behavior.value.delta = rows;
  ghostty_terminal_scroll_viewport(impl_->terminal, behavior);
}

void GhosttyVt::clear() {
  if (impl_->terminal)
    ghostty_terminal_reset(impl_->terminal);
}

TerminalSession::TerminalSession() = default;
TerminalSession::~TerminalSession() { stop(); }

bool TerminalSession::start(std::string shell, const std::filesystem::path& cwd,
                            int columns, int rows, std::string& error) {
  TerminalLaunchOptions launch;
  launch.executable = std::move(shell);
  return start_profile(launch, cwd, columns, rows, error);
}

bool TerminalSession::start_profile(const TerminalLaunchOptions& launch,
                                    const std::filesystem::path& cwd,
                                    int columns, int rows, std::string& error) {
  stop();
#if defined(_WIN32)
  HANDLE input_read = nullptr;
  HANDLE input_write = nullptr;
  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  if (!CreatePipe(&input_read, &input_write, &security, 0) ||
      !CreatePipe(&output_read, &output_write, &security, 0)) {
    error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
  HPCON pseudo_console = nullptr;
  const COORD size{static_cast<SHORT>(std::clamp(columns, 1, 32767)),
                   static_cast<SHORT>(std::clamp(rows, 1, 32767))};
  const auto pseudo_result = CreatePseudoConsole(size, input_read, output_write, 0,
                                                  &pseudo_console);
  CloseHandle(input_read);
  CloseHandle(output_write);
  if (FAILED(pseudo_result)) {
    CloseHandle(input_write);
    CloseHandle(output_read);
    error = "CreatePseudoConsole failed";
    return false;
  }

  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<std::byte> attribute_storage(attribute_size);
  auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                 pseudo_console, sizeof(pseudo_console), nullptr, nullptr)) {
    ClosePseudoConsole(pseudo_console);
    CloseHandle(input_write);
    CloseHandle(output_read);
    error = "cannot initialize ConPTY process attributes";
    return false;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  // Prevent redirected host handles (CTest, services, IDE terminals) from
  // bypassing the pseudoconsole. ConPTY supplies the child terminal handles.
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nullptr;
  startup.StartupInfo.hStdOutput = nullptr;
  startup.StartupInfo.hStdError = nullptr;
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process{};
  auto shell_path = launch.executable.wstring();
  if (shell_path.empty())
    shell_path = L"powershell.exe";
  const auto quote = [](const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
      if (character == L'\\') {
        ++backslashes;
      } else if (character == L'\"') {
        result.append(backslashes * 2 + 1, L'\\');
        result.push_back(L'\"');
        backslashes = 0;
      } else {
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
      }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
  };
  std::wstring command = quote(shell_path);
  for (const auto& argument : launch.arguments) {
    command.push_back(L' ');
    command += quote(std::filesystem::path(argument).wstring());
  }
  auto cwd_wide = cwd.wstring();
  const BOOL created = CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, FALSE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      nullptr, cwd_wide.empty() ? nullptr : cwd_wide.c_str(), &startup.StartupInfo, &process);
  DeleteProcThreadAttributeList(attributes);
  if (!created) {
    ClosePseudoConsole(pseudo_console);
    CloseHandle(input_write);
    CloseHandle(output_read);
    error = "cannot start shell in ConPTY";
    return false;
  }
  ResumeThread(process.hThread);
  CloseHandle(process.hThread);
  pseudo_console_ = pseudo_console;
  input_write_ = input_write;
  output_read_ = output_read;
  process_ = process.hProcess;
#else
  winsize size{};
  size.ws_col = static_cast<unsigned short>(std::clamp(columns, 1, 65535));
  size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
  int master = -1;
  const auto pid = forkpty(&master, nullptr, nullptr, &size);
  if (pid < 0) {
    error = std::string("forkpty failed: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    if (!cwd.empty())
      (void)chdir(cwd.c_str());
    auto shell = launch.executable.string();
    if (shell.empty()) {
      const auto* environment_shell = std::getenv("SHELL");
      shell = environment_shell && *environment_shell ? environment_shell : "/bin/sh";
    }
    std::vector<std::string> values;
    values.push_back(shell);
    if (launch.arguments.empty())
      values.push_back("-l");
    else
      values.insert(values.end(), launch.arguments.begin(), launch.arguments.end());
    std::vector<char*> argv;
    argv.reserve(values.size() + 1);
    for (auto& value : values)
      argv.push_back(value.data());
    argv.push_back(nullptr);
    execv(shell.c_str(), argv.data());
    _exit(127);
  }
  master_fd_ = master;
  process_id_ = static_cast<int>(pid);
#endif
  running_.store(true);
  reader_ = std::thread([this] { read_loop(); });
  return true;
}

std::vector<TerminalProfile> discover_terminal_profiles() {
  std::vector<TerminalProfile> profiles;
#if defined(_WIN32)
  const auto system_root = environment_path("SystemRoot");
  const auto powershell = system_root.empty() ? std::filesystem::path{}
      : system_root / "System32" / "WindowsPowerShell" / "v1.0" /
            "powershell.exe";
  const auto command = environment_path("ComSpec");
  const auto pwsh = find_in_path("pwsh.exe");
  const auto wsl = find_in_path("wsl.exe");
  profiles.push_back({"auto", "PowerShell（自动）",
                      !pwsh.empty() ? pwsh : powershell, {},
                      !pwsh.empty() || std::filesystem::exists(powershell)});
  profiles.push_back({"pwsh", "PowerShell 7", pwsh, {}, !pwsh.empty()});
  profiles.push_back({"windows-powershell", "Windows PowerShell", powershell,
                      {}, std::filesystem::exists(powershell)});
  profiles.push_back({"cmd", "命令提示符", command, {},
                      !command.empty() && std::filesystem::exists(command)});
  profiles.push_back({"wsl", "WSL", wsl, {}, !wsl.empty()});
#else
  auto shell = environment_path("SHELL");
  if (shell.empty())
    shell = "/bin/sh";
  profiles.push_back({"auto", "登录 Shell（自动）", shell, {},
                      std::filesystem::exists(shell)});
  for (const auto& [id, label] : {
           std::pair{"zsh", "zsh"}, std::pair{"bash", "bash"},
           std::pair{"fish", "fish"}, std::pair{"sh", "sh"}}) {
    auto executable = find_in_path(id);
    profiles.push_back({id, label, executable, {}, !executable.empty()});
  }
#endif
  return profiles;
}

std::optional<TerminalProfile> resolve_terminal_profile(
    const std::string_view id) {
  auto profiles = discover_terminal_profiles();
  const auto found = std::ranges::find(profiles, id, &TerminalProfile::id);
  if (found != profiles.end() && found->available)
    return *found;
  const auto automatic = std::ranges::find(profiles, std::string_view("auto"),
                                           &TerminalProfile::id);
  return automatic != profiles.end() && automatic->available
      ? std::optional<TerminalProfile>(*automatic) : std::nullopt;
}

std::optional<std::vector<std::string>> parse_terminal_arguments(
    const std::string_view text, std::string& error) {
  error.clear();
  std::vector<std::string> result;
  std::string current;
  char quote = 0;
  bool escaping = false;
  for (const char character : text) {
    if (escaping) {
      current.push_back(character);
      escaping = false;
    } else if (character == '\\') {
      if (quote == '\'')
        current.push_back(character);
      else
        escaping = true;
    } else if (quote) {
      if (character == quote)
        quote = 0;
      else
        current.push_back(character);
    } else if (character == '\'' || character == '"') {
      quote = character;
    } else if (std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (escaping)
    current.push_back('\\');
  if (quote) {
    error = "unterminated quote in terminal arguments";
    return std::nullopt;
  }
  if (!current.empty())
    result.push_back(std::move(current));
  return result;
}

std::string terminal_selection_text(const TerminalRenderSnapshot& snapshot,
                                    const std::size_t first,
                                    const std::size_t last) {
  if (snapshot.cells.empty() || snapshot.columns == 0 ||
      first >= snapshot.cells.size())
    return {};
  const auto bounded_last = std::min(last, snapshot.cells.size() - 1);
  std::string result;
  for (auto index = first; index <= bounded_last; ++index) {
    const auto& cell = snapshot.cells[index];
    result += cell.grapheme.empty() ? " " : cell.grapheme;
    if (index != bounded_last && (index + 1) % snapshot.columns == 0) {
      while (!result.empty() && result.back() == ' ')
        result.pop_back();
      result.push_back('\n');
    }
  }
  while (!result.empty() && result.back() == ' ')
    result.pop_back();
  return result;
}

bool terminal_safe_hyperlink(const std::string_view uri) noexcept {
  return uri.starts_with("https://") || uri.starts_with("http://");
}

bool TerminalSession::write(std::string_view text, std::string& error) {
  if (!running()) {
    error = "terminal is not running";
    return false;
  }
#if defined(_WIN32)
  DWORD written = 0;
  if (!WriteFile(static_cast<HANDLE>(input_write_), text.data(),
                 static_cast<DWORD>(text.size()), &written, nullptr)) {
    error = "ConPTY write failed";
    return false;
  }
#else
  if (::write(master_fd_, text.data(), text.size()) < 0) {
    error = std::string("PTY write failed: ") + std::strerror(errno);
    return false;
  }
#endif
  return true;
}

bool TerminalSession::resize(int columns, int rows, std::string& error) {
#if defined(_WIN32)
  if (!pseudo_console_) {
    error = "terminal is not running";
    return false;
  }
  const COORD size{static_cast<SHORT>(std::clamp(columns, 1, 32767)),
                   static_cast<SHORT>(std::clamp(rows, 1, 32767))};
  if (FAILED(ResizePseudoConsole(static_cast<HPCON>(pseudo_console_), size))) {
    error = "ConPTY resize failed";
    return false;
  }
#else
  if (master_fd_ < 0) {
    error = "terminal is not running";
    return false;
  }
  winsize size{};
  size.ws_col = static_cast<unsigned short>(std::clamp(columns, 1, 65535));
  size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
  if (ioctl(master_fd_, TIOCSWINSZ, &size) != 0) {
    error = std::string("PTY resize failed: ") + std::strerror(errno);
    return false;
  }
#endif
  return true;
}

void TerminalSession::read_loop() {
  std::array<char, 8192> buffer{};
  while (running_.load()) {
#if defined(_WIN32)
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(output_read_), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0)
      break;
    const auto size = static_cast<std::size_t>(read);
#else
    const auto read = ::read(master_fd_, buffer.data(), buffer.size());
    if (read <= 0)
      break;
    const auto size = static_cast<std::size_t>(read);
#endif
    std::scoped_lock lock(output_mutex_);
    output_.append(buffer.data(), size);
    constexpr std::size_t max_pending = 2u * 1024u * 1024u;
    if (output_.size() > max_pending)
      output_.erase(0, output_.size() - max_pending);
  }
  running_.store(false);
}

std::string TerminalSession::take_output(const std::size_t max_bytes) {
  std::scoped_lock lock(output_mutex_);
  if (max_bytes == 0 || output_.empty())
    return {};
  const auto count = std::min(max_bytes, output_.size());
  std::string result(output_.data(), count);
  output_.erase(0, count);
  return result;
}

bool TerminalSession::running() const noexcept { return running_.load(); }

void TerminalSession::stop() noexcept {
  running_.store(false);
#if defined(_WIN32)
  if (input_write_) {
    CloseHandle(static_cast<HANDLE>(input_write_));
    input_write_ = nullptr;
  }
  if (pseudo_console_) {
    ClosePseudoConsole(static_cast<HPCON>(pseudo_console_));
    pseudo_console_ = nullptr;
  }
  if (process_) {
    WaitForSingleObject(static_cast<HANDLE>(process_), 300);
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT)
      TerminateProcess(static_cast<HANDLE>(process_), 0);
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
  if (output_read_) {
    CloseHandle(static_cast<HANDLE>(output_read_));
    output_read_ = nullptr;
  }
#else
  if (master_fd_ >= 0) {
    close(master_fd_);
    master_fd_ = -1;
  }
  if (process_id_ > 0) {
    kill(process_id_, SIGHUP);
    (void)waitpid(process_id_, nullptr, WNOHANG);
    process_id_ = -1;
  }
#endif
  if (reader_.joinable())
    reader_.join();
}

} // namespace tokmon::desk
