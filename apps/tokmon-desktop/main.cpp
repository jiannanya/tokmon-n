#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

namespace {

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
      const auto continuation = static_cast<unsigned char>(input[index + offset]);
      if ((continuation & 0xc0u) != 0x80u) valid = false;
      else codepoint = (codepoint << 6u) | (continuation & 0x3fu);
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
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  const auto required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
      static_cast<int>(text.size()), nullptr, 0);
  if (required > 0) {
    const auto allocation = GlobalAlloc(GMEM_MOVEABLE,
        (static_cast<std::size_t>(required) + 1u) * sizeof(wchar_t));
    if (allocation) {
      auto* buffer = static_cast<wchar_t*>(GlobalLock(allocation));
      if (buffer) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            buffer, required);
        buffer[required] = L'\0';
        GlobalUnlock(allocation);
        if (!SetClipboardData(CF_UNICODETEXT, allocation)) GlobalFree(allocation);
      } else GlobalFree(allocation);
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
  if (path.empty()) return {};
  const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
      static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) return {};
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
      static_cast<int>(path.size()), result.data(), required, nullptr, nullptr) != required)
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
    BROWSEINFOW browse{};
    browse.hwndOwner = current_process_window();
    browse.lpszTitle = L"选择要交给 Tokmon 的文件夹";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    auto* selected = SHBrowseForFolderW(&browse);
    if (!selected) return {};
    wchar_t path[MAX_PATH]{};
    const auto resolved = SHGetPathFromIDListW(selected, path) != FALSE;
    CoTaskMemFree(selected);
    return resolved ? utf8_path(path) : std::string{};
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
  if (!GetOpenFileNameW(&dialog)) return {};
  path.resize(std::wcslen(path.c_str()));
  return utf8_path(path);
#else
  (void)directory;
  return {};
#endif
}

