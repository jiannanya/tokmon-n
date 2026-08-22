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

#if defined(_WIN32)
HWND current_process_window() {
  struct Search {
    DWORD process_id;
    HWND window;
  } search{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND candidate, LPARAM context) -> BOOL {
        auto* search = reinterpret_cast<Search*>(context);
        DWORD process_id = 0;
        GetWindowThreadProcessId(candidate, &process_id);
        if (process_id != search->process_id || !IsWindowVisible(candidate))
          return TRUE;
        search->window = candidate;
        return FALSE;
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
  item.kind = photon.kind;
  item.title = photon.kind;
  item.detail = tokmon::cbor::diagnostic(photon.payload);
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
  std::vector<CodeLine> result;
  std::istringstream input(content);
  std::string text;
  for (std::size_t index = 0; std::getline(input, text) && index < 20'000u; ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u);
    line.text = text;
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
                   std::shared_ptr<slint::VectorModel<CodeLine>> code)
      : endpoint_(std::move(endpoint)), timeline_(std::move(timeline)),
        code_(std::move(code)),
        worker_([this](std::stop_token stop) { run(stop); }) {}

  ~UiSnowController() {
    worker_.request_stop();
    condition_.notify_all();
  }

  void chat(std::string text) { enqueue(Command{"chat", std::move(text)}); }
  void snapshot() { enqueue(Command{"snapshot", {}}); }
  void reconcile() { enqueue(Command{"reconcile", {}}); }
  void new_session() { enqueue(Command{"new-session", {}}); }

 private:
  struct Command { std::string kind; std::string text; };

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
    (void)slint::invoke_from_event_loop(
        [timeline, code, items = std::move(items), lines = std::move(lines)]() mutable {
          timeline->clear();
          for (auto& item : items) timeline->push_back(std::move(item));
          code->clear();
          for (auto& line : lines) code->push_back(std::move(line));
        });
  }

  void show_error(std::string message) {
    TimelineItem item;
    item.time = "now"; item.kind = "snow.error";
    item.title = "tokmond 连接失败"; item.detail = std::move(message);
    item.progress = -1; item.tone = "danger";
    auto model = timeline_;
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
      request.request_id = next_request_.fetch_add(1, std::memory_order_relaxed);
      if (command.kind == "snapshot") {
        request.kind = tokmon::SnowMessageKind::snapshot_request;
        request.cursor = cursor_;
      } else {
        request.kind = tokmon::SnowMessageKind::intent;
        request.payload = command.kind == "chat"
            ? tokmon::cbor::object({{"action", "chat"}, {"text", command.text},
                                     {"ray", active_ray_}})
            : tokmon::cbor::object({{"action", "lens.reconcile"}});
      }
      tokmon::SnowClient client(endpoint_);
      auto response = client.request(request);
      if (!response) {
        const auto message = response.error().describe();
        if (message != last_error_) { last_error_ = message; show_error(message); }
        continue;
      }
      last_error_.clear();
      if (response->kind == tokmon::SnowMessageKind::error) {
        const auto* message = tokmon::cbor::find(response->payload, "message");
        show_error(message ? std::string(message->as_string()) : "tokmond 拒绝了请求");
        continue;
      }
      cursor_ = std::max(cursor_, response->cursor);
      if (command.kind == "reconcile") continue;
      if (command.kind == "chat")
        if (const auto* ray = tokmon::cbor::find(response->payload, "ray"))
          active_ray_ = std::string(ray->as_string());
      auto photons = photons_from(*response);
      if (!photons.empty())
        apply_photons(std::move(photons), response->kind == tokmon::SnowMessageKind::snapshot);
      if (command.kind == "chat" && !active_ray_.empty()) {
        tokmon::SnowMessage surface_request;
        surface_request.kind = tokmon::SnowMessageKind::intent;
        surface_request.request_id = next_request_.fetch_add(1, std::memory_order_relaxed);
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
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Command> commands_;
  std::atomic_uint64_t next_request_{1};
  std::uint64_t cursor_{0};
  tokmon::RayId active_ray_;
  std::vector<tokmon::Photon> photons_;
  std::string last_error_;
  std::jthread worker_;
};

}  // namespace

int main(int argc, char** argv) {
  auto paths = tokmon::resolve_paths(std::nullopt);
  if (!paths) return 2;
  auto window = MainWindow::create();

  std::error_code path_error;
  auto executable = argc > 0 ? std::filesystem::absolute(argv[0], path_error)
                             : std::filesystem::current_path();
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
  window->set_navigation(nav_model);
  window->set_timeline(timeline_model);
  window->set_code_lines(code_model);
  UiSnowController controller(tokmon::default_snow_endpoint(paths->run), timeline_model,
                              code_model);
  controller.snapshot();
  window->on_send_message([&controller, timeline_model](const slint::SharedString& text) {
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
    controller.chat(std::string(text));
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

  return window->run();
}
