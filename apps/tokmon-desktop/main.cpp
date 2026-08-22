#include <chrono>
#include <atomic>
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

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

namespace {

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

std::vector<CodeLine> sample_code() {
  const std::vector<std::pair<std::string, std::string>> lines = {
      {"import os", "keyword"}, {"import json", "keyword"},
      {"from pathlib import Path", "keyword"},
      {"from faster_whisper import WhisperModel", "keyword"}, {"", "normal"},
      {"def transcribe_audio(model_path: str, audio_path: str,", "keyword"},
      {"        output_srt: str, language: str = \"zh\",", "string"},
      {"        beam_size: int = 5, vad_filter: bool = True) -> dict:", "keyword"},
      {"    \"\"\"使用 faster-whisper 进行音频转录并输出 SRT。\"\"\"", "string"},
      {"    model = WhisperModel(model_path, device=\"auto\",", "normal"},
      {"                         compute_type=\"int8\")", "string"}, {"", "normal"},
      {"    segments, info = model.transcribe(", "normal"},
      {"        audio_path,", "normal"}, {"        language=language,", "normal"},
      {"        beam_size=beam_size,", "normal"}, {"        vad_filter=vad_filter,", "normal"},
      {"        vad_parameters=dict(min_silence_duration_ms=400),", "keyword"},
      {"        word_timestamps=True,", "keyword"}, {"    )", "normal"}, {"", "normal"},
      {"    results = []", "normal"},
      {"    for i, seg in enumerate(segments, start=1):", "keyword"},
      {"        results.append({", "normal"}, {"            \"index\": i,", "string"},
      {"            \"start\": round(seg.start, 2),", "string"},
      {"            \"end\": round(seg.end, 2),", "string"},
      {"            \"text\": seg.text.strip(),", "string"}, {"        })", "normal"},
      {"", "normal"}, {"    # 写入 SRT 文件 (UTF-8)", "comment"},
      {"    Path(output_srt).write_text(to_srt(results), encoding=\"utf-8\")", "string"},
      {"    return {\"segments\": len(results), \"language\": info.language}", "keyword"}};
  std::vector<CodeLine> result;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u); line.text = lines[index].first;
    line.tone = lines[index].second; result.push_back(std::move(line));
  }
  return result;
}

class UiSnowController final {
 public:
  UiSnowController(std::filesystem::path endpoint,
                   std::shared_ptr<slint::VectorModel<TimelineItem>> timeline)
      : endpoint_(std::move(endpoint)), timeline_(std::move(timeline)),
        worker_([this](std::stop_token stop) { run(stop); }) {}

  ~UiSnowController() {
    worker_.request_stop();
    condition_.notify_all();
  }

  void chat(std::string text) { enqueue(Command{"chat", std::move(text)}); }
  void snapshot() { enqueue(Command{"snapshot", {}}); }
  void reconcile() { enqueue(Command{"reconcile", {}}); }

 private:
  struct Command { std::string kind; std::string text; };

  void enqueue(Command command) {
    {
      std::scoped_lock lock(mutex_);
      commands_.push_back(std::move(command));
    }
    condition_.notify_one();
  }

  std::vector<TimelineItem> items_from(const tokmon::SnowMessage& response) {
    std::vector<TimelineItem> items;
    const auto* field = tokmon::cbor::find(response.payload, "photons");
    if (field == nullptr || field->as_array() == nullptr) return items;
    for (const auto& encoded : *field->as_array()) {
      auto photon = tokmon::photon_from_cbor(encoded);
      if (photon) items.push_back(timeline_item(*photon));
    }
    return items;
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
        condition_.wait(lock, stop, [this] { return !commands_.empty(); });
        if (stop.stop_requested()) return;
        command = std::move(commands_.front()); commands_.pop_front();
      }
      tokmon::SnowMessage request;
      request.request_id = next_request_.fetch_add(1, std::memory_order_relaxed);
      if (command.kind == "snapshot") {
        request.kind = tokmon::SnowMessageKind::snapshot_request;
      } else {
        request.kind = tokmon::SnowMessageKind::intent;
        request.payload = command.kind == "chat"
            ? tokmon::cbor::object({{"action", "chat"}, {"text", command.text}})
            : tokmon::cbor::object({{"action", "lens.reconcile"}});
      }
      tokmon::SnowClient client(endpoint_);
      auto response = client.request(request);
      if (!response) { show_error(response.error().describe()); continue; }
      if (response->kind == tokmon::SnowMessageKind::error) {
        const auto* message = tokmon::cbor::find(response->payload, "message");
        show_error(message ? std::string(message->as_string()) : "tokmond 拒绝了请求");
        continue;
      }
      if (command.kind == "reconcile") continue;
      auto items = items_from(*response);
      auto model = timeline_;
      (void)slint::invoke_from_event_loop([model, items = std::move(items)]() mutable {
        model->clear();
        for (const auto& item : items) model->push_back(item);
      });
    }
  }

  std::filesystem::path endpoint_;
  std::shared_ptr<slint::VectorModel<TimelineItem>> timeline_;
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<Command> commands_;
  std::atomic_uint64_t next_request_{1};
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
  auto nav_model = std::make_shared<slint::VectorModel<NavigationItem>>(navigation);
  auto timeline_model = std::make_shared<slint::VectorModel<TimelineItem>>();
  auto code_model = std::make_shared<slint::VectorModel<CodeLine>>(sample_code());
  window->set_navigation(nav_model);
  window->set_timeline(timeline_model);
  window->set_code_lines(code_model);
  UiSnowController controller(tokmon::default_snow_endpoint(paths->run), timeline_model);
  controller.snapshot();
  window->on_send_message([&controller](const slint::SharedString& text) {
    controller.chat(std::string(text));
  });
  window->on_new_session([timeline_model] { timeline_model->clear(); });
  window->on_select_navigation([nav_model](int index) {
    for (int row = 0; row < static_cast<int>(nav_model->row_count()); ++row) {
      auto item = *nav_model->row_data(row); item.selected = row == index;
      nav_model->set_row_data(row, item);
    }
  });
  window->on_reconcile([&controller] { controller.reconcile(); });
  return window->run();
}