std::filesystem::path path_from_utf8(const std::string_view value) {
#if defined(_WIN32)
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
#else
  return std::filesystem::path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path& value) {
  const auto encoded = value.generic_u8string();
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

std::optional<std::filesystem::path> normalize_workspace_path(
    const std::string_view value,
    const std::optional<std::filesystem::path>& relative_to = std::nullopt) {
  if (value.empty() || value.size() > 4'096u || value.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::error_code error;
  auto path = path_from_utf8(value);
  if (path.is_relative()) {
    path = relative_to ? *relative_to / path : std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
  }
  const auto original = path;
  path = std::filesystem::weakly_canonical(original, error);
  if (error) {
    error.clear();
    path = std::filesystem::absolute(original, error).lexically_normal();
    if (error) return std::nullopt;
  }
  return path.lexically_normal();
}

bool same_workspace(const std::filesystem::path& left,
                    const std::filesystem::path& right) {
  std::error_code left_error;
  std::error_code right_error;
  const auto normalized_left = std::filesystem::weakly_canonical(left, left_error);
  const auto normalized_right = std::filesystem::weakly_canonical(right, right_error);
  const auto& lhs = left_error ? left.lexically_normal() : normalized_left;
  const auto& rhs = right_error ? right.lexically_normal() : normalized_right;
#if defined(_WIN32)
  auto lhs_text = lhs.wstring();
  auto rhs_text = rhs.wstring();
  std::ranges::transform(lhs_text, lhs_text.begin(),
                         [](const wchar_t value) { return std::towlower(value); });
  std::ranges::transform(rhs_text, rhs_text.begin(),
                         [](const wchar_t value) { return std::towlower(value); });
  return lhs_text == rhs_text;
#else
  return lhs == rhs;
#endif
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
        auto* search = reinterpret_cast<Search*>(context);
        DWORD process_id = 0;
        GetWindowThreadProcessId(candidate, &process_id);
        if (process_id != search->process_id || !IsWindowVisible(candidate))
          return TRUE;
        // Winit also exposes a same-sized "Thread Event Target" proxy HWND.
        // It is not the Slint surface and cannot receive caption, close, or
        // file-dialog ownership operations. The real desktop window carries
        // the application title.
        if (GetWindowTextLengthW(candidate) <= 0) return TRUE;
        RECT bounds{};
        if (!GetWindowRect(candidate, &bounds)) return TRUE;
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
  if (!hwnd) return;
  auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  style &= ~(static_cast<LONG_PTR>(WS_CAPTION) |
             static_cast<LONG_PTR>(WS_SYSMENU) |
             static_cast<LONG_PTR>(WS_MINIMIZEBOX) |
             static_cast<LONG_PTR>(WS_MAXIMIZEBOX));
  style |= static_cast<LONG_PTR>(WS_THICKFRAME);
  SetWindowLongPtrW(hwnd, GWL_STYLE, style);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE);
}
#endif

slint::SharedString time_label(const std::int64_t unix_ms) {
  const auto time = static_cast<std::time_t>(unix_ms / 1000);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::ostringstream stream;
  stream << std::put_time(&local, "%H:%M");
  return slint::SharedString(stream.str());
}

TimelineItem timeline_item(const tokmon::Photon& photon) {
  TimelineItem item;
  item.time = time_label(photon.committed_at_ms);
  item.kind = display_string(photon.kind);
  item.title = display_string(photon.kind);
  item.detail = display_string(tokmon::cbor::diagnostic(photon.payload));
  item.progress = -1;
  if (photon.kind == "act.failed" || photon.kind == "act.rejected") item.tone = "danger";
  else if (photon.kind == "act.completed" || photon.kind == "assistant.message" ||
           photon.kind == "tool.result" || photon.kind == "ray.darkened") item.tone = "success";
  else if (photon.kind.starts_with("act.")) item.tone = "warning";
  else item.tone = "neutral";
  if (photon.kind == "worker.progress") {
    if (const auto* progress = tokmon::cbor::find(photon.payload, "percent"))
      item.progress = static_cast<int>(progress->as_integer());
  }
  return item;
}

std::string payload_text(const tokmon::cbor::Value& payload,
                         const std::string_view key) {
  const auto* value = tokmon::cbor::find(payload, key);
  if (!value) return {};
  if (std::holds_alternative<std::string>(value->data))
    return std::string(value->as_string());
  return tokmon::cbor::diagnostic(*value);
}

std::string bounded_detail(std::string value, const std::size_t capacity = 220u) {
  if (value.size() <= capacity) return value;
  value.resize(capacity);
  return display_utf8(value) + "…";
}

std::string joined_detail(std::initializer_list<std::string> parts) {
  std::string result;
  for (auto& part : parts) {
    if (part.empty()) continue;
    if (!result.empty()) result.append(" · ");
    result.append(part);
  }
  return bounded_detail(std::move(result));
}

std::string act_field(const tokmon::Photon& photon, const std::string_view key) {
  const auto* act = tokmon::cbor::find(photon.payload, "act");
  return act ? payload_text(*act, key) : std::string{};
}

std::string attempt_detail(const tokmon::Photon& photon) {
  const auto attempt = payload_text(photon.payload, "attempt");
  return joined_detail({payload_text(photon.payload, "provider"),
                        payload_text(photon.payload, "model"),
                        attempt.empty() ? std::string{} : "第 " + attempt + " 次"});
}

std::optional<TimelineItem> conversation_workflow_item(const tokmon::Photon& photon) {
  TimelineItem item;
  item.time = time_label(photon.committed_at_ms);
  item.kind = display_string(photon.kind);
  item.progress = -1;
  item.tone = "neutral";
  std::string title;
  std::string detail;

  if (photon.kind == "model.tool-call") {
    const auto tool = payload_text(photon.payload, "tool");
    const auto* arguments = tokmon::cbor::find(photon.payload, "arguments");
    title = tool == "write_file" ? "Agent 准备写入文件" :
            tool == "read_file" ? "Agent 准备回读验证" :
            tool == "run_command" ? "Agent 准备运行验证命令" :
            tool == "calculate" ? "Agent 准备计算" : "Agent 调用工具：" + tool;
    detail = arguments ? bounded_detail(tokmon::cbor::diagnostic(*arguments), 320) : tool;
    item.tone = "warning";
  } else if (photon.kind == "model.failed") {
    title = "Agent 无法继续处理";
    detail = payload_text(photon.payload, "error");
    item.tone = "danger";
  } else if (photon.kind == "act.started") {
    return std::nullopt;
  } else if (photon.kind == "act.completed") {
    return std::nullopt;
  } else if (photon.kind == "act.failed" || photon.kind == "act.rejected") {
    const auto kind = act_field(photon, "kind");
    if (kind == "model.call") return std::nullopt;
    title = photon.kind == "act.rejected" ? "工具执行被拒绝" : "工具执行失败";
    if (!kind.empty()) title.append("：" + kind);
    detail = joined_detail({act_field(photon, "target"),
                            payload_text(photon.payload, "error")});
    item.tone = "danger";
  } else if (photon.kind == "tool.result") {
    title = "Agent 已获得工具结果";
    detail = joined_detail({payload_text(photon.payload, "tool"),
                            payload_text(photon.payload, "result")});
    item.tone = "success";
  } else if (photon.kind == "assistant.message") {
    return std::nullopt;
  } else if (photon.kind == "fs.read-completed" || photon.kind == "fs.read") {
    title = "Agent 已回读文件";
    detail = payload_text(photon.payload, "path");
    if (const auto content = payload_text(photon.payload, "content"); !content.empty())
      detail = joined_detail({detail, "内容：" + bounded_detail(content, 160)});
    item.tone = "success";
  } else if (photon.kind == "fs.changed" || photon.kind == "fs.written" ||
             photon.kind == "fs.created") {
    const auto operation = payload_text(photon.payload, "operation");
    title = operation == "create" || photon.kind == "fs.created"
        ? "Agent 已创建文件" : "Agent 已写入文件";
    detail = joined_detail({payload_text(photon.payload, "path"),
                            payload_text(photon.payload, "bytes").empty() ? std::string{} :
                                payload_text(photon.payload, "bytes") + " bytes",
                            tokmon::cbor::find(photon.payload, "write_verified") &&
                                tokmon::cbor::find(photon.payload, "write_verified")->as_bool()
                                    ? "已回读校验" : std::string{}});
    item.tone = "success";
  } else if (photon.kind == "fs.deleted") {
    title = "删除文件";
    detail = payload_text(photon.payload, "path");
    item.tone = "warning";
  } else if (photon.kind == "process.started") {
    title = "Agent 正在运行命令";
    detail = payload_text(photon.payload, "argv");
    item.tone = "warning";
  } else if (photon.kind == "process.stdout" || photon.kind == "process.stderr" ||
             photon.kind == "process.output") {
    title = "命令输出";
    detail = bounded_detail(payload_text(photon.payload, "text"));
    item.tone = photon.kind == "process.stderr" ? "warning" : "neutral";
  } else if (photon.kind == "process.exited" || photon.kind == "process.exit") {
    const auto code = payload_text(photon.payload, "exit_code");
    title = code == "0" || code.empty() ? "Agent 已完成命令验证" : "Agent 命令执行失败 (" + code + ")";
    detail = payload_text(photon.payload, "summary");
    item.tone = code == "0" || code.empty() ? "success" : "danger";
  } else if (photon.kind == "worker.progress") {
    title = "正在执行任务";
    detail = payload_text(photon.payload, "status");
    item.tone = "warning";
    if (const auto* progress = tokmon::cbor::find(photon.payload, "percent"))
      item.progress = static_cast<int>(progress->as_integer());
  } else if (photon.kind == "workflow.defined") {
    title = "透镜工作流已定义";
    detail = payload_text(photon.payload, "name");
  } else if (photon.kind == "workflow.step-dispatched") {
    title = "工作流步骤开始";
    detail = joined_detail({payload_text(photon.payload, "node_id"),
                            payload_text(photon.payload, "kind")});
    item.tone = "warning";
  } else if (photon.kind == "workflow.step-completed") {
    title = "工作流步骤完成";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "success";
  } else if (photon.kind == "workflow.step-failed" ||
             photon.kind == "workflow.compensation-failed") {
    title = photon.kind == "workflow.step-failed" ? "工作流步骤失败" : "补偿行动失败";
    detail = joined_detail({payload_text(photon.payload, "node_id"),
                            payload_text(photon.payload, "error")});
    item.tone = "danger";
  } else if (photon.kind == "workflow.step-skipped") {
    title = "工作流步骤已跳过";
    detail = payload_text(photon.payload, "node_id");
  } else if (photon.kind == "workflow.step-retry-requested") {
    title = "工作流步骤准备重试";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "warning";
  } else if (photon.kind == "workflow.compensation-dispatched") {
    title = "补偿行动开始";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "warning";
  } else if (photon.kind == "workflow.compensation-completed") {
    title = "补偿行动完成";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "success";
  } else if (photon.kind == "workflow.paused" || photon.kind == "workflow.resumed" ||
             photon.kind == "workflow.cancelled") {
    title = photon.kind == "workflow.paused" ? "工作流已暂停" :
            photon.kind == "workflow.resumed" ? "工作流已恢复" : "工作流已取消";
    detail = payload_text(photon.payload, "node_id");
    item.tone = photon.kind == "workflow.resumed" ? "success" :
                photon.kind == "workflow.cancelled" ? "danger" : "warning";
  } else {
    return std::nullopt;
  }

  item.title = display_string(title);
  item.detail = display_string(detail);
  return item;
}

std::vector<TimelineItem> conversation_workflow_from(
    const std::vector<tokmon::Photon>& photons) {
  std::uint64_t turn_start = 0;
  for (auto iterator = photons.rbegin(); iterator != photons.rend(); ++iterator)
    if (iterator->kind == "user.input" || iterator->kind == "user.message") {
      turn_start = iterator->sequence;
      break;
    }
  std::vector<TimelineItem> result;
  const tokmon::Photon* assistant = nullptr;
  const tokmon::Photon* failure = nullptr;
  const tokmon::Photon* latest_tool_result = nullptr;
  int tool_calls = 0;
  int verified_actions = 0;
  std::string reasoning_text;
  std::int64_t reasoning_time = 0;
  const auto flush_reasoning = [&] {
    if (reasoning_text.empty()) return;
    TimelineItem reasoning;
    reasoning.time = time_label(reasoning_time);
    reasoning.kind = "model.reasoning-summary";
    reasoning.title = "Agent 正在分析与规划";
    reasoning.detail = display_string(bounded_detail(std::move(reasoning_text)));
    reasoning.tone = "warning";
    reasoning.progress = -1;
    result.push_back(std::move(reasoning));
    reasoning_text.clear();
    reasoning_time = 0;
  };
  for (const auto& photon : photons) {
    if (photon.sequence < turn_start) continue;
    if (photon.kind == "model.reasoning-chunk") {
      if (reasoning_time == 0) reasoning_time = photon.committed_at_ms;
      reasoning_text.append(payload_text(photon.payload, "text"));
      continue;
    }
    flush_reasoning();
    if (photon.kind == "assistant.message") assistant = &photon;
    if (photon.kind == "model.tool-call") ++tool_calls;
    if (photon.kind == "tool.result" || photon.kind == "fs.changed" ||
        photon.kind == "fs.read-completed" || photon.kind == "process.exited") {
      latest_tool_result = &photon;
      ++verified_actions;
    }
    if (photon.kind == "model.failed" || photon.kind == "act.failed") failure = &photon;
    if (auto item = conversation_workflow_item(photon)) result.push_back(std::move(*item));
  }
  flush_reasoning();
  const bool verified_complete = assistant && tool_calls > 0 && latest_tool_result &&
      assistant->sequence > latest_tool_result->sequence;
  if (verified_complete) {
    TimelineItem done;
    done.time = time_label(assistant->committed_at_ms);
    done.kind = "task.completed";
    done.title = "任务已完成";
    done.detail = display_string("已完成 " + std::to_string(verified_actions) +
        " 个可验证行动并给出最终结果；完整证据请在「轨迹」页查看");
    done.tone = "success";
    done.progress = -1;
    result.push_back(std::move(done));
  } else if (assistant && tool_calls == 0) {
    TimelineItem reply;
    reply.time = time_label(assistant->committed_at_ms);
    reply.kind = "agent.reply-only";
    reply.title = "Agent 已给出回复，但未执行工具";
    reply.detail = "未检测到可验证的文件、命令或计算行动；本回合不标记为任务完成";
    reply.tone = "warning";
    reply.progress = -1;
    result.push_back(std::move(reply));
  } else if (failure) {
    TimelineItem failed;
    failed.time = time_label(failure->committed_at_ms);
    failed.kind = "task.failed";
    failed.title = "任务执行失败";
    failed.detail = "已完成既定重试仍未成功；完整错误与重试轨迹请在「轨迹」页查看";
    failed.tone = "danger";
    failed.progress = -1;
    result.push_back(std::move(failed));
  }
  return result;
}

struct TraceSummary final {
  std::string duration{"0ms"};
  std::string turn_duration{"0ms"};
  int turns{0};
  int calls{0};
  std::int64_t input_tokens{0};
  std::int64_t output_tokens{0};
  std::string provider{"-"};
  std::string model{"-"};
  std::string result{"等待输入"};
};

std::string duration_label(const std::int64_t milliseconds) {
  if (milliseconds < 1'000) return std::to_string(std::max<std::int64_t>(0, milliseconds)) + "ms";
  const auto seconds = milliseconds / 1'000;
  if (seconds < 60) return std::to_string(seconds) + "." +
      std::to_string((milliseconds % 1'000) / 100) + "s";
  const auto minutes = seconds / 60;
  return std::to_string(minutes) + "m " + std::to_string(seconds % 60) + "s";
}

TraceSummary trace_summary_from(const std::vector<tokmon::Photon>& photons) {
  TraceSummary summary;
  if (!photons.empty())
    summary.duration = duration_label(std::max<std::int64_t>(0,
        photons.back().committed_at_ms - photons.front().committed_at_ms));
  std::int64_t turn_start_ms = 0;
  std::uint64_t turn_start_sequence = 0;
  std::uint64_t latest_call = 0;
  std::uint64_t latest_result = 0;
  std::uint64_t latest_assistant = 0;
  std::uint64_t latest_failure = 0;
  for (const auto& photon : photons) {
    if (photon.kind == "user.input" || photon.kind == "user.message") {
      ++summary.turns;
      turn_start_ms = photon.committed_at_ms;
      turn_start_sequence = photon.sequence;
      latest_call = latest_result = latest_assistant = latest_failure = 0;
    }
    if (photon.kind == "model.dispatched") ++summary.calls;
    if (photon.sequence >= turn_start_sequence && photon.kind == "model.tool-call")
      latest_call = photon.sequence;
    if (photon.sequence >= turn_start_sequence &&
        (photon.kind == "tool.result" || photon.kind == "fs.changed" ||
         photon.kind == "fs.read-completed" || photon.kind == "process.exited"))
      latest_result = photon.sequence;
    if (photon.sequence >= turn_start_sequence && photon.kind == "assistant.message")
      latest_assistant = photon.sequence;
    if (photon.sequence >= turn_start_sequence &&
        (photon.kind == "model.failed" || photon.kind == "act.failed" ||
         photon.kind == "act.rejected"))
      latest_failure = photon.sequence;
    if (photon.kind == "model.usage") {
      if (const auto* value = tokmon::cbor::find(photon.payload, "input_tokens"))
        summary.input_tokens += value->as_integer();
      if (const auto* value = tokmon::cbor::find(photon.payload, "output_tokens"))
        summary.output_tokens += value->as_integer();
    }
    if (photon.kind.starts_with("model.") || photon.kind == "assistant.message") {
      const auto provider = payload_text(photon.payload, "provider");
      const auto model = payload_text(photon.payload, "model");
      if (!provider.empty()) summary.provider = provider;
      if (!model.empty()) summary.model = model;
    }
  }
  if (latest_failure > std::max(latest_assistant, latest_result))
    summary.result = "执行失败";
  else if (latest_call > 0 && latest_result > latest_call &&
           latest_assistant > latest_result)
    summary.result = "已完成";
  else if (latest_assistant > 0)
    summary.result = latest_call == 0 ? "已回复（未执行工具）" : "等待工具结果";
  else if (turn_start_sequence > 0)
    summary.result = "执行中";
  if (turn_start_ms > 0 && !photons.empty())
    summary.turn_duration = duration_label(std::max<std::int64_t>(0,
        photons.back().committed_at_ms - turn_start_ms));
  return summary;
}

std::vector<CodeLine> code_lines_from(const std::vector<tokmon::Photon>& photons) {
  std::string content;
  for (auto iterator = photons.rbegin(); iterator != photons.rend(); ++iterator) {
    if (iterator->kind != "fs.read" && iterator->kind != "fs.written" &&
        iterator->kind != "fs.created" && iterator->kind != "artifact.previewed")
      continue;
    const auto* field = tokmon::cbor::find(iterator->payload, "content");
    if (!field) field = tokmon::cbor::find(iterator->payload, "text");
    if (field && std::holds_alternative<std::string>(field->data)) {
      content = std::string(field->as_string());
      break;
    }
  }
  if (content.empty())
    content = "// 当前会话尚无文件变更。\n"
              "// 真实工具创建或修改文件后，内容会投影到此处。";
  std::vector<CodeLine> result;
  std::istringstream input(content);
  std::string text;
  for (std::size_t index = 0; std::getline(input, text) && index < 20'000u; ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u);
    line.text = display_string(text);
    const auto first = text.find_first_not_of(" \t");
    line.tone = first != std::string::npos && text[first] == '#' ? "comment" : "normal";
    result.push_back(std::move(line));
  }
  return result;
}

void refresh_navigation(
    const std::shared_ptr<slint::VectorModel<NavigationItem>>& model,
    const std::shared_ptr<std::vector<NavigationItem>>& items,
    std::string query,
    const slint::ComponentWeakHandle<MainWindow>& window = {}) {
  for (auto& character : query)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

  const auto matches = [&query](const NavigationItem& item) {
    auto title = std::string(item.title);
    for (auto& character : title)
      character = static_cast<char>(
          std::tolower(static_cast<unsigned char>(character)));
    return title.find(query) != std::string::npos;
  };

  int visible_sessions = 0;
  model->clear();
  for (std::size_t row = 0; row < items->size(); ++row) {
    bool shown = true;
    if (query.empty()) {
      auto required_indent = (*items)[row].indent - 1;
      for (std::size_t previous = row; previous > 0 && required_indent >= 0;) {
        --previous;
        if ((*items)[previous].indent != required_indent) continue;
        if (!(*items)[previous].expanded) shown = false;
        --required_indent;
      }
    } else {
      shown = matches((*items)[row]);
      for (std::size_t child = row + 1;
           !shown && child < items->size() &&
           (*items)[child].indent > (*items)[row].indent;
           ++child)
        shown = matches((*items)[child]);
    }
    if (shown) {
      model->push_back((*items)[row]);
      if ((*items)[row].kind == "session") ++visible_sessions;
    }
  }
  if (window.lock()) {
    auto weak = window;
    const auto count = query.empty() ? 0 : visible_sessions;
    (void)slint::invoke_from_event_loop([weak, count] {
      if (auto locked = weak.lock()) (*locked)->set_search_session_count(count);
    });
  }
}

std::string path_basename_utf8(const std::string_view value) {
  std::string clean(value);
  while (!clean.empty() && (clean.back() == '/' || clean.back() == '\\'))
    clean.pop_back();
  const auto pos = clean.find_last_of("/\\");
  return pos == std::string::npos ? clean : clean.substr(pos + 1);
}

std::string short_workspace_label(const std::filesystem::path& workspace,
                                  const std::filesystem::path& root_workspace) {
  auto text = path_to_utf8(workspace);
  auto root_text = path_to_utf8(root_workspace);
  if (!root_text.empty()) {
    auto prefix = root_text;
    while (!prefix.empty() && (prefix.back() == '/' || prefix.back() == '\\'))
      prefix.pop_back();
    if (text.size() > prefix.size() && text.starts_with(prefix)) {
      auto rest = text.substr(prefix.size());
      while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\'))
        rest.erase(rest.begin());
      if (rest.empty()) return "~/";
      return "~/" + rest;
    }
  }
  return text;
}

std::string git_branch_label(const std::filesystem::path& workspace) {
  std::error_code error;
  const auto head = workspace / ".git" / "HEAD";
  if (!std::filesystem::is_regular_file(head, error)) return {};
  std::ifstream stream(head, std::ios::binary);
  if (!stream) return {};
  std::string line;
  std::getline(stream, line);
  const auto prefix = std::string_view("ref: refs/heads/");
  if (line.starts_with(prefix)) return std::string(line.substr(prefix.size()));
  if (line.size() >= 12) return line.substr(0, 12);
  return {};
}

int count_indexed_files(const std::filesystem::path& workspace) {
  int files = 0;
  std::error_code error;
  std::function<void(const std::filesystem::path&, int)> scan =
      [&](const std::filesystem::path& directory, int depth) {
        if (depth > 3 || files > 4'096) return;
        for (std::filesystem::directory_iterator it(directory, error), end;
             it != end && !error; it.increment(error)) {
          const auto& entry = *it;
          std::error_code entry_error;
          if (entry.is_directory(entry_error)) {
            const auto name = entry.path().filename().string();
            if (name == ".git" || name == "node_modules" ||
                name == ".tokmon" || name == "build")
              continue;
            scan(entry.path(), depth + 1);
          } else if (entry.is_regular_file(entry_error)) {
            ++files;
            if (files > 4'096) return;
          }
        }
      };
  scan(workspace, 1);
  return files;
}

struct SessionFile final {
  std::string name;
  std::string path;
  std::string content;
  bool written{false};
};

SessionFile session_file_from_photon(const tokmon::Photon& photon) {
  SessionFile file;
  const auto* field = tokmon::cbor::find(photon.payload, "path");
  file.path = field ? std::string(field->as_string()) : std::string{};
  file.name = path_basename_utf8(file.path);
  if (const auto* content = tokmon::cbor::find(photon.payload, "content");
      content && std::holds_alternative<std::string>(content->data))
    file.content = std::string(content->as_string());
  else if (const auto* text = tokmon::cbor::find(photon.payload, "text");
           text && std::holds_alternative<std::string>(text->data) &&
               (photon.kind == "fs.written" || photon.kind == "fs.created" ||
                photon.kind == "artifact.previewed"))
    file.content = std::string(text->as_string());
  file.written = photon.kind == "fs.written" || photon.kind == "fs.created";
  return file;
}

std::vector<CodeLine> code_lines_from_text(const std::string& content) {
  std::vector<CodeLine> result;
  std::istringstream input(content);
  std::string text;
  for (std::size_t index = 0; std::getline(input, text) && index < 20'000u; ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u);
    line.text = display_string(text);
    const auto first = text.find_first_not_of(" \t");
    line.tone = first != std::string::npos && text[first] == '#' ? "comment" : "normal";
    result.push_back(std::move(line));
  }
  if (result.empty()) {
    CodeLine single;
    single.number = 1;
    single.text = display_string(content.empty() ? "// 当前会话尚无文件投影。" : content);
    single.tone = "normal";
    result.push_back(std::move(single));
  }
  return result;
}

NavigationItem make_navigation_item(const std::filesystem::path& assets,
                                    std::string id, std::string kind,
                                    std::string title, const int indent,
                                    const bool selected, const bool expanded = true,
                                    std::string ray = {}, std::string workspace = {}) {
  NavigationItem item;
  item.id = display_string(id);
  item.ray = display_string(ray);
  item.workspace = display_string(workspace);
  item.kind = display_string(kind);
  item.title = display_string(title);
  item.indent = indent;
  item.selected = selected;
  item.expandable = kind != "session";
  item.expanded = expanded;
  const auto icon = kind == "group" ? "icon-06.svg" :
                    kind == "project" ? "icon-08.svg" : "icon-09.svg";
  item.icon = slint::Image::load_from_path(
      slint::SharedString((assets / icon).string()));
  return item;
}

tokmon::cbor::Value navigation_value(const std::vector<NavigationItem>& items) {
  tokmon::cbor::Value::Array encoded;
  encoded.reserve(items.size());
  for (const auto& item : items)
    encoded.push_back(tokmon::cbor::object({
        {"id", std::string(item.id)}, {"ray", std::string(item.ray)},
        {"workspace", std::string(item.workspace)},
        {"kind", std::string(item.kind)},
        {"title", std::string(item.title)},
        {"indent", static_cast<std::int64_t>(item.indent)},
        {"selected", item.selected}, {"expanded", item.expanded}}));
  return encoded;
}

std::optional<std::vector<NavigationItem>> navigation_items(
    const tokmon::cbor::Value& value, const std::filesystem::path& assets,
    const std::filesystem::path& default_workspace) {
  if (!value.as_array()) return std::nullopt;
  std::vector<NavigationItem> items;
  items.reserve(value.as_array()->size());
  for (const auto& encoded : *value.as_array()) {
    const auto* id = tokmon::cbor::find(encoded, "id");
    const auto* kind = tokmon::cbor::find(encoded, "kind");
    const auto* title = tokmon::cbor::find(encoded, "title");
    const auto kind_text = kind ? std::string(kind->as_string()) : std::string{};
    if (!id || !title || (kind_text != "group" && kind_text != "project" &&
                          kind_text != "session"))
      return std::nullopt;
    const auto indent = tokmon::cbor::find(encoded, "indent")
        ? static_cast<int>(tokmon::cbor::find(encoded, "indent")->as_integer()) : 0;
    if (indent < 0 || indent > 8 || title->as_string().empty() ||
        title->as_string().size() > 256)
      return std::nullopt;
    std::string workspace;
    if (const auto* encoded_workspace = tokmon::cbor::find(encoded, "workspace")) {
      if (!std::holds_alternative<std::string>(encoded_workspace->data) ||
          encoded_workspace->as_string().size() > 4'096u ||
          encoded_workspace->as_string().find('\0') != std::string_view::npos)
        return std::nullopt;
      workspace = std::string(encoded_workspace->as_string());
    }
    if (kind_text == "group") {
      workspace.clear();
    } else if (kind_text == "project" && workspace.empty()) {
      workspace = path_to_utf8(default_workspace);
    }
    if (!workspace.empty()) {
      auto normalized = normalize_workspace_path(workspace, default_workspace);
      if (!normalized) return std::nullopt;
      workspace = path_to_utf8(*normalized);
    }
    items.push_back(make_navigation_item(assets, std::string(id->as_string()), kind_text,
        std::string(title->as_string()), indent,
        tokmon::cbor::find(encoded, "selected") &&
            tokmon::cbor::find(encoded, "selected")->as_bool(),
        !tokmon::cbor::find(encoded, "expanded") ||
            tokmon::cbor::find(encoded, "expanded")->as_bool(),
        tokmon::cbor::find(encoded, "ray")
            ? std::string(tokmon::cbor::find(encoded, "ray")->as_string())
            : std::string{}, std::move(workspace)));
  }
  return items;
}

std::filesystem::path navigation_workspace_at(
    const std::vector<NavigationItem>& items, const std::size_t index,
    const std::filesystem::path& fallback) {
  if (index >= items.size()) return fallback;
  if (!std::string(items[index].workspace).empty()) {
    if (auto normalized = normalize_workspace_path(
            std::string(items[index].workspace), fallback))
      return *normalized;
  }
  for (std::size_t previous = index; previous > 0;) {
    --previous;
    if (items[previous].indent >= items[index].indent) continue;
    if (items[previous].kind == "project") {
      if (auto normalized = normalize_workspace_path(
              std::string(items[previous].workspace), fallback))
        return *normalized;
    }
    break;
  }
  return fallback;
}

std::size_t navigation_ancestor_at(const std::vector<NavigationItem>& items,
                                   const std::size_t index,
                                   const std::string_view kind) {
  if (index >= items.size()) return items.size();
  if (std::string(items[index].kind) == kind) return index;
  auto ancestor_indent = items[index].indent;
  for (std::size_t previous = index; previous > 0;) {
    --previous;
    if (items[previous].indent >= ancestor_indent) continue;
    ancestor_indent = items[previous].indent;
    if (std::string(items[previous].kind) == kind) return previous;
  }
  return items.size();
}

class UiSnowController final {
 public:
  UiSnowController(std::filesystem::path endpoint,
                   std::filesystem::path workspace,
                   std::filesystem::path daemon_executable,
                   std::shared_ptr<slint::VectorModel<TimelineItem>> timeline,
                   std::shared_ptr<slint::VectorModel<TimelineItem>> conversation_workflow,
                   std::shared_ptr<slint::VectorModel<CodeLine>> code,
                   std::shared_ptr<slint::VectorModel<TraceEvent>> trace_events,
                   std::shared_ptr<slint::VectorModel<GanttSegment>> gantt,
                   std::shared_ptr<slint::VectorModel<NavigationItem>> navigation_model,
                   std::shared_ptr<std::vector<NavigationItem>> navigation,
                   std::filesystem::path assets,
                   slint::ComponentWeakHandle<MainWindow> window,
                   const bool restore_initial_workspace)
      : endpoint_(endpoint), navigation_endpoint_(std::move(endpoint)),
        current_workspace_(workspace), navigation_workspace_(std::move(workspace)),
        daemon_executable_(std::move(daemon_executable)), timeline_(std::move(timeline)),
        conversation_workflow_(std::move(conversation_workflow)), code_(std::move(code)),
          trace_events_(std::move(trace_events)),
          gantt_(std::move(gantt)),
        navigation_model_(std::move(navigation_model)),
        navigation_(std::move(navigation)), assets_(std::move(assets)),
        window_(std::move(window)),
        restore_initial_workspace_(restore_initial_workspace),
        worker_([this](std::stop_token stop) { run(stop); }) {}

  ~UiSnowController() {
    worker_.request_stop();
    condition_.notify_all();
  }

  void chat(std::string text, std::string provider, std::string model, std::string access_mode,
            std::string effort) {
    Command command{"chat", std::move(text)};
    command.payload = tokmon::cbor::object({{"provider", std::move(provider)},
        {"model", std::move(model)},
        {"access_mode", std::move(access_mode)}, {"effort", std::move(effort)}});
    enqueue_user(std::move(command));
  }
  void slash_command(std::string text, std::string provider, std::string model,
                     std::string access_mode, std::string effort) {
    Command command{"slash-command", std::move(text)};
    command.payload = tokmon::cbor::object({{"provider", std::move(provider)},
        {"model", std::move(model)}, {"access_mode", std::move(access_mode)},
        {"effort", std::move(effort)}, {"surface", "desktop"}});
    enqueue_user(std::move(command));
  }
  void snapshot() { enqueue(Command{"snapshot", {}}); }
  void reconcile() { enqueue(Command{"reconcile", {}}); }
  void new_session(std::string workspace = {}) {
    Command command{"new-session", {}};
    command.payload = tokmon::cbor::object({{"workspace", std::move(workspace)}});
    enqueue(std::move(command));
  }
  void open_session(std::string ray, std::string workspace = {}) {
    Command command{"open-session", std::move(ray)};
    command.payload = tokmon::cbor::object({{"workspace", std::move(workspace)}});
    enqueue(std::move(command));
  }
  void switch_workspace(std::string workspace) {
    enqueue(Command{"switch-workspace", std::move(workspace)});
  }
  void load_settings(const bool include_navigation = false) {
    Command command{"settings-load", {}};
    command.payload = tokmon::cbor::object({{"include_navigation", include_navigation}});
    enqueue(std::move(command));
  }
  void load_providers() { enqueue(Command{"providers-load", {}}); }
  void save_navigation() {
    Command command{"navigation-save", {}};
    command.payload = navigation_value(*navigation_);
    enqueue(std::move(command));
  }
  void save_settings(tokmon::cbor::Value values) {
    Command command{"settings-save", {}};
    command.payload = std::move(values);
    enqueue(std::move(command));
  }
  void configure_provider(tokmon::cbor::Value values) {
    Command command{"provider-configure", {}};
    command.payload = std::move(values);
    enqueue(std::move(command));
  }
  void select_provider(std::string id) {
    Command command{"provider-use", {}};
    command.payload = tokmon::cbor::object({{"id", std::move(id)}});
    enqueue(std::move(command));
  }
  void store_provider_secret(std::string id, std::string secret) {
    Command command{"provider-secret", {}};
    command.payload = tokmon::cbor::object(
        {{"id", std::move(id)}, {"secret", std::move(secret)}});
    enqueue(std::move(command));
  }
  void test_provider(std::string id) {
    Command command{"provider-test", {}};
    command.payload = tokmon::cbor::object({{"provider", std::move(id)}});
    enqueue(std::move(command));
  }

  void publish_trace_view() {
    auto window = window_;
    auto trace_events = trace_events_;
    auto gantt = gantt_;
    const auto photons = photons_;
    (void)slint::invoke_from_event_loop(
        [window, trace_events, gantt, photons]() mutable {
          auto locked = window.lock();
          if (!locked) return;
          auto handle = *locked;
          const auto search = display_utf8(std::string_view(handle->get_trace_search()));
          const auto filter_index = handle->get_trace_filter_index();
          const auto page = std::max(1, handle->get_trace_page());
          const auto page_size = std::max(1, handle->get_trace_page_size());

          struct Entry { TraceEvent event; std::int64_t time; std::string kind_l; };
          std::vector<Entry> all;
          all.reserve(photons.size());
          int num = 0;
          for (const auto& photon : photons) {
            ++num;
            TraceEvent ev;
            ev.num = num;
            ev.time = time_label(photon.committed_at_ms);
            const auto kind = std::string(photon.kind);
            const auto act_kind = kind.starts_with("act.")
                ? act_field(photon, "kind") : std::string{};
            const auto tool_event = kind == "model.tool-call" ||
                kind == "tool.result" || kind.starts_with("fs.") ||
                kind.starts_with("process.") ||
                (kind.starts_with("act.") && act_kind != "model.call");
            const auto model_event = kind.starts_with("model.") ||
                (kind.starts_with("act.") && act_kind == "model.call");
            ev.tone = display_string(kind == "user.input" || kind == "user.message" ? "USER"
                : kind.find("context") != std::string::npos || kind == "system.prompt" ? "CONTEXT"
                : kind == "assistant.message" || kind == "model.completed" ? "ASSISTANT"
                : kind.find("failed") != std::string::npos || kind.find("rejected") != std::string::npos ? "ERROR"
                : tool_event ? "TOOL"
                : model_event ? "MODEL" : "LENS");
            ev.role = display_string(std::string(ev.tone) == "USER" ? "User"
                : std::string(ev.tone) == "CONTEXT" ? "System"
                : std::string(ev.tone) == "ASSISTANT" ? "Assistant"
                : std::string(ev.tone) == "MODEL" ? "Model"
                : std::string(ev.tone) == "TOOL" ? "Tool"
                : std::string(ev.tone) == "LENS" ? "Lens" : "-");
            ev.title = display_string(photon.kind);
            ev.detail = display_string(bounded_detail(
                kind + " · " + tokmon::cbor::diagnostic(photon.payload), 120));
            if (const auto* dur = tokmon::cbor::find(photon.payload, "duration_ms"))
              ev.dur = slint::SharedString(std::to_string(dur->as_integer()) + "ms");
            else
              ev.dur = "-";
            if (photon.kind == "model.usage") {
              std::int64_t total = 0;
              if (const auto* v = tokmon::cbor::find(photon.payload, "input_tokens")) total += v->as_integer();
              if (const auto* v = tokmon::cbor::find(photon.payload, "output_tokens")) total += v->as_integer();
              ev.tokens = slint::SharedString(std::to_string(total));
            } else {
              ev.tokens = "-";
            }
            std::string hay = kind + std::string(ev.detail);
            for (auto& ch : hay) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool matches_search = search.empty() ||
                hay.find(search) != std::string::npos;
            const auto tone_str = std::string(ev.tone);
            bool matches_filter =
                filter_index == 0 ||
                (filter_index == 1 && tone_str == "USER") ||
                (filter_index == 2 && tone_str == "CONTEXT") ||
                (filter_index == 3 && tone_str == "ASSISTANT") ||
                (filter_index == 4 && tone_str == "TOOL");
            if (matches_search && matches_filter)
              all.push_back({std::move(ev), photon.committed_at_ms, kind});
          }
          const int total_count = static_cast<int>(all.size());
          const int pages = std::max(1, (total_count + page_size - 1) / page_size);
          const int clamped_page = std::min(page, pages);
          handle->set_trace_total(total_count);
          handle->set_trace_pages(pages);
          handle->set_trace_page(clamped_page);
          const int begin = (clamped_page - 1) * page_size;
          const int end = std::min(total_count, begin + page_size);
          trace_events->clear();
          std::vector<TraceEvent> page_events;
          for (int i = begin; i < end; ++i) {
            page_events.push_back(all[i].event);
          }
          for (auto& e : page_events) trace_events->push_back(std::move(e));

          // Gantt segments from real timestamps
          gantt->clear();
          if (!photons.empty()) {
            const auto t0 = photons.front().committed_at_ms;
            const auto t1 = std::max(t0 + 1, photons.back().committed_at_ms);
            const double span = static_cast<double>(t1 - t0);
            constexpr double min_frac = 0.02;
            int count = 0;
            for (const auto& photon : photons) {
              if (++count > 200) break;
              const double start = std::clamp(
                  static_cast<double>(photon.committed_at_ms - t0) / span, 0.0, 0.98);
              const auto kind = std::string(photon.kind);
              const auto act_kind = kind.starts_with("act.")
                  ? act_field(photon, "kind") : std::string{};
              int row = 0;
              slint::Color tint = slint::Color::from_rgb_uint8(0x6B,0x72,0x80);
              if (kind == "user.input" || kind == "user.message") { row = 0; tint = slint::Color::from_rgb_uint8(0x6B,0x72,0x80); }
              else if (kind.find("context") != std::string::npos || kind == "system.prompt") { row = 0; tint = slint::Color::from_rgb_uint8(0x3B,0x82,0xF6); }
              else if (kind == "assistant.message" || kind == "model.completed") { row = 0; tint = slint::Color::from_rgb_uint8(0x22,0xC5,0x5E); }
              else if (kind == "tool.result" || kind.starts_with("fs.") ||
                       kind.starts_with("process.") ||
                       (kind.starts_with("act.") && act_kind != "model.call")) {
                row = 2; tint = slint::Color::from_rgb_uint8(0xF9,0x73,0x16);
              }
              else if (kind.starts_with("model.")) { row = 1; tint = slint::Color::from_rgb_uint8(0xA8,0x55,0xF7); }
              else if (kind.starts_with("act.") && act_kind == "model.call") {
                row = 1; tint = slint::Color::from_rgb_uint8(0xA8,0x55,0xF7);
              }
              GanttSegment seg;
              seg.row = row;
              seg.start = static_cast<float>(start);
              seg.span = static_cast<float>(std::max(min_frac, 1.0 / std::max(1.0, span / 1000)));
              seg.span = static_cast<float>(std::min(0.98 - start, static_cast<double>(seg.span)));
              seg.tint = tint;
              gantt->push_back(seg);
            }
          }

          // Time ticks
          if (!photons.empty()) {
            const auto total_s = (photons.back().committed_at_ms - photons.front().committed_at_ms) / 1000;
            std::vector<slint::SharedString> ticks;
            for (int i = 0; i < 7; ++i) {
              const auto sec = total_s * i / 6;
              if (sec < 60) ticks.push_back(slint::SharedString(std::to_string(sec) + "s"));
              else ticks.push_back(slint::SharedString(std::to_string(sec / 60) + "m " + std::to_string(sec % 60) + "s"));
            }
            auto tick_model = std::make_shared<slint::VectorModel<slint::SharedString>>(ticks);
            handle->set_trace_ticks(tick_model);
          }

          // Token labels with thousands separators
          auto group_digits = [](std::int64_t value) {
            auto str = std::to_string(value);
            std::string out;
            int pos = 0;
            for (auto it = str.rbegin(); it != str.rend(); ++it) {
              if (pos > 0 && pos % 3 == 0) out += ',';
              out += *it; ++pos;
            }
            return std::string(out.rbegin(), out.rend());
          };
          std::int64_t in_toks = 0;
          std::int64_t out_toks = 0;
          for (const auto& photon : photons) {
            if (photon.kind != "model.usage") continue;
            if (const auto* value = tokmon::cbor::find(photon.payload, "input_tokens"))
              in_toks += value->as_integer();
            if (const auto* value = tokmon::cbor::find(photon.payload, "output_tokens"))
              out_toks += value->as_integer();
          }
          const auto tot = in_toks + out_toks;
          handle->set_trace_total_label(display_string(group_digits(tot)));
          if (tot > 0) {
            handle->set_trace_prompt_label(display_string(
                group_digits(in_toks) + " (" + std::to_string(in_toks * 100 / tot) + "%)"));
            handle->set_trace_completion_label(display_string(
                group_digits(out_toks) + " (" + std::to_string(out_toks * 100 / tot) + "%)"));
          }

          // Workflow counters
          int explored = 0, ran = 0;
          for (const auto& p : photons) {
            if (p.kind == "fs.read-completed" || p.kind == "fs.changed" ||
                p.kind == "artifact.previewed") ++explored;
            if (p.kind == "process.exited" || p.kind == "tool.result") ++ran;
          }
          handle->set_workflow_explored(explored);
          handle->set_workflow_ran(ran);
        });
  }

  void export_trace() {
    auto window = window_;
    const auto photons = photons_;
    const auto workspace = current_workspace_;
    (void)slint::invoke_from_event_loop([window, photons, workspace] {
      auto locked = window.lock();
      if (!locked) return;
      auto handle = *locked;
      const auto search = display_utf8(std::string_view(handle->get_trace_search()));
      const auto filter_index = handle->get_trace_filter_index();
      try {
        auto dir = workspace / "exports";
        std::filesystem::create_directories(dir);
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto file = dir / ("tokmon-trace-" + std::to_string(ms) + ".json");
        std::ofstream out(file, std::ios::binary);
        if (!out) { handle->set_settings_status("导出失败：无法写入文件"); return; }
        out << "{\"events\":[";
        int num = 0; bool first = true;
        for (const auto& photon : photons) {
          ++num;
          const auto kind = std::string(photon.kind);
          std::string tone = kind == "user.input" || kind == "user.message" ? "USER"
              : kind.find("context") != std::string::npos ? "CONTEXT"
              : kind == "assistant.message" || kind == "model.completed" ? "ASSISTANT"
              : kind.find("failed") != std::string::npos ? "ERROR"
              : kind == "model.tool-call" || kind.starts_with("fs.") || kind.starts_with("process.") || kind == "tool.result" ? "TOOL"
              : "OTHER";
          bool match = filter_index == 0 ||
              (filter_index == 1 && tone == "USER") || (filter_index == 2 && tone == "CONTEXT") ||
              (filter_index == 3 && tone == "ASSISTANT") || (filter_index == 4 && tone == "TOOL");
          if (!match) continue;
          auto detail = tokmon::cbor::diagnostic(photon.payload);
          for (auto& ch : detail) if (ch == '"' || ch == '\\') ch = ' ';
          if (!first) out << ",";
          first = false;
          out << "\n{\"num\":" << num << ",\"kind\":\"" << kind << "\",\"tone\":\"" << tone
              << "\",\"time\":" << photon.committed_at_ms << ",\"detail\":\"" << detail << "\"}";
        }
        out << "\n]}\n";
        out.close();
        handle->set_settings_status(slint::SharedString(
            "轨迹已导出: " + std::filesystem::absolute(file).string()));
      } catch (...) {
        handle->set_settings_status("轨迹导出失败");
      }
    });
  }

  [[nodiscard]] const std::filesystem::path& current_workspace() const noexcept {
    return current_workspace_;
  }

  void select_session_file(const int index) {
    std::string name;
    std::string content;
    int added_lines = 0;
    std::vector<CodeLine> preview;
    {
      std::scoped_lock lock(files_mutex_);
      if (index < 0 || index >= static_cast<int>(session_files_.size())) return;
      const auto& chosen = session_files_[static_cast<std::size_t>(index)];
      name = chosen.name;
      content = chosen.content;
      added_lines = chosen.written
          ? static_cast<int>(std::count(chosen.content.begin(),
                                        chosen.content.end(), '\n')) + 1
          : 0;
      full_preview_lines_ = code_lines_from_text(content);
      preview = full_preview_lines_;
    }
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [window, preview = std::move(preview), name, content, added_lines]() mutable {
          auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
          for (auto& line : preview) preview_model->push_back(std::move(line));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_selected_file_name(display_string(name));
            handle->set_preview_content(display_string(content));
            handle->set_file_added_lines(added_lines);
            handle->set_preview_lines(preview_model);
            handle->set_preview_search(slint::SharedString{});
          }
        });
  }

  void filter_preview_lines(std::string query) {
    for (auto& character : query)
      character = static_cast<char>(
          std::tolower(static_cast<unsigned char>(character)));
    std::vector<CodeLine> preview;
    {
      std::scoped_lock lock(files_mutex_);
      for (const auto& line : full_preview_lines_) {
        if (query.empty()) {
          preview.push_back(line);
          continue;
        }
        auto text = std::string(line.text);
        for (auto& character : text)
          character = static_cast<char>(
              std::tolower(static_cast<unsigned char>(character)));
        if (text.find(query) != std::string::npos) preview.push_back(line);
      }
    }
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [window, preview = std::move(preview)]() mutable {
          auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
          for (auto& line : preview) preview_model->push_back(std::move(line));
          if (auto locked = window.lock())
            (*locked)->set_preview_lines(preview_model);
        });
  }

  void notify_copied() {
    auto window = window_;
    (void)slint::invoke_from_event_loop([window] {
      if (auto locked = window.lock())
        (*locked)->set_status_text("内容已复制到剪贴板");
    });
  }

  void publish_workspace_info() {
    auto window = window_;
    const auto workspace = current_workspace_;
    const auto root = navigation_workspace_;
    const auto project = path_basename_utf8(path_to_utf8(workspace));
    const auto short_label = short_workspace_label(workspace, root);
    const auto branch = git_branch_label(workspace);
    const auto indexed = count_indexed_files(workspace);
    std::vector<slint::SharedString> groups;
    std::vector<ProjectFile> presets;
    for (std::size_t index = 0; index < navigation_->size(); ++index) {
      const auto& item = (*navigation_)[index];
      if (std::string(item.kind) == "group") {
        bool has_projects = false;
        for (auto next = index + 1;
             next < navigation_->size() && (*navigation_)[next].indent > item.indent;
             ++next)
          if (std::string((*navigation_)[next].kind) == "project") has_projects = true;
        if (has_projects) groups.push_back(item.title);
      }
      if (std::string(item.kind) == "project" &&
          !std::string(item.workspace).empty()) {
        ProjectFile preset;
        preset.name = item.title;
        preset.path = display_string(std::string(item.workspace));
        for (auto previous = index; previous > 0;) {
          --previous;
          if ((*navigation_)[previous].indent >= item.indent) continue;
          if (std::string((*navigation_)[previous].kind) == "group")
            preset.group_title = (*navigation_)[previous].title;
          break;
        }
        presets.push_back(std::move(preset));
      }
    }
    (void)slint::invoke_from_event_loop(
        [window, project, short_label, branch, indexed,
         groups = std::move(groups), presets = std::move(presets)]() mutable {
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_workspace_project(display_string(project));
            handle->set_workspace_short_path(display_string(short_label));
            handle->set_workspace_branch(display_string(branch));
            handle->set_workspace_has_git(!branch.empty());
            handle->set_workspace_indexed_files(indexed);
            auto group_model =
                std::make_shared<slint::VectorModel<slint::SharedString>>();
            for (auto& group : groups) group_model->push_back(std::move(group));
            handle->set_group_options(group_model);
            auto preset_model = std::make_shared<slint::VectorModel<ProjectFile>>();
            for (auto& preset : presets) preset_model->push_back(std::move(preset));
            handle->set_workspace_presets(preset_model);
          }
        });
  }
 private:
  struct Command {
    std::string kind;
    std::string text;
    tokmon::cbor::Value payload;
  };

  void enqueue(Command command) {
    {
      std::scoped_lock lock(mutex_);
      commands_.push_back(std::move(command));
    }
    condition_.notify_one();
  }

  void enqueue_user(Command command) {
    {
      std::scoped_lock lock(mutex_);
      if (initializing_) {
        deferred_user_commands_.push_back(std::move(command));
        return;
      }
      commands_.push_back(std::move(command));
    }
    condition_.notify_one();
  }

  void finish_initialization() {
    {
      std::scoped_lock lock(mutex_);
      if (!initializing_) return;
      initializing_ = false;
      while (!deferred_user_commands_.empty()) {
        commands_.push_back(std::move(deferred_user_commands_.front()));
        deferred_user_commands_.pop_front();
      }
    }
    condition_.notify_one();
  }

  void publish_pending(const std::string& text) {
    TimelineItem item;
    item.time = time_label(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count());
    item.kind = "task";
    item.title = display_string("已提交后续请求: " + text);
    item.detail = "";
    item.tone = "warning";
    item.progress = -1;
    auto timeline = timeline_;
    auto workflow = conversation_workflow_;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [timeline, workflow, window, item = std::move(item),
         message = display_string(text)]() mutable {
          workflow->clear();
          timeline->push_back(std::move(item));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_slash_menu_visible(false);
            handle->set_last_message(message);
            handle->set_assistant_text("");
            handle->set_status_text("正在提交请求");
            handle->set_chat_empty(false);
            handle->set_workspace_locked(true);
          }
        });
  }

  void bind_active_ray_to_selected_session() {
    if (active_ray_.empty()) return;
    const auto ray = active_ray_;
    auto navigation = navigation_;
    auto navigation_model = navigation_model_;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [this, ray, navigation, navigation_model, window] {
          bool changed = false;
          for (auto& item : *navigation) {
            if (!item.selected || item.kind != "session") continue;
            if (std::string(item.ray) != ray) {
              item.ray = display_string(ray);
              changed = true;
            }
            break;
          }
          if (!changed) return;
          auto query = std::string{};
          if (auto locked = window.lock()) query = std::string((*locked)->get_search_text());
          refresh_navigation(navigation_model, navigation, std::move(query));
          save_navigation();
        });
  }

  std::vector<tokmon::Photon> photons_from(const tokmon::SnowMessage& response) {
    std::vector<tokmon::Photon> result;
    const auto* field = tokmon::cbor::find(response.payload, "photons");
    if (field == nullptr || field->as_array() == nullptr) return result;
    for (const auto& encoded : *field->as_array()) {
      auto photon = tokmon::photon_from_cbor(encoded);
      if (photon) result.push_back(std::move(*photon));
    }
    return result;
  }

  std::vector<tokmon::Photon> photons_from_surface(
      const tokmon::SnowMessage& response) const {
    const auto* encoded = tokmon::cbor::find(response.payload, "surface");
    if (!encoded) return {};
    auto surface = tokmon::surface_from_cbor(*encoded);
    if (!surface) return {};
    const tokmon::SurfaceContribution* selected = nullptr;
    for (const auto& contribution : surface->contributions) {
      if (contribution.channel != "ui.trajectory" ||
          contribution.value.as_array() == nullptr) continue;
      if (!selected || contribution.priority > selected->priority)
        selected = &contribution;
    }
    if (!selected) return {};
    std::vector<tokmon::Photon> result;
    result.reserve(selected->value.as_array()->size());
    for (const auto& item : *selected->value.as_array()) {
        tokmon::Photon photon;
        photon.sequence = static_cast<std::uint64_t>(
            tokmon::cbor::find(item, "sequence")
                ? tokmon::cbor::find(item, "sequence")->as_integer() : 0);
        photon.id = tokmon::cbor::find(item, "id")
            ? std::string(tokmon::cbor::find(item, "id")->as_string()) : std::string{};
        photon.ray = active_ray_;
        photon.kind = tokmon::cbor::find(item, "kind")
            ? std::string(tokmon::cbor::find(item, "kind")->as_string()) : std::string{};
        photon.schema = tokmon::cbor::find(item, "schema")
            ? std::string(tokmon::cbor::find(item, "schema")->as_string()) : std::string{};
        if (const auto* payload = tokmon::cbor::find(item, "payload"))
          photon.payload = *payload;
        photon.committed_at_ms = tokmon::cbor::find(item, "time")
            ? tokmon::cbor::find(item, "time")->as_integer() : 0;
        photon.caused_by_act = tokmon::cbor::find(item, "caused_by_act")
            ? std::string(tokmon::cbor::find(item, "caused_by_act")->as_string())
            : std::string{};
        if (!photon.id.empty() && !photon.kind.empty()) result.push_back(std::move(photon));
    }
    return result;
  }

  void publish_session_files(const bool select_first) {
    auto window = window_;
    std::vector<ProjectFile> files;
    std::vector<CodeLine> preview;
    std::string selected_name;
    std::string selected_content;
    int added_lines = 0;
    {
      std::scoped_lock lock(files_mutex_);
      files.reserve(session_files_.size());
      for (const auto& file : session_files_)
        files.push_back(ProjectFile{
            display_string(file.name), display_string(file.path), {}});
      if (!session_files_.empty()) {
        const auto& chosen = session_files_.front();
        selected_name = chosen.name;
        selected_content = chosen.content;
        added_lines = chosen.written
            ? static_cast<int>(std::count(chosen.content.begin(),
                                          chosen.content.end(), '\n')) + 1
            : 0;
      }
      full_preview_lines_ = code_lines_from_text(selected_content);
      preview = full_preview_lines_;
    }
    (void)slint::invoke_from_event_loop(
        [window, files = std::move(files), preview = std::move(preview),
         selected_name, selected_content, added_lines, select_first]() mutable {
          auto files_model = std::make_shared<slint::VectorModel<ProjectFile>>();
          for (auto& file : files) files_model->push_back(std::move(file));
          auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
          for (auto& line : preview) preview_model->push_back(std::move(line));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_project_files(files_model);
            handle->set_preview_lines(preview_model);
            if (select_first || !selected_name.empty()) {
              handle->set_selected_file_name(display_string(selected_name));
              handle->set_preview_content(display_string(selected_content));
              handle->set_file_added_lines(added_lines);
            }
            if (select_first && files_model->row_count() == 0)
              handle->set_selected_file_name(slint::SharedString{});
          }
        });
  }

  void apply_photons(std::vector<tokmon::Photon> incoming, const bool replace) {
    if (replace) photons_.clear();
    for (auto& photon : incoming) {
      const auto found = std::ranges::find(photons_, photon.id, &tokmon::Photon::id);
      if (found == photons_.end()) photons_.push_back(std::move(photon));
      else *found = std::move(photon);
    }
    std::ranges::sort(photons_, {}, &tokmon::Photon::sequence);
    std::vector<TimelineItem> items;
    items.reserve(photons_.size());
    for (const auto& photon : photons_) items.push_back(timeline_item(photon));
    auto workflow_items = conversation_workflow_from(photons_);
    const bool workflow_complete = std::ranges::any_of(
        workflow_items, [](const TimelineItem& item) {
          return std::string(item.kind) == "task.completed";
        });
    const auto trace = trace_summary_from(photons_);
    auto lines = code_lines_from(photons_);
    {
      std::scoped_lock lock(files_mutex_);
      session_files_.clear();
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator) {
        const auto kind = std::string(iterator->kind);
        if (!kind.starts_with("fs.") && kind != "artifact.previewed") continue;
        auto file = session_file_from_photon(*iterator);
        if (file.name.empty()) continue;
        bool merged = false;
        for (auto& existing : session_files_) {
          if (existing.path != file.path) continue;
          if (!file.content.empty()) existing.content = file.content;
          existing.written = existing.written || file.written;
          merged = true;
          break;
        }
        if (!merged) session_files_.push_back(std::move(file));
        if (session_files_.size() >= 24u) break;
      }
      for (auto& file : session_files_) {
        if (!file.content.empty() || file.path.empty()) continue;
        std::error_code error;
        std::ifstream stream(path_from_utf8(file.path), std::ios::binary);
        if (stream && std::filesystem::exists(path_from_utf8(file.path), error)) {
          std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
          if (text.size() <= 262'144u) file.content = std::move(text);
        }
      }
    }
    publish_session_files(false);
    publish_trace_view();
    auto timeline = timeline_;
    auto workflow = conversation_workflow_;
    auto code = code_;
    auto window = window_;
    std::string assistant;
    std::string user_message;
    std::string current_turn_time;
    std::uint64_t turn_start_sequence = 0;
    for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator) {
      if (iterator->kind != "user.input" && iterator->kind != "user.message") continue;
      turn_start_sequence = iterator->sequence;
      current_turn_time = std::string(time_label(iterator->committed_at_ms));
      if (const auto* text = tokmon::cbor::find(iterator->payload, "text"))
        user_message = std::string(text->as_string());
      break;
    }
    std::string state = "正在沿光路执行";
    for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator) {
      if (iterator->sequence < turn_start_sequence) continue;
      if (assistant.empty() && iterator->kind == "assistant.message") {
        if (const auto* text = tokmon::cbor::find(iterator->payload, "text"))
          assistant = std::string(text->as_string());
      }
      if (iterator->kind == "ray.darkened" || iterator->kind == "assistant.message")
        state = "审阅完成";
    }
    if (assistant.empty())
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator)
        if (iterator->sequence >= turn_start_sequence && iterator->kind == "tool.result") {
          assistant = "真实工具已执行：" + tokmon::cbor::diagnostic(iterator->payload);
          break;
        }
    if (assistant.empty())
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator)
        if (iterator->sequence >= turn_start_sequence &&
            (iterator->kind == "act.failed" || iterator->kind == "model.failed")) {
          const auto* detail = tokmon::cbor::find(iterator->payload, "detail");
          if (!detail) detail = tokmon::cbor::find(iterator->payload, "error");
          assistant = "执行失败：" + (detail
              ? std::string(detail->as_string())
              : std::string("请在轨迹中查看失败 Photon"));
          state = "执行失败";
          break;
        }
    (void)slint::invoke_from_event_loop(
        [timeline, workflow, code, window, items = std::move(items),
         workflow_items = std::move(workflow_items), lines = std::move(lines), trace,
          assistant = std::move(assistant), user_message = std::move(user_message),
          current_turn_time = std::move(current_turn_time), state = std::move(state),
          workflow_complete, replace]() mutable {
          timeline->clear();
          for (auto& item : items) timeline->push_back(std::move(item));
          workflow->clear();
          for (auto& item : workflow_items) workflow->push_back(std::move(item));
          code->clear();
          for (auto& line : lines) code->push_back(std::move(line));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            if (replace || !assistant.empty())
              handle->set_assistant_text(display_string(assistant));
            if (replace || !user_message.empty())
             handle->set_last_message(display_string(user_message));
           handle->set_status_text(display_string(state));
            handle->set_chat_empty(items.empty() && workflow_items.empty() &&
                                   assistant.empty());
            if (!user_message.empty()) handle->set_workspace_locked(true);
            handle->set_chat_time(display_string(current_turn_time));
            handle->set_trace_duration(display_string(trace.duration));
            handle->set_workflow_duration(display_string(trace.turn_duration));
            handle->set_trace_turns(trace.turns);
            handle->set_trace_calls(trace.calls);
            handle->set_trace_input_tokens(static_cast<int>(std::min<std::int64_t>(
                trace.input_tokens, std::numeric_limits<int>::max())));
            handle->set_trace_output_tokens(static_cast<int>(std::min<std::int64_t>(
                trace.output_tokens, std::numeric_limits<int>::max())));
            handle->set_trace_provider(display_string(trace.provider));
            handle->set_trace_model(display_string(trace.model));
            handle->set_trace_result(display_string(trace.result));
            handle->set_workflow_done(workflow_complete ? 1 : 0);
            handle->set_daemon_state("后台服务已连接");
          }
        });
  }

  void apply_settings(tokmon::cbor::Value values, const bool include_navigation) {
    auto window = window_;
    auto navigation = navigation_;
    auto navigation_model = navigation_model_;
    const auto assets = assets_;
    const auto workspace = current_workspace_;
    const auto navigation_workspace = navigation_workspace_;
    (void)slint::invoke_from_event_loop([this, window, navigation, navigation_model, assets,
                                         workspace, navigation_workspace, include_navigation,
                                         values = std::move(values)] {
      auto locked = window.lock();
      if (!locked || !values.as_map()) return;
      auto handle = *locked;
      const auto string_value = [&values](const char* key)
          -> std::optional<slint::SharedString> {
        const auto* field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<std::string>(field->data)) return std::nullopt;
        return display_string(field->as_string());
      };
      const auto bool_value = [&values](const char* key) -> std::optional<bool> {
        const auto* field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<bool>(field->data)) return std::nullopt;
        return field->as_bool();
      };
      const auto int_value = [&values](const char* key) -> std::optional<int> {
        const auto* field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<std::int64_t>(field->data)) return std::nullopt;
        return static_cast<int>(field->as_integer());
      };
      if (auto value = string_value("language")) handle->set_setting_language(*value);
      if (auto value = string_value("startup")) handle->set_setting_startup(*value);
      if (auto value = string_value("autosave")) handle->set_setting_autosave(*value);
      else if (auto value = bool_value("autosave")) handle->set_setting_autosave(display_string(*value ? "5 分钟" : "关闭"));
      if (auto value = string_value("provider")) handle->set_setting_provider(*value);
      if (auto value = string_value("main_model")) {
        handle->set_setting_main_model(*value); handle->set_model_name(*value);
      }
      if (auto value = string_value("reasoning")) handle->set_setting_reasoning(*value);
      if (auto value = string_value("reasoning")) handle->set_effort(*value);
      if (auto value = string_value("command_approval")) handle->set_setting_command_approval(*value);
      if (auto value = bool_value("network")) handle->set_setting_network(*value);
      if (auto value = bool_value("high_risk_confirmation")) handle->set_setting_high_risk(*value);
      if (auto value = string_value("index_mode")) handle->set_setting_index_mode(*value);
      if (auto value = bool_value("workspace_sync")) handle->set_setting_workspace_sync(*value);
      if (auto value = bool_value("git")) handle->set_setting_git(*value);
      if (auto value = bool_value("notifications")) handle->set_setting_notifications(*value);
      if (auto value = bool_value("desktop_notifications")) handle->set_setting_desktop_notifications(*value);
      if (auto value = bool_value("message_alerts")) handle->set_setting_message_alerts(*value);
      if (auto value = string_value("quiet_hours")) handle->set_setting_quiet_hours(*value);
      else if (auto value = bool_value("quiet_hours")) handle->set_setting_quiet_hours(display_string(*value ? "22:00 - 08:00" : "关闭"));
      if (auto value = bool_value("dark_theme")) handle->set_setting_dark_theme(*value);
      if (auto value = int_value("accent")) handle->set_setting_accent(*value);
      if (auto value = string_value("density")) handle->set_setting_density(*value);
      if (auto value = int_value("font_scale")) handle->set_setting_font_scale(*value);
      if (auto value = string_value("nickname")) handle->set_setting_nickname(*value);
      if (auto value = string_value("email")) handle->set_setting_email(*value);
      if (auto value = bool_value("cloud_sync")) handle->set_setting_cloud_sync(*value);
      if (auto value = bool_value("sidebar_visible")) handle->set_sidebar_visible(*value);
      if (auto value = bool_value("code_visible")) handle->set_code_visible(*value);
      if (auto value = bool_value("task_expanded")) handle->set_task_expanded(*value);
      if (auto value = string_value("update_channel")) handle->set_setting_channel(*value);
      if (auto value = string_value("file_access")) handle->set_setting_file_access(*value);
      handle->set_setting_workspace(display_string(path_to_utf8(workspace)));
      if (include_navigation) {
        const auto* encoded = tokmon::cbor::find(values, "navigation");
        if (encoded) {
          if (auto decoded = navigation_items(*encoded, assets, navigation_workspace)) {
            *navigation = std::move(*decoded);
            refresh_navigation(navigation_model, navigation,
                               std::string(handle->get_search_text()));
            save_navigation();
            bool selection_changed = false;
            for (std::size_t index = 0; index < navigation->size(); ++index) {
              auto& item = (*navigation)[index];
              const auto target = navigation_workspace_at(
                  *navigation, index, navigation_workspace);
              if (item.selected && !restore_initial_workspace_ &&
                  !same_workspace(target, current_workspace_)) {
                item.selected = false;
                selection_changed = true;
                continue;
              }
              if (item.selected && item.kind == "session") {
                handle->set_session_title(item.title);
                const auto target_text = path_to_utf8(target);
                switch_workspace(target_text);
                open_session(std::string(item.ray), target_text);
                break;
              }
              if (item.selected && item.kind == "project") {
                const auto target_text = path_to_utf8(target);
                switch_workspace(target_text);
                new_session(target_text);
                break;
              }
            }
            if (selection_changed)
              refresh_navigation(navigation_model, navigation,
                                 std::string(handle->get_search_text()));
          }
        }
      }
      handle->set_settings_status("已从项目级 .tokmon/config.yaml 载入");
      if (include_navigation) finish_initialization();
    });
    publish_workspace_info();
  }

  void apply_providers(const tokmon::cbor::Value& payload) {
    const auto* selected = tokmon::cbor::find(payload, "default");
    const auto* providers = tokmon::cbor::find(payload, "providers");
    if (!selected || !providers || !providers->as_array()) return;
    tokmon::cbor::Value chosen;
    std::vector<ModelChoice> choices;
    for (const auto& provider : *providers->as_array()) {
      const auto* id = tokmon::cbor::find(provider, "id");
      const auto* model = tokmon::cbor::find(provider, "model");
      const auto* enabled = tokmon::cbor::find(provider, "enabled");
      if (id && model && (!enabled || enabled->as_bool())) {
        ModelChoice choice;
        choice.provider = display_string(id->as_string());
        choice.model = display_string(model->as_string());
        choice.label = display_string(std::string(id->as_string()) + " · " +
                                      std::string(model->as_string()));
        choices.push_back(std::move(choice));
      }
      if (const auto* id = tokmon::cbor::find(provider, "id");
          id && id->as_string() == selected->as_string()) chosen = provider;
    }
    if (!chosen.as_map()) return;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [window, chosen = std::move(chosen), choices = std::move(choices)]() mutable {
      auto locked = window.lock();
      if (!locked) return;
      auto handle = *locked;
      auto model = std::make_shared<slint::VectorModel<ModelChoice>>();
      for (auto& choice : choices) model->push_back(std::move(choice));
      handle->set_model_choices(model);
      const auto string_field = [&chosen](const char* key) {
        const auto* value = tokmon::cbor::find(chosen, key);
        return display_string(value ? value->as_string() : std::string_view{});
      };
      handle->set_setting_provider(string_field("id"));
      handle->set_setting_provider_protocol(string_field("protocol"));
      handle->set_setting_provider_endpoint(string_field("endpoint"));
      handle->set_setting_provider_auth(string_field("auth"));
      handle->set_setting_main_model(string_field("model"));
      handle->set_model_name(string_field("model"));
      const auto* thinking = tokmon::cbor::find(chosen, "thinking");
      handle->set_setting_provider_thinking(thinking && thinking->as_bool());
      const auto* credential = tokmon::cbor::find(chosen, "credential_present");
      handle->set_setting_provider_credential(
          credential && credential->as_bool() ? "凭据已安全保存（输入可轮换）" : "尚未配置 API Key");
      handle->set_settings_status("provider 配置已由 tokmond 验证并载入");
    });
  }

  void update_daemon_state(const slint::SharedString& state) {
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, state] {
      if (auto locked = window.lock()) {
        auto handle = *locked;
        handle->set_daemon_state(state);
      }
    });
  }

  void apply_command_response(const tokmon::cbor::Value& payload) {
    const auto read_string = [&payload](const char* key) {
      const auto* value = tokmon::cbor::find(payload, key);
      return value ? std::string(value->as_string()) : std::string{};
    };
    const auto read_bool = [&payload](const char* key) {
      const auto* value = tokmon::cbor::find(payload, key);
      return value && value->as_bool();
    };
    const auto display = read_string("display");
    const auto copied = read_string("copy_text");
    const auto title = read_string("session_title");
    const auto model = read_string("model");
    const auto provider = read_string("provider");
    const auto effort = read_string("effort");
    const auto access = read_string("access_mode");
    const auto clear = read_bool("clear_session");
    const auto settings = read_bool("open_settings");
    const auto close = read_bool("close_client");
    if (!copied.empty()) copy_to_clipboard(copied);
    auto timeline = timeline_; auto workflow = conversation_workflow_;
    auto code = code_; auto window = window_;
    (void)slint::invoke_from_event_loop([timeline, workflow, code, window, display, title, model,
        provider, effort, access, clear, settings, close, copied]() {
      if (clear) { timeline->clear(); workflow->clear(); code->clear(); }
      if (auto locked = window.lock()) {
        auto handle = *locked;
        if (!display.empty()) handle->set_assistant_text(display_string(display));
        if (!title.empty()) handle->set_session_title(display_string(title));
        if (!model.empty()) handle->set_model_name(display_string(model));
        if (!provider.empty()) handle->set_setting_provider(display_string(provider));
        if (!effort.empty()) handle->set_effort(effort == "low" ? "低" :
            effort == "medium" ? "标准" : effort == "high" ? "高" : "最高");
        if (!access.empty()) handle->set_access_mode(access == "full" ? "完全访问" :
            access == "restricted" ? "受限访问" : "只读模式");
        if (settings) handle->set_settings_open(true);
        handle->set_status_text(copied.empty() ? "命令已完成" : "内容已复制到剪贴板");
      }
      if (close) slint::quit_event_loop();
    });
  }

  void show_error(std::string message) {
    TimelineItem item;
    item.time = "now"; item.kind = "snow.error";
    item.title = "tokmond 连接失败"; item.detail = display_string(message);
    item.progress = -1; item.tone = "danger";
    auto model = timeline_;
    update_daemon_state("后台服务连接失败");
    (void)slint::invoke_from_event_loop([model, item = std::move(item)]() mutable {
      model->push_back(std::move(item));
    });
  }

  void show_workspace_error(std::string message) {
    TimelineItem item;
    item.time = "now";
    item.kind = "workspace.error";
    item.title = "工作空间切换失败";
    item.detail = display_string(message);
    item.progress = -1;
    item.tone = "danger";
    auto model = timeline_;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [model, window, item = std::move(item), message = std::move(message)]() mutable {
          model->push_back(std::move(item));
          if (auto locked = window.lock()) {
            (*locked)->set_daemon_state("原工作空间仍连接");
            (*locked)->set_settings_status(display_string(message));
          }
        });
  }

  bool activate_workspace(const std::string_view requested) {
    auto target = normalize_workspace_path(requested, navigation_workspace_);
    if (!target) {
      show_workspace_error("工作空间路径无效；请输入有效的文件夹路径");
      return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(*target, directory_error);
    if (directory_error) {
      show_workspace_error("无法创建工作空间：" + directory_error.message());
      return false;
    }
    if (same_workspace(*target, current_workspace_)) {
      auto window = window_;
      const auto display = display_string(path_to_utf8(*target));
      (void)slint::invoke_from_event_loop([window, display] {
        if (auto locked = window.lock()) {
          (*locked)->set_setting_workspace(display);
          (*locked)->set_daemon_state("当前工作空间已连接");
        }
      });
      return true;
    }

    auto paths = tokmon::resolve_paths(*target);
    if (!paths) {
      show_workspace_error(paths.error().describe());
      return false;
    }
    const auto target_endpoint = tokmon::workspace_snow_endpoint(
        paths->run, paths->project.parent_path());

    std::optional<tokmon::DaemonClientLease> next_lease;
    bool started = false;
    if (!same_workspace(*target, navigation_workspace_)) {
      auto connected = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
          .endpoint = target_endpoint,
          .workspace = *target,
          .executable = daemon_executable_});
      if (!connected) {
        show_workspace_error("无法启动工作空间 tokmond：" + connected.error().describe());
        return false;
      }
      started = connected->started;
      auto attached = tokmon::DaemonClientLease::attach(tokmon::DaemonClientOptions{
          .endpoint = target_endpoint,
          .client_id = tokmon::make_id("desktop-workspace-client"),
          .client_kind = "desktop",
          .shutdown_when_idle = true,
          .idle_timeout = std::chrono::milliseconds(250),
          .lease_ttl = std::chrono::seconds(6)});
      if (!attached) {
        show_workspace_error("无法附着工作空间 tokmond：" + attached.error().describe());
        return false;
      }
      next_lease.emplace(std::move(*attached));
    }

    if (active_workspace_lease_) {
      (void)active_workspace_lease_->detach();
      active_workspace_lease_.reset();
    }
    if (next_lease) active_workspace_lease_.emplace(std::move(*next_lease));
    endpoint_ = target_endpoint;
    current_workspace_ = *target;
    cursor_ = 0;
    active_ray_.clear();
    photons_.clear();
    last_error_.clear();
    {
      std::scoped_lock lock(files_mutex_);
      session_files_.clear();
      full_preview_lines_.clear();
    }

    auto timeline = timeline_;
    auto workflow = conversation_workflow_;
    auto code = code_;
    auto window = window_;
    const auto display = display_string(path_to_utf8(*target));
    const auto state = slint::SharedString(started
        ? "工作空间后台服务已自动启动" : "工作空间后台服务已连接");
    (void)slint::invoke_from_event_loop([timeline, workflow, code, window, display, state] {
      timeline->clear();
      workflow->clear();
      code->clear();
      if (auto locked = window.lock()) {
        auto handle = *locked;
        handle->set_assistant_text("");
        handle->set_last_message("");
        handle->set_status_text("等待输入");
        handle->set_chat_empty(true);
        handle->set_chat_time("");
        handle->set_workspace_locked(false);
        handle->set_setting_workspace(display);
        handle->set_settings_status("已切换工作空间；正在载入项目级 .tokmon/config.yaml");
        handle->set_daemon_state(state);
      }
    });
    publish_workspace_info();
    load_settings(false);
    load_providers();
    return true;
  }

  void run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
      Command command;
      {
        std::unique_lock lock(mutex_);
        const auto queued = condition_.wait_for(lock, stop, std::chrono::milliseconds(500),
                                                [this] { return !commands_.empty(); });
        if (stop.stop_requested()) return;
        if (queued) {
          command = std::move(commands_.front()); commands_.pop_front();
        } else {
          // A snapshot is explicitly queued at startup. Replaying the global
          // tail while a new session is idle would overwrite its blank state
          // with an unrelated older ray.
          continue;
        }
      }
      if (command.kind == "switch-workspace") {
        (void)activate_workspace(command.text);
        continue;
      }
      if (command.kind == "new-session" || command.kind == "open-session") {
        if (const auto* expected = tokmon::cbor::find(command.payload, "workspace");
            expected && !expected->as_string().empty()) {
          auto target = normalize_workspace_path(expected->as_string(), navigation_workspace_);
          if (!target || !same_workspace(*target, current_workspace_)) {
            show_workspace_error(
                "目标工作空间尚未连接；会话未打开，原工作空间与 Ray 保持不变");
            continue;
          }
        }
        active_ray_.clear();
        photons_.clear();
        {
          std::scoped_lock lock(files_mutex_);
          session_files_.clear();
          full_preview_lines_.clear();
        }
        publish_session_files(true);
        auto timeline = timeline_; auto workflow = conversation_workflow_; auto code = code_;
        auto window = window_;
        (void)slint::invoke_from_event_loop([timeline, workflow, code, window] {
          timeline->clear(); workflow->clear(); code->clear();
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_assistant_text("");
            handle->set_last_message("");
            handle->set_chat_time("");
            handle->set_status_text("等待输入");
            handle->set_chat_empty(true);
            handle->set_workspace_locked(false);
            handle->set_selected_file_name("");
            handle->set_file_added_lines(0);
            handle->set_preview_content("");
          }
        });
        if (command.kind == "new-session" || command.text.empty()) continue;
        active_ray_ = command.text;
      }
      if (command.kind == "chat" || command.kind == "slash-command")
        publish_pending(command.text);
      tokmon::SnowMessage request;
      request.request_id = tokmon::next_snow_request_id();
      if (command.kind == "snapshot") {
        request.kind = tokmon::SnowMessageKind::snapshot_request;
        request.cursor = cursor_;
      } else {
        request.kind = tokmon::SnowMessageKind::intent;
        if (command.kind == "chat") {
          request.payload = tokmon::cbor::object({{"action", "chat"},
              {"text", command.text}, {"ray", active_ray_}});
          if (const auto* selection = command.payload.as_map())
            for (const auto& [key, value] : *selection)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "slash-command") {
          request.payload = tokmon::cbor::object({{"action", "command.execute"},
              {"text", command.text}, {"ray", active_ray_}});
          if (const auto* selection = command.payload.as_map())
            for (const auto& [key, value] : *selection)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "settings-load")
          request.payload = tokmon::cbor::object({{"action", "settings.get"}});
        else if (command.kind == "providers-load")
          request.payload = tokmon::cbor::object({{"action", "model.providers"}});
        else if (command.kind == "settings-save")
          request.payload = tokmon::cbor::object({{"action", "settings.save"},
              {"values", std::move(command.payload)}});
        else if (command.kind == "navigation-save")
          request.payload = tokmon::cbor::object({{"action", "navigation.save"},
                                                  {"items", std::move(command.payload)}});
        else if (command.kind == "open-session")
          request.payload = tokmon::cbor::object({{"action", "surface"},
                                                  {"ray", command.text}});
        else if (command.kind == "provider-configure") {
          request.payload = tokmon::cbor::object({{"action", "model.provider.configure"}});
          if (const auto* values = command.payload.as_map())
            for (const auto& [key, value] : *values) (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-use") {
          request.payload = tokmon::cbor::object({{"action", "model.provider.use"}});
          if (const auto* values = command.payload.as_map())
            for (const auto& [key, value] : *values) (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-secret") {
          request.payload = tokmon::cbor::object({{"action", "model.provider.secret.set"}});
          if (const auto* values = command.payload.as_map())
            for (const auto& [key, value] : *values) (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-test") {
          request.payload = tokmon::cbor::object({{"action", "model.provider.test"}});
          if (const auto* values = command.payload.as_map())
            for (const auto& [key, value] : *values) (*request.payload.as_map())[key] = value;
        }
        else request.payload = tokmon::cbor::object({{"action", "lens.reconcile"}});
      }
      const auto& request_endpoint = command.kind == "navigation-save"
          ? navigation_endpoint_ : endpoint_;
      tokmon::SnowClient client(request_endpoint);
      auto response = client.request(request);
      if (!response) {
        const auto message = response.error().describe();
        if (message != last_error_) { last_error_ = message; show_error(message); }
        if (command.kind == "settings-load") {
          const auto* include = tokmon::cbor::find(command.payload, "include_navigation");
          if (include && include->as_bool()) finish_initialization();
        }
        continue;
      }
      last_error_.clear();
      update_daemon_state("后台服务已连接");
      if (response->kind == tokmon::SnowMessageKind::error) {
        const auto* message = tokmon::cbor::find(response->payload, "message");
        show_error(message ? std::string(message->as_string()) : "tokmond 拒绝了请求");
        if (command.kind == "settings-load") {
          const auto* include = tokmon::cbor::find(command.payload, "include_navigation");
          if (include && include->as_bool()) finish_initialization();
        }
        continue;
      }
      if (command.kind != "navigation-save")
        cursor_ = std::max(cursor_, response->cursor);
      if (command.kind == "settings-load") {
        const auto* include = tokmon::cbor::find(command.payload, "include_navigation");
        if (const auto* values = tokmon::cbor::find(response->payload, "values"))
          apply_settings(*values, include && include->as_bool());
        continue;
      }
      if (command.kind == "providers-load") {
        apply_providers(response->payload);
        continue;
      }
      if (command.kind == "settings-save") {
        auto window = window_;
        (void)slint::invoke_from_event_loop([window] {
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_settings_status("已原子保存到项目级 .tokmon/config.yaml");
          }
        });
        continue;
      }
      if (command.kind == "navigation-save") continue;
      if (command.kind == "open-session") {
        auto active = photons_from_surface(*response);
        apply_photons(std::move(active), true);
        continue;
      }
      if (command.kind == "provider-configure" || command.kind == "provider-secret" ||
          command.kind == "provider-use") {
        auto window = window_;
        const auto status = command.kind == "provider-configure"
            ? slint::SharedString("平台 YAML 已原子保存并完成热重载")
            : command.kind == "provider-secret"
            ? slint::SharedString("API Key 已写入系统凭据库；未进入 YAML/Photon/日志")
            : slint::SharedString("默认模型平台已切换并完成热重载");
        (void)slint::invoke_from_event_loop([window, status] {
          if (auto locked = window.lock()) (*locked)->set_settings_status(status);
        });
        load_providers();
        continue;
      }
      if (command.kind == "reconcile") continue;
      if (command.kind == "slash-command") {
        if (const auto* ray = tokmon::cbor::find(response->payload, "ray"))
          active_ray_ = std::string(ray->as_string());
        bind_active_ray_to_selected_session();
        if (tokmon::cbor::find(response->payload, "clear_session") &&
            tokmon::cbor::find(response->payload, "clear_session")->as_bool()) {
          photons_.clear();
        } else {
          auto photons = photons_from(*response);
          if (!photons.empty()) apply_photons(std::move(photons), true);
        }
        apply_command_response(response->payload);
        continue;
      }
      if (command.kind == "chat" || command.kind == "provider-test")
        if (const auto* ray = tokmon::cbor::find(response->payload, "ray"))
          active_ray_ = std::string(ray->as_string());
      if (command.kind == "chat") bind_active_ray_to_selected_session();
      auto photons = photons_from(*response);
      const bool received_full_photons = !photons.empty();
      if (!photons.empty())
        apply_photons(std::move(photons), response->kind == tokmon::SnowMessageKind::snapshot);
      if (command.kind == "provider-test") {
        auto window = window_;
        (void)slint::invoke_from_event_loop([window] {
          if (auto locked = window.lock())
            (*locked)->set_settings_status("真实 provider 请求已完成；结果已投影到对话与轨迹");
        });
      }
      if (!received_full_photons &&
          (command.kind == "chat" || command.kind == "provider-test") &&
          !active_ray_.empty()) {
        tokmon::SnowMessage surface_request;
        surface_request.kind = tokmon::SnowMessageKind::intent;
        surface_request.request_id = tokmon::next_snow_request_id();
        surface_request.cursor = cursor_;
        surface_request.payload = tokmon::cbor::object(
            {{"action", "surface"}, {"ray", active_ray_}});
        auto projected = client.request(surface_request);
        if (projected && projected->kind != tokmon::SnowMessageKind::error) {
          cursor_ = std::max(cursor_, projected->cursor);
          auto active = photons_from_surface(*projected);
          if (!active.empty()) apply_photons(std::move(active), true);
        }
      }
    }
  }

  std::filesystem::path endpoint_;
  std::filesystem::path navigation_endpoint_;
  std::filesystem::path current_workspace_;
  std::filesystem::path navigation_workspace_;
  std::filesystem::path daemon_executable_;
  std::optional<tokmon::DaemonClientLease> active_workspace_lease_;
  std::shared_ptr<slint::VectorModel<TimelineItem>> timeline_;
  std::shared_ptr<slint::VectorModel<TimelineItem>> conversation_workflow_;
  std::shared_ptr<slint::VectorModel<CodeLine>> code_;
  std::shared_ptr<slint::VectorModel<TraceEvent>> trace_events_;
  std::shared_ptr<slint::VectorModel<GanttSegment>> gantt_;
  std::shared_ptr<slint::VectorModel<NavigationItem>> navigation_model_;
  std::shared_ptr<std::vector<NavigationItem>> navigation_;
  std::mutex files_mutex_;
  std::vector<SessionFile> session_files_;
  std::vector<CodeLine> full_preview_lines_;
  std::filesystem::path assets_;
  slint::ComponentWeakHandle<MainWindow> window_;
  bool restore_initial_workspace_{true};
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Command> commands_;
  std::deque<Command> deferred_user_commands_;
  bool initializing_{true};
  std::uint64_t cursor_{0};
  tokmon::RayId active_ray_;
  std::vector<tokmon::Photon> photons_;
  std::string last_error_;
  std::jthread worker_;
};

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> workspace;
  bool open_settings = false;
  int settings_page = 0;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--workspace" && index + 1 < argc)
      workspace = argv[++index];
    else if (std::string_view(argv[index]) == "--open-settings") open_settings = true;
    else if (std::string_view(argv[index]) == "--settings-page" && index + 1 < argc) {
      try { settings_page = std::clamp(std::stoi(argv[++index]), 0, 7); }
      catch (...) { settings_page = 0; }
    }
  }
  auto paths = tokmon::resolve_paths(workspace);
  if (!paths) return 2;

  std::error_code path_error;
  auto executable = argc > 0 ? std::filesystem::absolute(argv[0], path_error)
                             : std::filesystem::current_path();
