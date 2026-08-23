#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
  if (content.empty()) content = R"PY(from pathlib import Path
from faster_whisper import WhisperModel

def transcribe(audio: Path, output: Path) -> None:
    model = WhisperModel("large-v3-turbo", device="cuda")
    segments, info = model.transcribe(str(audio), beam_size=5)
    lines: list[str] = []
    for index, segment in enumerate(segments, start=1):
        lines.extend([
            str(index),
            f"{segment.start:.3f} --> {segment.end:.3f}",
            segment.text.strip(),
            "",
        ])
    output.write_text("\n".join(lines), encoding="utf-8")
)PY";
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
    std::string query) {
  for (auto& character : query)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

  const auto matches = [&query](const NavigationItem& item) {
    auto title = std::string(item.title);
    for (auto& character : title)
      character = static_cast<char>(
          std::tolower(static_cast<unsigned char>(character)));
    return title.find(query) != std::string::npos;
  };

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
    if (shown) model->push_back((*items)[row]);
  }
}

class UiSnowController final {
 public:
  UiSnowController(std::filesystem::path endpoint,
                   std::shared_ptr<slint::VectorModel<TimelineItem>> timeline,
                   std::shared_ptr<slint::VectorModel<CodeLine>> code,
                   slint::ComponentWeakHandle<MainWindow> window)
      : endpoint_(std::move(endpoint)), timeline_(std::move(timeline)),
        code_(std::move(code)), window_(std::move(window)),
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
    enqueue(std::move(command));
  }
  void slash_command(std::string text, std::string provider, std::string model,
                     std::string access_mode, std::string effort) {
    Command command{"slash-command", std::move(text)};
    command.payload = tokmon::cbor::object({{"provider", std::move(provider)},
        {"model", std::move(model)}, {"access_mode", std::move(access_mode)},
        {"effort", std::move(effort)}, {"surface", "desktop"}});
    enqueue(std::move(command));
  }
  void snapshot() { enqueue(Command{"snapshot", {}}); }
  void reconcile() { enqueue(Command{"reconcile", {}}); }
  void new_session() { enqueue(Command{"new-session", {}}); }
  void load_settings() { enqueue(Command{"settings-load", {}}); }
  void load_providers() { enqueue(Command{"providers-load", {}}); }
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
    for (const auto& contribution : surface->contributions) {
      if (contribution.channel != "ui.trajectory" ||
          contribution.value.as_array() == nullptr) continue;
      std::vector<tokmon::Photon> result;
      result.reserve(contribution.value.as_array()->size());
      for (const auto& item : *contribution.value.as_array()) {
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
    return {};
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
    auto lines = code_lines_from(photons_);
    auto timeline = timeline_;
    auto code = code_;
    auto window = window_;
    std::string assistant;
    std::string state = "正在沿光路执行";
    for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator) {
      if (assistant.empty() && iterator->kind == "assistant.message") {
        if (const auto* text = tokmon::cbor::find(iterator->payload, "text"))
          assistant = std::string(text->as_string());
      }
      if (iterator->kind == "ray.darkened" || iterator->kind == "act.completed") {
        state = "审阅完成";
        break;
      }
    }
    if (assistant.empty())
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend(); ++iterator)
        if (iterator->kind == "tool.result") {
          assistant = "真实工具已执行：" + tokmon::cbor::diagnostic(iterator->payload);
          break;
        }
    (void)slint::invoke_from_event_loop(
        [timeline, code, window, items = std::move(items), lines = std::move(lines),
         assistant = std::move(assistant), state = std::move(state)]() mutable {
          timeline->clear();
          for (auto& item : items) timeline->push_back(std::move(item));
          code->clear();
          for (auto& line : lines) code->push_back(std::move(line));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            if (!assistant.empty()) handle->set_assistant_text(display_string(assistant));
            handle->set_status_text(display_string(state));
            handle->set_daemon_state("后台服务已连接");
          }
        });
  }

  void apply_settings(tokmon::cbor::Value values) {
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, values = std::move(values)] {
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
      if (auto value = bool_value("autosave")) handle->set_setting_autosave(*value);
      if (auto value = string_value("provider")) handle->set_setting_provider(*value);
      if (auto value = string_value("main_model")) {
        handle->set_setting_main_model(*value); handle->set_model_name(*value);
      }
      if (auto value = string_value("reasoning")) handle->set_setting_reasoning(*value);
      if (auto value = string_value("command_approval")) handle->set_setting_command_approval(*value);
      if (auto value = bool_value("network")) handle->set_setting_network(*value);
      if (auto value = bool_value("high_risk_confirmation")) handle->set_setting_high_risk(*value);
      if (auto value = string_value("workspace")) handle->set_setting_workspace(*value);
      if (auto value = string_value("index_mode")) handle->set_setting_index_mode(*value);
      if (auto value = bool_value("workspace_sync")) handle->set_setting_workspace_sync(*value);
      if (auto value = bool_value("git")) handle->set_setting_git(*value);
      if (auto value = bool_value("notifications")) handle->set_setting_notifications(*value);
      if (auto value = bool_value("desktop_notifications")) handle->set_setting_desktop_notifications(*value);
      if (auto value = bool_value("message_alerts")) handle->set_setting_message_alerts(*value);
      if (auto value = bool_value("quiet_hours")) handle->set_setting_quiet_hours(*value);
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
      handle->set_settings_status("已从项目级 .tokmon/config.yaml 载入");
    });
  }

  void apply_providers(const tokmon::cbor::Value& payload) {
    const auto* selected = tokmon::cbor::find(payload, "default");
    const auto* providers = tokmon::cbor::find(payload, "providers");
    if (!selected || !providers || !providers->as_array()) return;
    tokmon::cbor::Value chosen;
    for (const auto& provider : *providers->as_array())
      if (const auto* id = tokmon::cbor::find(provider, "id");
          id && id->as_string() == selected->as_string()) { chosen = provider; break; }
    if (!chosen.as_map()) return;
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, chosen = std::move(chosen)] {
      auto locked = window.lock();
      if (!locked) return;
      auto handle = *locked;
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
    auto timeline = timeline_; auto code = code_; auto window = window_;
    (void)slint::invoke_from_event_loop([timeline, code, window, display, title, model,
        provider, effort, access, clear, settings, close, copied]() {
      if (clear) { timeline->clear(); code->clear(); }
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
          command = Command{"snapshot", {}};
        }
      }
      if (command.kind == "new-session") {
        active_ray_.clear();
        photons_.clear();
        auto timeline = timeline_; auto code = code_;
        (void)slint::invoke_from_event_loop([timeline, code] {
          timeline->clear(); code->clear();
        });
        continue;
      }
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
        else if (command.kind == "provider-configure") {
          request.payload = tokmon::cbor::object({{"action", "model.provider.configure"}});
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
      tokmon::SnowClient client(endpoint_);
      auto response = client.request(request);
      if (!response) {
        const auto message = response.error().describe();
        if (message != last_error_) { last_error_ = message; show_error(message); }
        continue;
      }
      last_error_.clear();
      update_daemon_state("后台服务已连接");
      if (response->kind == tokmon::SnowMessageKind::error) {
        const auto* message = tokmon::cbor::find(response->payload, "message");
        show_error(message ? std::string(message->as_string()) : "tokmond 拒绝了请求");
        continue;
      }
      cursor_ = std::max(cursor_, response->cursor);
      if (command.kind == "settings-load") {
        if (const auto* values = tokmon::cbor::find(response->payload, "values"))
          apply_settings(*values);
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
      if (command.kind == "provider-configure" || command.kind == "provider-secret") {
        auto window = window_;
        const auto status = command.kind == "provider-configure"
            ? slint::SharedString("平台 YAML 已原子保存并完成热重载")
            : slint::SharedString("API Key 已写入系统凭据库；未进入 YAML/Photon/日志");
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
      auto photons = photons_from(*response);
      if (!photons.empty())
        apply_photons(std::move(photons), response->kind == tokmon::SnowMessageKind::snapshot);
      if (command.kind == "provider-test") {
        auto window = window_;
        (void)slint::invoke_from_event_loop([window] {
          if (auto locked = window.lock())
            (*locked)->set_settings_status("真实 provider 请求已完成；结果已投影到对话与轨迹");
        });
      }
      if ((command.kind == "chat" || command.kind == "provider-test") &&
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
  std::shared_ptr<slint::VectorModel<TimelineItem>> timeline_;
  std::shared_ptr<slint::VectorModel<CodeLine>> code_;
  slint::ComponentWeakHandle<MainWindow> window_;
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Command> commands_;
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
  auto connected = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
      .endpoint = endpoint,
      .workspace = paths->project.parent_path(),
#if defined(_WIN32)
      .executable = executable.parent_path() / "tokmond.exe"
#else
      .executable = executable.parent_path() / "tokmond"
#endif
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

  const std::vector<NavigationItem> navigation = [&assets] {
    std::vector<NavigationItem> items;
    const auto add = [&items, &assets](const char* title, const char* icon, int indent,
                              bool selected, bool expandable) {
      NavigationItem item; item.title = title; item.indent = indent;
      item.selected = selected; item.expandable = expandable;
      item.expanded = true;
      item.icon = slint::Image::load_from_path(
          slint::SharedString((assets / icon).string()));
      items.push_back(std::move(item));
    };
    add("内容生产", "icon-06.svg", 0, false, true);
    add("字幕制作空间", "icon-08.svg", 1, false, true);
    add("生成音频时间轴字幕", "icon-09.svg", 2, true, false);
    add("字幕校对优化", "icon-10.svg", 2, false, false);
    add("批量字幕质检优化", "icon-10.svg", 2, false, false);
    add("音频切片处理", "icon-12.svg", 1, false, true);
    add("演示助手", "icon-13.svg", 0, false, true);
    add("PPT 智绘项目", "icon-16.svg", 1, false, true);
    add("PPT 大纲生成", "icon-17.svg", 2, false, false);
    add("演讲稿润色", "icon-18.svg", 2, false, false);
    add("旅行计划", "icon-19.svg", 0, false, true);
    return items;
  }();
  auto navigation_state =
      std::make_shared<std::vector<NavigationItem>>(navigation);
  auto nav_model = std::make_shared<slint::VectorModel<NavigationItem>>();
  refresh_navigation(nav_model, navigation_state, {});
  auto timeline_model = std::make_shared<slint::VectorModel<TimelineItem>>();
  auto code_model = std::make_shared<slint::VectorModel<CodeLine>>();
  auto slash_model = std::make_shared<slint::VectorModel<SlashCommandItem>>();
  for (auto& line : code_lines_from({})) code_model->push_back(std::move(line));
  window->set_navigation(nav_model);
  window->set_timeline(timeline_model);
  window->set_code_lines(code_model);
  window->set_slash_commands(slash_model);
  window->set_setting_workspace(
      slint::SharedString(paths->project.parent_path().generic_string()));
  window->set_daemon_state(connected->started ? "后台服务已自动启动" : "后台服务已连接");
  UiSnowController controller(endpoint, timeline_model, code_model,
                              slint::ComponentWeakHandle<MainWindow>(window));
  controller.snapshot();
  controller.load_settings();
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
  window->on_send_message([&controller, timeline_model, window](const slint::SharedString& text) {
    window->set_slash_menu_visible(false);
    TimelineItem item;
    item.time = time_label(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count());
    item.kind = "task";
    item.title = slint::SharedString(
        std::string("已提交后续请求: ") + std::string(text));
    item.detail = "";
    item.tone = "warning";
    item.progress = -1;
    timeline_model->push_back(std::move(item));
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
  window->on_new_session([&controller] { controller.new_session(); });
  window->on_select_navigation([nav_model, navigation_state, window](int index) {
    if (index < 0 || index >= static_cast<int>(nav_model->row_count())) return;
    const auto clicked = *nav_model->row_data(index);
    for (auto& item : *navigation_state) {
      if (item.title != clicked.title || item.indent != clicked.indent) continue;
      if (item.expandable) item.expanded = !item.expanded;
      else {
        for (auto& candidate : *navigation_state) candidate.selected = false;
        item.selected = true;
      }
      break;
    }
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()));
  });
  window->on_search_changed([nav_model, navigation_state](const slint::SharedString& text) {
    refresh_navigation(nav_model, navigation_state, std::string(text));
  });
  window->on_add_navigation([nav_model, navigation_state, assets, window] {
    NavigationItem item;
    item.title = slint::SharedString(
        "新建项目 " + std::to_string(navigation_state->size() + 1));
    item.icon = slint::Image::load_from_path(
        slint::SharedString((assets / "icon-06.svg").string()));
    item.indent = 0;
    item.selected = false;
    item.expandable = true;
    item.expanded = true;
    navigation_state->push_back(std::move(item));
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()));
  });
  window->on_reconcile([&controller] { controller.reconcile(); });
  window->on_configure_provider([&controller](const slint::SharedString& id,
      const slint::SharedString& protocol, const slint::SharedString& endpoint,
      const slint::SharedString& model, const slint::SharedString& auth, bool thinking) {
    controller.configure_provider(tokmon::cbor::object({
        {"id", std::string(id)}, {"protocol", std::string(protocol)},
        {"endpoint", std::string(endpoint)}, {"model", std::string(model)},
        {"auth", std::string(auth)}, {"thinking", thinking}, {"default", true},
        {"max_output_tokens", 4096}, {"max_attempts", 2},
        {"retry_backoff_ms", 250}}));
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
        {"autosave", window->get_setting_autosave()},
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
        {"quiet_hours", window->get_setting_quiet_hours()},
        {"dark_theme", window->get_setting_dark_theme()},
        {"accent", static_cast<std::int64_t>(window->get_setting_accent())},
        {"density", std::string(window->get_setting_density())},
        {"font_scale", static_cast<std::int64_t>(window->get_setting_font_scale())},
        {"nickname", std::string(window->get_setting_nickname())},
        {"email", std::string(window->get_setting_email())},
        {"cloud_sync", window->get_setting_cloud_sync()},
        {"sidebar_visible", window->get_sidebar_visible()},
        {"code_visible", window->get_code_visible()},
        {"task_expanded", window->get_task_expanded()}}));
    window->set_model_name(window->get_setting_main_model());
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
#if defined(_WIN32)
    if (const auto hwnd = current_process_window()) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      return;
    }
#endif
    slint::quit_event_loop();
  });

  window->run();
  (void)client_lease->detach();
  return 0;
}
