#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tokmon::desk {

struct TerminalColor {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};
};

struct TerminalCell {
  std::string grapheme;
  std::string hyperlink;
  TerminalColor foreground{};
  TerminalColor background{};
  bool bold{false};
  bool italic{false};
  bool underline{false};
  bool strikethrough{false};
  bool selected{false};
};

struct TerminalCursor {
  std::uint16_t column{0};
  std::uint16_t row{0};
  bool visible{false};
  bool blinking{false};
  enum class Style { bar, block, underline, hollow_block } style{Style::block};
};

struct TerminalRenderSnapshot {
  std::uint16_t columns{0};
  std::uint16_t rows{0};
  TerminalColor default_foreground{};
  TerminalColor default_background{};
  TerminalColor cursor_color{};
  TerminalCursor cursor{};
  std::vector<TerminalCell> cells;
};

struct TerminalProfile {
  std::string id;
  std::string label;
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  bool available{false};
};

struct TerminalLaunchOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
};

[[nodiscard]] std::vector<TerminalProfile> discover_terminal_profiles();
[[nodiscard]] std::optional<TerminalProfile> resolve_terminal_profile(
    std::string_view id);
[[nodiscard]] std::optional<std::vector<std::string>>
parse_terminal_arguments(std::string_view text, std::string& error);
[[nodiscard]] std::string terminal_selection_text(
    const TerminalRenderSnapshot& snapshot, std::size_t first,
    std::size_t last);
[[nodiscard]] bool terminal_safe_hyperlink(std::string_view uri) noexcept;

enum class TerminalKey {
  unidentified,
  key_a, key_b, key_c, key_d, key_e, key_f, key_g, key_h, key_i, key_j,
  key_k, key_l, key_m, key_n, key_o, key_p, key_q, key_r, key_s, key_t,
  key_u, key_v, key_w, key_x, key_y, key_z,
  digit_0, digit_1, digit_2, digit_3, digit_4,
  digit_5, digit_6, digit_7, digit_8, digit_9,
  enter, backspace, tab, escape, space,
  left, right, up, down, home, end, page_up, page_down,
  insert_key, delete_key,
  f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

enum TerminalModifiers : std::uint16_t {
  terminal_shift = 1u << 0u,
  terminal_ctrl = 1u << 1u,
  terminal_alt = 1u << 2u,
  terminal_super = 1u << 3u,
  terminal_caps_lock = 1u << 4u,
  terminal_num_lock = 1u << 5u,
};

enum class TerminalMouseAction { press, release, motion };
enum class TerminalMouseButton { none, left, right, middle, wheel_up, wheel_down };
enum class TerminalPasteResult { written, empty, unsafe, failed };

// Stable C++ adapter over the pinned libghostty-vt C ABI. No Ghostty types
// escape this boundary, which keeps ElementTerminal and PTY code independent
// from upstream ABI details.
class GhosttyVt final {
 public:
  GhosttyVt(int columns = 80, int rows = 24,
            std::size_t scrollback_lines = 10000);
  ~GhosttyVt();
  GhosttyVt(const GhosttyVt&) = delete;
  GhosttyVt& operator=(const GhosttyVt&) = delete;

  void append(std::string_view bytes);
  [[nodiscard]] std::string plain_text() const;
  [[nodiscard]] TerminalRenderSnapshot render_snapshot() const;
  [[nodiscard]] bool resize(int columns, int rows, int cell_width_px = 8,
                            int cell_height_px = 16);
  void set_response_sink(std::function<void(std::string_view)> sink);
  [[nodiscard]] std::string encode_key(TerminalKey key,
                                       std::string_view utf8,
                                       std::uint16_t modifiers,
                                       bool repeat = false);
  [[nodiscard]] std::string encode_mouse(
      TerminalMouseAction action, TerminalMouseButton button,
      float x, float y, int screen_width, int screen_height,
      int cell_width, int cell_height, std::uint16_t modifiers,
      bool any_button_pressed = false);
  [[nodiscard]] bool mouse_tracking() const;
  [[nodiscard]] TerminalPasteResult paste(std::string_view text,
                                           bool allow_unsafe = false);
  void scroll_viewport(int rows);
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TerminalSession final {
 public:
  TerminalSession();
  ~TerminalSession();
  TerminalSession(const TerminalSession&) = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  [[nodiscard]] bool start(std::string shell, const std::filesystem::path& cwd,
                           int columns, int rows, std::string& error);
  [[nodiscard]] bool start_profile(const TerminalLaunchOptions& launch,
                                   const std::filesystem::path& cwd,
                                   int columns, int rows, std::string& error);
  [[nodiscard]] bool write(std::string_view text, std::string& error);
  [[nodiscard]] bool resize(int columns, int rows, std::string& error);
  [[nodiscard]] std::string take_output(
      std::size_t max_bytes = 128u * 1024u);
  [[nodiscard]] bool running() const noexcept;
  void stop() noexcept;

 private:
  void read_loop();
  std::atomic_bool running_{false};
  std::thread reader_;
  std::mutex output_mutex_;
  std::string output_;
#if defined(_WIN32)
  void* pseudo_console_{nullptr};
  void* input_write_{nullptr};
  void* output_read_{nullptr};
  void* process_{nullptr};
#else
  int master_fd_{-1};
  int process_id_{-1};
#endif
};

} // namespace tokmon::desk