#if defined(_WIN32)
  std::wstring module(32'768, L'\0');
  const auto module_size = GetModuleFileNameW(nullptr, module.data(),
                                              static_cast<DWORD>(module.size()));
  if (module_size > 0 && module_size < module.size()) {
    module.resize(module_size);
    executable = std::filesystem::path(module);
  }
#endif
  const auto endpoint = tokmon::workspace_snow_endpoint(
      paths->run, paths->project.parent_path());
#if defined(_WIN32)
  const auto daemon_executable = executable.parent_path() / "tokmond.exe";
#else
  const auto daemon_executable = executable.parent_path() / "tokmond";
#endif
  auto connected = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
      .endpoint = endpoint,
      .workspace = paths->project.parent_path(),
      .executable = daemon_executable
  });
  if (!connected) {
#if defined(_WIN32)
    const auto description = connected.error().describe();
    const auto message = std::wstring(description.begin(), description.end());
    MessageBoxW(nullptr, message.c_str(), L"Tokmon 无法启动", MB_OK | MB_ICONERROR);
#endif
    return 2;
  }
  auto client_lease = tokmon::DaemonClientLease::attach(tokmon::DaemonClientOptions{
      .endpoint = endpoint,
      .client_id = tokmon::make_id("desktop-client"),
      .client_kind = "desktop",
      .shutdown_when_idle = true,
      .idle_timeout = std::chrono::milliseconds(250),
      .lease_ttl = std::chrono::seconds(6)});
  if (!client_lease) {
#if defined(_WIN32)
    const auto description = client_lease.error().describe();
    const auto message = std::wstring(description.begin(), description.end());
    MessageBoxW(nullptr, message.c_str(), L"Tokmon 无法连接后台服务",
                MB_OK | MB_ICONERROR);
#endif
    return 2;
  }
  auto window = MainWindow::create();
  window->set_settings_page(settings_page);
  window->set_settings_open(open_settings);
  auto assets = executable.parent_path() / "assets" / "figma";
  if (!std::filesystem::exists(assets))
    assets = std::filesystem::current_path() / "apps" / "tokmon-desktop" /
             "assets" / "figma";

  const auto navigation_workspace = paths->project.parent_path();
  const auto navigation_workspace_text = path_to_utf8(navigation_workspace);
  const std::vector<NavigationItem> navigation = [&assets, &navigation_workspace_text] {
    std::vector<NavigationItem> items;
    const auto add = [&items, &assets, &navigation_workspace_text](
                         const char* title, const char* kind, int indent, bool selected) {
      items.push_back(make_navigation_item(assets, tokmon::make_id("navigation"), kind,
          title, indent, selected, true, {},
          std::string_view(kind) == "project" ? navigation_workspace_text : std::string{}));
    };
    add("内容生产", "group", 0, false);
    add("字幕制作空间", "project", 1, false);
    add("生成音频时间轴字幕", "session", 2, true);
    add("字幕校对优化", "session", 2, false);
    add("批量字幕质检优化", "session", 2, false);
    add("音频切片处理", "project", 1, false);
    add("演示助手", "group", 0, false);
    add("PPT 智绘项目", "project", 1, false);
    add("PPT 大纲生成", "session", 2, false);
    add("演讲稿润色", "session", 2, false);
    add("旅行计划", "group", 0, false);
    return items;
  }();
  auto navigation_state =
      std::make_shared<std::vector<NavigationItem>>(navigation);
  auto nav_model = std::make_shared<slint::VectorModel<NavigationItem>>();
  refresh_navigation(nav_model, navigation_state, {});
  auto timeline_model = std::make_shared<slint::VectorModel<TimelineItem>>();
  auto conversation_workflow_model =
      std::make_shared<slint::VectorModel<TimelineItem>>();
  auto code_model = std::make_shared<slint::VectorModel<CodeLine>>();
  auto slash_model = std::make_shared<slint::VectorModel<SlashCommandItem>>();
  auto trace_events_model = std::make_shared<slint::VectorModel<TraceEvent>>();
  auto gantt_model = std::make_shared<slint::VectorModel<GanttSegment>>();
  for (auto& line : code_lines_from({})) code_model->push_back(std::move(line));
  window->set_navigation(nav_model);
  window->set_timeline(timeline_model);
  window->set_conversation_workflow(conversation_workflow_model);
  window->set_code_lines(code_model);
  window->set_slash_commands(slash_model);
  window->set_trace_events(trace_events_model);
  window->set_gantt(gantt_model);
  window->set_setting_workspace(
      display_string(navigation_workspace_text));
  window->set_daemon_state(connected->started ? "后台服务已自动启动" : "后台服务已连接");
  UiSnowController controller(endpoint, navigation_workspace, daemon_executable,
                               timeline_model, conversation_workflow_model, code_model,
                               trace_events_model, gantt_model, nav_model,
                               navigation_state, assets,
                               slint::ComponentWeakHandle<MainWindow>(window),
                               !workspace.has_value());
  controller.load_settings(true);
  controller.load_providers();
  window->on_slash_query_changed([slash_model, window](const slint::SharedString& text) {
    const auto query = std::string(text);
    const auto separator = query.find_first_of(" \t\r\n");
    const auto visible = tokmon::is_slash_command(query) && separator == std::string::npos;
    slash_model->clear();
    if (visible) {
      for (const auto* descriptor : tokmon::match_slash_commands(query, 8)) {
        SlashCommandItem item;
        item.command = slint::SharedString("/" + descriptor->name);
        item.usage = display_string(descriptor->usage);
        item.summary = display_string(descriptor->summary);
        item.category = display_string(descriptor->category);
        slash_model->push_back(std::move(item));
      }
    }
    window->set_slash_menu_visible(visible && slash_model->row_count() != 0);
  });
  window->on_send_message([&controller, conversation_workflow_model, window](
                              const slint::SharedString& text) {
    window->set_slash_menu_visible(false);
    conversation_workflow_model->clear();
    window->set_last_message(text);
    window->set_assistant_text("");
    window->set_status_text("正在提交请求");
    window->set_chat_empty(false);
    window->set_workspace_locked(true);
    if (tokmon::is_slash_command(std::string_view(text)))
      controller.slash_command(std::string(text), std::string(window->get_setting_provider()),
                               std::string(window->get_model_name()),
                               std::string(window->get_access_mode()),
                               std::string(window->get_effort()));
    else
      controller.chat(std::string(text), std::string(window->get_setting_provider()),
                      std::string(window->get_model_name()),
                      std::string(window->get_access_mode()),
                      std::string(window->get_effort()));
  });
  window->on_new_session([navigation_state, window, navigation_workspace] {
    std::size_t project = navigation_state->size();
    for (std::size_t index = 0; index < navigation_state->size(); ++index) {
      if (!(*navigation_state)[index].selected) continue;
      project = navigation_ancestor_at(*navigation_state, index, "project");
      break;
    }
    if (project == navigation_state->size())
      for (std::size_t index = 0; index < navigation_state->size(); ++index)
        if ((*navigation_state)[index].kind == "project") { project = index; break; }
    const auto title = "新会话 " + std::to_string(
        std::ranges::count_if(*navigation_state, [](const NavigationItem& item) {
          return item.kind == "session";
        }) + 1);
    const auto workspace = navigation_workspace_at(
        *navigation_state, project, navigation_workspace);
    window->set_create_navigation_kind("会话");
    window->set_create_navigation_name(display_string(title));
    window->set_create_navigation_workspace(display_string(path_to_utf8(workspace)));
    window->set_create_navigation_group(slint::SharedString{});
    window->set_create_navigation_error("");
    window->set_create_navigation_open(true);
  });
  window->on_quick_create([nav_model, navigation_state, window,
                           navigation_workspace](int index) {
    if (index < 0 || index >= static_cast<int>(nav_model->row_count())) return;
    const auto clicked = *nav_model->row_data(index);
    const auto found = std::ranges::find(*navigation_state, clicked.id,
                                         &NavigationItem::id);
    if (found == navigation_state->end()) return;
    const auto state_index = static_cast<std::size_t>(
        std::distance(navigation_state->begin(), found));
    const auto workspace = navigation_workspace_at(
        *navigation_state, state_index, navigation_workspace);
    slint::SharedString group;
    for (auto previous = state_index; previous > 0;) {
      --previous;
      if ((*navigation_state)[previous].indent >= found->indent) continue;
      if (std::string((*navigation_state)[previous].kind) == "group") {
        group = (*navigation_state)[previous].title;
        break;
      }
    }
    const auto title = "新会话 " + std::to_string(
        std::ranges::count_if(*navigation_state, [](const NavigationItem& item) {
          return item.kind == "session";
        }) + 1);
    window->set_create_navigation_kind("会话");
    window->set_create_navigation_name(display_string(title));
    window->set_create_navigation_workspace(display_string(path_to_utf8(workspace)));
    window->set_create_navigation_group(group);
    window->set_create_navigation_error("");
    window->set_create_navigation_open(true);
  });
  window->on_clear_search([nav_model, navigation_state, window] {
    window->set_search_text(slint::SharedString{});
    refresh_navigation(nav_model, navigation_state, {},
                       slint::ComponentWeakHandle<MainWindow>(window));
  });
  window->on_select_navigation([nav_model, navigation_state, window, &controller,
                                 navigation_workspace](int index) {
    if (index < 0 || index >= static_cast<int>(nav_model->row_count())) return;
    const auto clicked = *nav_model->row_data(index);
    const auto found = std::ranges::find(*navigation_state, clicked.id,
                                         &NavigationItem::id);
    if (found == navigation_state->end()) return;
    const auto state_index = static_cast<std::size_t>(
        std::distance(navigation_state->begin(), found));
    for (auto& candidate : *navigation_state) candidate.selected = false;
    found->selected = true;
    if (found->expandable) found->expanded = !found->expanded;
    const auto kind = std::string(found->kind);
    const auto ray = std::string(found->ray);
    const auto title = found->title;
    const auto workspace = navigation_workspace_at(
        *navigation_state, state_index, navigation_workspace);
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    controller.save_navigation();
    if (kind == "session") {
      window->set_session_title(title);
      const auto target = path_to_utf8(workspace);
      controller.switch_workspace(target);
      controller.open_session(ray, target);
    } else if (kind == "project") {
      window->set_session_title(title);
      const auto target = path_to_utf8(workspace);
      controller.switch_workspace(target);
      controller.new_session(target);
    }
  });
  window->on_search_changed([nav_model, navigation_state, window](
                                const slint::SharedString& text) {
    refresh_navigation(nav_model, navigation_state, std::string(text),
                       slint::ComponentWeakHandle<MainWindow>(window));
  });
  window->on_add_navigation([nav_model, navigation_state, assets, window] {
    NavigationItem item;
    item.id = display_string(tokmon::make_id("navigation"));
    item.kind = "project";
    item.title = slint::SharedString(
        "新建项目 " + std::to_string(navigation_state->size() + 1));
    item.icon = slint::Image::load_from_path(
        slint::SharedString((assets / "icon-06.svg").string()));
    item.indent = 0;
    item.selected = false;
    item.expandable = true;
    item.expanded = true;
    item.workspace = window->get_setting_workspace();
    navigation_state->push_back(std::move(item));
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
  });
  window->on_identify_project([](const slint::SharedString& path_value) {
    return display_string(path_basename_utf8(std::string(path_value)));
  });
  window->on_project_in_group(
      [navigation_state](const slint::SharedString& group_value,
                         const slint::SharedString& path_value) -> bool {
        const auto group = std::string(group_value);
        const auto name = path_basename_utf8(std::string(path_value));
        if (name.empty()) return false;
        std::size_t index = 0;
        while (index < navigation_state->size()) {
          const auto& item = (*navigation_state)[index];
          if (std::string(item.kind) == "group" && std::string(item.title) == group) {
            for (auto next = index + 1;
                 next < navigation_state->size() &&
                 (*navigation_state)[next].indent > item.indent;
                 ++next) {
              if (std::string((*navigation_state)[next].kind) != "project") continue;
              const auto project_title =
                  path_basename_utf8(std::string((*navigation_state)[next].workspace));
              if (std::string((*navigation_state)[next].title) == name ||
                  project_title == name)
                return true;
            }
            return false;
          }
          ++index;
        }
        return false;
      });
  window->on_create_navigation([nav_model, navigation_state, assets, window, &controller,
                                 navigation_workspace](
      const slint::SharedString& kind_value, const slint::SharedString& title_value,
      const slint::SharedString& group_value,
      const slint::SharedString& workspace_value) -> bool {
    const auto kind = std::string(kind_value) == "会话" ? "session"
        : std::string(kind_value) == "项目" ? "project"
        : std::string(kind_value) == "分组" ? "group" : std::string(kind_value);
    const auto title = std::string(title_value);
    const auto requested_group = std::string(group_value);
    if ((kind != "group" && kind != "project" && kind != "session") ||
        title.empty() || title.size() > 256) {
      window->set_create_navigation_error("名称必须为 1–256 个字符");
      return false;
    }
    std::size_t parent = navigation_state->size();
    std::size_t host_group = navigation_state->size();
    const auto locate_group = [&](const std::string_view preferred)
        -> std::size_t {
      if (!preferred.empty())
        for (std::size_t index = 0; index < navigation_state->size(); ++index)
          if (std::string((*navigation_state)[index].kind) == "group" &&
              std::string((*navigation_state)[index].title) == preferred)
            return index;
      for (std::size_t index = 0; index < navigation_state->size(); ++index)
        if ((*navigation_state)[index].kind == "group") return index;
      return navigation_state->size();
    };
    if (kind == "group") {
      parent = navigation_state->size();
    } else if (kind == "project") {
      host_group = locate_group(requested_group);
      parent = host_group;
    } else {
      // The session attaches to an existing project of the chosen group whose
      // workspace matches the requested directory; otherwise a fresh project
      // is created under that group first.
      host_group = locate_group(requested_group);
      std::string normalized_requested;
      if (auto normalized = normalize_workspace_path(
              std::string(workspace_value), navigation_workspace))
        normalized_requested = path_to_utf8(*normalized);
      const auto project_name =
          path_basename_utf8(normalized_requested.empty()
                                 ? std::string(workspace_value)
                                 : normalized_requested);
      if (host_group != navigation_state->size()) {
        for (auto next = host_group + 1;
             next < navigation_state->size() &&
             (*navigation_state)[next].indent > (*navigation_state)[host_group].indent;
             ++next) {
          if (std::string((*navigation_state)[next].kind) != "project") continue;
          const auto stored = std::string((*navigation_state)[next].workspace);
          const auto candidate_name =
              path_basename_utf8(stored.empty()
                                     ? std::string((*navigation_state)[next].title)
                                     : stored);
          if ((!stored.empty() && !normalized_requested.empty() &&
               same_workspace(path_from_utf8(stored),
                              path_from_utf8(normalized_requested))) ||
              candidate_name == project_name) {
            parent = next;
            break;
          }
        }
        if (parent == navigation_state->size() && !project_name.empty()) {
          for (auto& item : *navigation_state) item.selected = false;
          auto hosting = make_navigation_item(assets, tokmon::make_id("navigation"),
              "project", project_name,
              (*navigation_state)[host_group].indent + 1, true, true, {},
              normalized_requested);
          std::size_t insertion = host_group + 1;
          while (insertion < navigation_state->size() &&
                 (*navigation_state)[insertion].indent >
                     (*navigation_state)[host_group].indent)
            ++insertion;
          navigation_state->insert(navigation_state->begin() +
              static_cast<std::ptrdiff_t>(insertion), std::move(hosting));
          host_group = insertion;
        }
      }
      if (parent == navigation_state->size()) parent = host_group;
    }

    std::filesystem::path workspace = navigation_workspace;
    std::string stored_workspace;
    if (kind != "group") {
      auto normalized = normalize_workspace_path(
          std::string(workspace_value), navigation_workspace);
      if (!normalized) {
        window->set_create_navigation_error("请选择或输入有效的工作空间路径");
        return false;
      }
      std::error_code directory_error;
      std::filesystem::create_directories(*normalized, directory_error);
      if (directory_error) {
        window->set_create_navigation_error(display_string(
            "无法创建工作空间：" + directory_error.message()));
        return false;
      }
      workspace = *normalized;
      if (kind == "project") {
        stored_workspace = path_to_utf8(workspace);
      } else if (kind == "session") {
        if (parent != navigation_state->size() &&
            std::string((*navigation_state)[parent].kind) == "project")
          stored_workspace = std::string((*navigation_state)[parent].workspace);
        else {
          const auto inherited = navigation_workspace_at(
              *navigation_state, parent, navigation_workspace);
          if (!same_workspace(workspace, inherited))
            stored_workspace = path_to_utf8(workspace);
        }
      }
    }
    for (auto& item : *navigation_state) item.selected = false;
    auto created = make_navigation_item(assets,
        tokmon::make_id(kind == "session" ? "session" : "navigation"), kind,
        title, parent == navigation_state->size() ? 0 :
            (*navigation_state)[parent].indent + 1, true, true, {}, stored_workspace);
    std::size_t insertion = navigation_state->size();
    if (parent != navigation_state->size()) {
      (*navigation_state)[parent].expanded = true;
      insertion = parent + 1;
      while (insertion < navigation_state->size() &&
             (*navigation_state)[insertion].indent > (*navigation_state)[parent].indent)
        ++insertion;
    }
    navigation_state->insert(navigation_state->begin() +
        static_cast<std::ptrdiff_t>(insertion), std::move(created));
    refresh_navigation(nav_model, navigation_state, std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    window->set_session_title(display_string(title));
    controller.save_navigation();
    if (kind != "group") {
      const auto target = path_to_utf8(workspace);
      controller.switch_workspace(target);
      controller.new_session(target);
    }
    return true;
  });
  window->on_rename_session([navigation_state, nav_model, window, &controller](
      const slint::SharedString& title_value) {
    const auto title = std::string(title_value);
    if (title.empty() || title.size() > 256) return;
    for (auto& item : *navigation_state)
      if (item.selected && item.kind == "session") { item.title = title_value; break; }
    refresh_navigation(nav_model, navigation_state, std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    controller.save_navigation();
    controller.slash_command("/rename " + title,
        std::string(window->get_setting_provider()), std::string(window->get_model_name()),
        std::string(window->get_access_mode()), std::string(window->get_effort()));
  });
  window->on_choose_attachment([](bool directory) {
    return display_string(choose_attachment(directory));
  });
  window->on_open_change_workspace([&controller, window] {
    window->set_change_workspace_path(
        display_string(path_to_utf8(controller.current_workspace())));
    window->set_change_workspace_open(true);
  });
  window->on_confirm_change_workspace(
      [&controller](const slint::SharedString& path_value) {
        const auto target = std::string(path_value);
        if (target.empty()) return;
        controller.switch_workspace(target);
      });
  window->on_select_project_file([&controller](int index) {
    controller.select_session_file(index);
  });
  window->on_copy_text([&controller](const slint::SharedString& text) {
    copy_to_clipboard(std::string_view(text));
    controller.notify_copied();
  });
  window->on_filter_preview([&controller](const slint::SharedString& query) {
    controller.filter_preview_lines(std::string(query));
  });
  window->on_cancel_settings([&controller] {
    controller.load_settings();
    controller.load_providers();
  });
  window->on_reconcile([&controller] { controller.reconcile(); });
  window->on_configure_provider([&controller](const slint::SharedString& id,
      const slint::SharedString& protocol, const slint::SharedString& endpoint,
      const slint::SharedString& model, const slint::SharedString& auth, bool thinking,
      const slint::SharedString& effort) {
    const auto effort_value = std::string(effort) == "低" ? "low" :
        std::string(effort) == "标准" ? "medium" :
        std::string(effort) == "高" ? "high" : "max";
    controller.configure_provider(tokmon::cbor::object({
        {"id", std::string(id)}, {"protocol", std::string(protocol)},
        {"endpoint", std::string(endpoint)}, {"model", std::string(model)},
        {"auth", std::string(auth)}, {"thinking", thinking}, {"default", true},
        {"reasoning_effort", effort_value},
        {"max_output_tokens", 4096}, {"max_attempts", 6},
        {"retry_backoff_ms", 5'000}}));
  });
  window->on_store_provider_secret([&controller](const slint::SharedString& id,
                                                  const slint::SharedString& secret) {
    controller.store_provider_secret(std::string(id), std::string(secret));
  });
  window->on_test_provider([&controller](const slint::SharedString& id) {
    controller.test_provider(std::string(id));
  });
  window->on_save_settings([window, &controller] {
    controller.save_settings(tokmon::cbor::object({
        {"language", std::string(window->get_setting_language())},
        {"startup", std::string(window->get_setting_startup())},
        {"autosave", std::string(window->get_setting_autosave())},
        {"provider", std::string(window->get_setting_provider())},
        {"main_model", std::string(window->get_setting_main_model())},
        {"reasoning", std::string(window->get_setting_reasoning())},
        {"command_approval", std::string(window->get_setting_command_approval())},
        {"network", window->get_setting_network()},
        {"high_risk_confirmation", window->get_setting_high_risk()},
        {"workspace", std::string(window->get_setting_workspace())},
        {"index_mode", std::string(window->get_setting_index_mode())},
        {"workspace_sync", window->get_setting_workspace_sync()},
        {"git", window->get_setting_git()},
        {"notifications", window->get_setting_notifications()},
        {"desktop_notifications", window->get_setting_desktop_notifications()},
        {"message_alerts", window->get_setting_message_alerts()},
        {"quiet_hours", std::string(window->get_setting_quiet_hours())},
        {"dark_theme", window->get_setting_dark_theme()},
        {"accent", static_cast<std::int64_t>(window->get_setting_accent())},
        {"density", std::string(window->get_setting_density())},
        {"font_scale", static_cast<std::int64_t>(window->get_setting_font_scale())},
        {"nickname", std::string(window->get_setting_nickname())},
        {"email", std::string(window->get_setting_email())},
        {"cloud_sync", window->get_setting_cloud_sync()},
        {"sidebar_visible", window->get_sidebar_visible()},
        {"code_visible", window->get_code_visible()},
        {"task_expanded", window->get_task_expanded()},
        {"update_channel", std::string(window->get_setting_channel())},
        {"file_access", std::string(window->get_setting_file_access())}}));
    controller.select_provider(std::string(window->get_setting_provider()));
    window->set_model_name(window->get_setting_main_model());
    window->set_effort(window->get_setting_reasoning());
    window->set_settings_status("正在通过 tokmond 原子保存…");
  });
  window->on_reset_settings([window] {
    window->set_settings_status("已恢复默认值；点击“保存更改”后写入");
  });
  window->on_drag_window([] {
#if defined(_WIN32)
    if (const auto hwnd = current_process_window()) {
      ReleaseCapture();
      SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
  });
  window->on_minimize_window([window] {
    window->window().set_minimized(true);
  });
  window->on_toggle_maximize_window([window] {
    const auto maximized = !window->window().is_maximized();
    window->window().set_maximized(maximized);
    window->set_window_maximized(maximized);
  });
  window->on_close_window([] {
    slint::quit_event_loop();
  });
  window->on_refresh_trace([&controller] {
    controller.publish_trace_view();
  });
  window->on_export_trace([&controller] {
    controller.export_trace();
  });
  window->on_settings_searched([window](const slint::SharedString& text) {
    const auto query = display_utf8(std::string_view(text));
    if (query.empty()) return;
    static const std::pair<int, std::vector<std::string>> table[] = {
        {0, {"语言", "启动", "自动保存", "更新通道", "通用"}},
        {1, {"智能体", "模型", "提供方", "推理", "主模型", "协议"}},
        {2, {"文件访问", "命令审批", "网络", "高风险", "权限", "安全"}},
        {3, {"工作区", "索引", "同步", "Git", "git"}},
        {4, {"通知", "桌面", "消息提醒", "免打扰"}},
        {5, {"外观", "主题", "强调色", "密度", "字体"}},
        {6, {"快捷键", "新建会话", "发送消息", "命令面板"}},
        {7, {"账户", "昵称", "邮箱", "方案", "云同步"}},
    };
    for (const auto& [page, keywords] : table) {
      for (const auto& keyword : keywords) {
        if (keyword.find(query) != std::string::npos ||
            query.find(keyword) != std::string::npos) {
          window->set_settings_page(page);
          window->set_settings_status(
              slint::SharedString("已定位到设置页 " + std::to_string(page + 1)));
          return;
        }
      }
    }
    window->set_settings_status(slint::SharedString("未找到匹配的设置项"));
  });

  window->show();
#if defined(_WIN32)
  make_current_process_window_frameless();
#endif
  slint::run_event_loop();
  window->hide();
  return 0;
}
