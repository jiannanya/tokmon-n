#include "platform_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace tokmon::desktop {

std::string display_utf8(const std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size();) {
    const auto lead = static_cast<unsigned char>(input[index]);
    std::size_t width = 0;
    std::uint32_t codepoint = 0;
    if (lead <= 0x7fu) {
      width = 1;
      codepoint = lead;
    } else if (lead >= 0xc2u && lead <= 0xdfu) {
      width = 2;
      codepoint = lead & 0x1fu;
    } else if (lead >= 0xe0u && lead <= 0xefu) {
      width = 3;
      codepoint = lead & 0x0fu;
    } else if (lead >= 0xf0u && lead <= 0xf4u) {
      width = 4;
      codepoint = lead & 0x07u;
    }
    bool valid = width != 0 && index + width <= input.size();
    for (std::size_t offset = 1; valid && offset < width; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(input[index + offset]);
      if ((continuation & 0xc0u) != 0x80u)
        valid = false;
      else
        codepoint = (codepoint << 6u) | (continuation & 0x3fu);
    }
    if (valid) {
      valid = (width != 2 || codepoint >= 0x80u) &&
              (width != 3 || codepoint >= 0x800u) &&
              (width != 4 || codepoint >= 0x10000u) &&
              !(codepoint >= 0xd800u && codepoint <= 0xdfffu) &&
              codepoint <= 0x10ffffu;
    }
    if (valid) {
      output.append(input.substr(index, width));
      index += width;
    } else {
      output.append("\xef\xbf\xbd");
      ++index;
    }
  }
  return output;
}

slint::SharedString display_string(const std::string_view input) {
  return slint::SharedString(display_utf8(input));
}

void copy_to_clipboard(const std::string_view text) {
#if defined(_WIN32)
  if (!OpenClipboard(nullptr))
    return;
  EmptyClipboard();
  const auto required = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (required > 0) {
    const auto allocation =
        GlobalAlloc(GMEM_MOVEABLE, (static_cast<std::size_t>(required) + 1u) *
                                       sizeof(wchar_t));
    if (allocation) {
      auto *buffer = static_cast<wchar_t *>(GlobalLock(allocation));
      if (buffer) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()), buffer, required);
        buffer[required] = L'\0';
        GlobalUnlock(allocation);
        if (!SetClipboardData(CF_UNICODETEXT, allocation))
          GlobalFree(allocation);
      } else
        GlobalFree(allocation);
    }
  }
  CloseClipboard();
#else
  (void)text;
#endif
}

#if defined(_WIN32)
HWND current_process_window();
#endif

std::string utf8_path(const std::wstring_view path) {
#if defined(_WIN32)
  if (path.empty())
    return {};
  const auto required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
      nullptr, 0, nullptr, nullptr);
  if (required <= 0)
    return {};
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
                          static_cast<int>(path.size()), result.data(),
                          required, nullptr, nullptr) != required)
    return {};
  return result;
#else
  (void)path;
  return {};
#endif
}

std::string choose_attachment(const bool directory) {
#if defined(_WIN32)
  if (directory) {
    // COM state is thread-local; the UI thread usually holds an STA from
    // OLE initialization, so only balance a successful init here.
    const auto com_init =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string selected;
    if (SUCCEEDED(com_init)) {
      // The Vista+ common item dialog offers the full Explorer-style folder
      // browser; SHBrowseForFolderW only shows the small tree window.
      IFileOpenDialog *folder_dialog = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                     reinterpret_cast<void **>(&folder_dialog))) &&
          folder_dialog) {
        DWORD options = 0;
        if (SUCCEEDED(folder_dialog->GetOptions(&options)))
          folder_dialog->SetOptions(options | FOS_PICKFOLDERS |
                                    FOS_FORCEFILESYSTEM);
        folder_dialog->SetTitle(L"选择要交给 Tokmon 的文件夹");
        if (folder_dialog->Show(current_process_window()) == S_OK) {
          IShellItem *chosen = nullptr;
          if (SUCCEEDED(folder_dialog->GetResult(&chosen)) && chosen) {
            wchar_t *wide_path = nullptr;
            if (SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH,
                                                 &wide_path)) &&
                wide_path) {
              selected = utf8_path(std::wstring_view(wide_path));
              CoTaskMemFree(wide_path);
            }
            chosen->Release();
          }
        }
        folder_dialog->Release();
      }
      CoUninitialize();
    }
    return selected;
  }
  std::wstring path(32'768, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = current_process_window();
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrFilter = L"所有文件\0*.*\0\0";
  dialog.lpstrTitle = L"选择要交给 Tokmon 的文件";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (!GetOpenFileNameW(&dialog))
    return {};
  path.resize(std::wcslen(path.c_str()));
  return utf8_path(path);
#else
  (void)directory;
  return {};
#endif
}

std::filesystem::path path_from_utf8(const std::string_view value) {
#if defined(_WIN32)
  const auto *first = reinterpret_cast<const char8_t *>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
#else
  return std::filesystem::path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path &value) {
  const auto encoded = value.generic_u8string();
  return std::string(reinterpret_cast<const char *>(encoded.data()),
                     encoded.size());
}

std::string path_basename_utf8(const std::string_view value) {
  std::string clean(value);
  while (!clean.empty() && (clean.back() == '/' || clean.back() == '\\'))
    clean.pop_back();
  const auto pos = clean.find_last_of("/\\");
  return pos == std::string::npos ? clean : clean.substr(pos + 1);
}

std::optional<std::filesystem::path> normalize_workspace_path(
    const std::string_view value,
    const std::optional<std::filesystem::path> &relative_to) {
  if (value.empty() || value.size() > 4'096u ||
      value.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::error_code error;
  auto path = path_from_utf8(value);
  if (path.is_relative()) {
    path = relative_to ? *relative_to / path
                       : std::filesystem::absolute(path, error);
    if (error)
      return std::nullopt;
  }
  const auto original = path;
  path = std::filesystem::weakly_canonical(original, error);
  if (error) {
    error.clear();
    path = std::filesystem::absolute(original, error).lexically_normal();
    if (error)
      return std::nullopt;
  }
  return path.lexically_normal();
}

bool same_workspace(const std::filesystem::path &left,
                    const std::filesystem::path &right) {
  std::error_code left_error;
  std::error_code right_error;
  const auto normalized_left =
      std::filesystem::weakly_canonical(left, left_error);
  const auto normalized_right =
      std::filesystem::weakly_canonical(right, right_error);
  const auto &lhs = left_error ? left.lexically_normal() : normalized_left;
  const auto &rhs = right_error ? right.lexically_normal() : normalized_right;
#if defined(_WIN32)
  auto lhs_text = lhs.wstring();
  auto rhs_text = rhs.wstring();
  std::ranges::transform(lhs_text, lhs_text.begin(), [](const wchar_t value) {
    return std::towlower(value);
  });
  std::ranges::transform(rhs_text, rhs_text.begin(), [](const wchar_t value) {
    return std::towlower(value);
  });
  return lhs_text == rhs_text;
#else
  return lhs == rhs;
#endif
}

int default_ui_scale_percent_for_resolution(const std::uint32_t width,
                                            const std::uint32_t height) {
  const auto long_edge = std::max(width, height);
  const auto short_edge = std::min(width, height);
  return long_edge >= 3840u && short_edge >= 2160u ? 125 : 100;
}

int default_ui_scale_percent_for_primary_display() {
#if defined(_WIN32)
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
    return default_ui_scale_percent_for_resolution(mode.dmPelsWidth,
                                                   mode.dmPelsHeight);
#endif
  return 100;
}

#if defined(_WIN32)
HWND current_process_window() {
  struct Search {
    DWORD process_id;
    HWND window;
    std::uint64_t largest_area;
  } search{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND candidate, LPARAM context) -> BOOL {
        auto *search = reinterpret_cast<Search *>(context);
        DWORD process_id = 0;
        GetWindowThreadProcessId(candidate, &process_id);
        if (process_id != search->process_id || !IsWindowVisible(candidate))
          return TRUE;
        // Winit also exposes a same-sized "Thread Event Target" proxy HWND.
        // It is not the Slint surface and cannot receive caption, close, or
        // file-dialog ownership operations. The real desktop window carries
        // the application title.
        if (GetWindowTextLengthW(candidate) <= 0)
          return TRUE;
        RECT bounds{};
        if (!GetWindowRect(candidate, &bounds))
          return TRUE;
        const auto width = std::max<LONG>(0, bounds.right - bounds.left);
        const auto height = std::max<LONG>(0, bounds.bottom - bounds.top);
        const auto area = static_cast<std::uint64_t>(width) *
                          static_cast<std::uint64_t>(height);
        if (area > search->largest_area) {
          search->largest_area = area;
          search->window = candidate;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.window;
}

void make_current_process_window_frameless() {
  const auto hwnd = current_process_window();
  if (!hwnd)
    return;
  auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  style &=
      ~(static_cast<LONG_PTR>(WS_CAPTION) | static_cast<LONG_PTR>(WS_SYSMENU) |
        static_cast<LONG_PTR>(WS_MINIMIZEBOX) |
        static_cast<LONG_PTR>(WS_MAXIMIZEBOX));
  style |= static_cast<LONG_PTR>(WS_THICKFRAME);
  SetWindowLongPtrW(hwnd, GWL_STYLE, style);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE);
}

void activate_current_process_window() {
  const auto hwnd = current_process_window();
  if (!hwnd)
    return;
  // Applying the custom frame above deliberately avoids activation so Windows
  // does not interpret the style refresh as a second window presentation. Once
  // the frame is stable, explicitly activate the real Slint HWND exactly once;
  // otherwise the first click is consumed by Windows and hover/click handling
  // inside the desktop UI only starts after that click.
  ShowWindow(hwnd, SW_SHOW);
  SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  SetFocus(hwnd);
}

void set_current_process_window_topmost(const bool enabled) {
  const auto hwnd = current_process_window();
  if (!hwnd)
    return;
  SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}
#endif

#if !defined(_WIN32)
void set_current_process_window_topmost(const bool) {}
#endif

#if defined(_WIN32)
namespace {
struct ProcessWindowDragState {
  HWND window = nullptr;
  POINT cursor_origin{};
  POINT window_origin{};
  bool active = false;
};

ProcessWindowDragState &process_window_drag_state() {
  static ProcessWindowDragState state;
  return state;
}
} // namespace
#endif

void drag_current_process_window() {
#if defined(_WIN32)
  auto &state = process_window_drag_state();
  state.active = false;
  const auto hwnd = current_process_window();
  RECT bounds{};
  POINT cursor{};
  if (!hwnd || !GetWindowRect(hwnd, &bounds) || !GetCursorPos(&cursor))
    return;
  state.window = hwnd;
  state.cursor_origin = cursor;
  state.window_origin = POINT{bounds.left, bounds.top};
  state.active = true;
#endif
}

void update_current_process_window_drag() {
#if defined(_WIN32)
  auto &state = process_window_drag_state();
  if (!state.active || !IsWindow(state.window))
    return;
  if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
    state.active = false;
    return;
  }
  POINT cursor{};
  if (!GetCursorPos(&cursor))
    return;
  // Screen-coordinate deltas stay correct across DPI/UI-scale changes. This
  // deliberately changes position only; Slint's 7px border owns resizing.
  SetWindowPos(state.window, nullptr,
               state.window_origin.x + cursor.x - state.cursor_origin.x,
               state.window_origin.y + cursor.y - state.cursor_origin.y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void end_current_process_window_drag() {
#if defined(_WIN32)
  process_window_drag_state().active = false;
#endif
}

} // namespace tokmon::desktop
