#include "ui_controller.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "navigation_state.hpp"
#include "platform_utils.hpp"
#include "ui_projection.hpp"

namespace tokmon::desktop {
namespace {

class UiControllerImpl final : public UiController {
public:
  UiControllerImpl(
      std::filesystem::path endpoint, std::filesystem::path workspace,
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
        current_workspace_(workspace),
        navigation_workspace_(std::move(workspace)),
        daemon_executable_(std::move(daemon_executable)),
        timeline_(std::move(timeline)),
        conversation_workflow_(std::move(conversation_workflow)),
        code_(std::move(code)), trace_events_(std::move(trace_events)),
        gantt_(std::move(gantt)),
        navigation_model_(std::move(navigation_model)),
        navigation_(std::move(navigation)), assets_(std::move(assets)),
        window_(std::move(window)),
        restore_initial_workspace_(restore_initial_workspace),
        worker_([this](std::stop_token stop) { run(stop); }) {}

  ~UiControllerImpl() {
    worker_.request_stop();
    condition_.notify_all();
  }

  void chat(std::string text, std::string provider, std::string model,
            std::string access_mode, std::string effort) {
    Command command{"chat", std::move(text)};
    command.payload =
        tokmon::cbor::object({{"provider", std::move(provider)},
                              {"model", std::move(model)},
                              {"access_mode", std::move(access_mode)},
                              {"effort", std::move(effort)}});
    enqueue_user(std::move(command));
  }
  void slash_command(std::string text, std::string provider, std::string model,
                     std::string access_mode, std::string effort) {
    Command command{"slash-command", std::move(text)};
    command.payload =
        tokmon::cbor::object({{"provider", std::move(provider)},
                              {"model", std::move(model)},
                              {"access_mode", std::move(access_mode)},
                              {"effort", std::move(effort)},
                              {"surface", "desktop"}});
    enqueue_user(std::move(command));
  }
  void rename_session(std::string title) {
    enqueue_user(Command{"rename-session", std::move(title)});
  }
  void snapshot() { enqueue(Command{"snapshot", {}}); }
  void reconcile() { enqueue(Command{"reconcile", {}}); }
  void refresh_workspace() { enqueue(Command{"workspace-refresh", {}}); }
  void new_session(std::string workspace = {}) {
    Command command{"new-session", {}};
    command.payload =
        tokmon::cbor::object({{"workspace", std::move(workspace)}});
    enqueue(std::move(command));
  }
  void open_session(std::string ray, std::string workspace = {}) {
    Command command{"open-session", std::move(ray)};
    command.payload =
        tokmon::cbor::object({{"workspace", std::move(workspace)}});
    enqueue(std::move(command));
  }
  void switch_workspace(std::string workspace) {
    enqueue(Command{"switch-workspace", std::move(workspace)});
  }
  void load_settings(const bool include_navigation = false) {
    Command command{"settings-load", {}};
    command.payload =
        tokmon::cbor::object({{"include_navigation", include_navigation}});
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
    (void)slint::invoke_from_event_loop([window, trace_events, gantt,
                                         photons]() mutable {
      auto locked = window.lock();
      if (!locked)
        return;
      auto handle = *locked;
      const auto search =
          display_utf8(std::string_view(handle->get_trace_search()));
      const auto filter_index = handle->get_trace_filter_index();
      const auto page = std::max(1, handle->get_trace_page());
      const auto page_size = std::max(1, handle->get_trace_page_size());

      struct Entry {
        TraceEvent event;
        std::int64_t time;
        std::string kind_l;
      };
      std::vector<Entry> all;
      all.reserve(photons.size());
      int num = 0;
      for (const auto &photon : photons) {
        ++num;
        TraceEvent ev;
        ev.num = num;
        ev.time = time_label(photon.committed_at_ms);
        const auto kind = std::string(photon.kind);
        const auto act_kind = kind.starts_with("act.")
                                  ? act_field(photon, "kind")
                                  : std::string{};
        const auto tool_event =
            kind == "model.tool-call" || kind == "tool.result" ||
            kind.starts_with("fs.") || kind.starts_with("process.") ||
            (kind.starts_with("act.") && act_kind != "model.call");
        const auto model_event =
            kind.starts_with("model.") ||
            (kind.starts_with("act.") && act_kind == "model.call");
        ev.tone = display_string(
            kind == "user.input" || kind == "user.message" ? "USER"
            : kind.find("context") != std::string::npos ||
                    kind == "system.prompt"
                ? "CONTEXT"
            : kind == "assistant.message" || kind == "model.completed"
                ? "ASSISTANT"
            : kind.find("failed") != std::string::npos ||
                    kind.find("rejected") != std::string::npos
                ? "ERROR"
            : tool_event  ? "TOOL"
            : model_event ? "MODEL"
                          : "LENS");
        ev.role =
            display_string(std::string(ev.tone) == "USER"        ? "User"
                           : std::string(ev.tone) == "CONTEXT"   ? "System"
                           : std::string(ev.tone) == "ASSISTANT" ? "Assistant"
                           : std::string(ev.tone) == "MODEL"     ? "Model"
                           : std::string(ev.tone) == "TOOL"      ? "Tool"
                           : std::string(ev.tone) == "LENS"      ? "Lens"
                                                                 : "-");
        ev.title = display_string(photon.kind);
        ev.detail = display_string(bounded_detail(
            kind + " · " + tokmon::cbor::diagnostic(photon.payload), 120));
        if (const auto *dur = tokmon::cbor::find(photon.payload, "duration_ms"))
          ev.dur =
              slint::SharedString(std::to_string(dur->as_integer()) + "ms");
        else
          ev.dur = "-";
        if (photon.kind == "model.usage") {
          std::int64_t total = 0;
          if (const auto *v =
                  tokmon::cbor::find(photon.payload, "input_tokens"))
            total += v->as_integer();
          if (const auto *v =
                  tokmon::cbor::find(photon.payload, "output_tokens"))
            total += v->as_integer();
          ev.tokens = slint::SharedString(std::to_string(total));
        } else {
          ev.tokens = "-";
        }
        std::string hay = kind + std::string(ev.detail);
        for (auto &ch : hay)
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        bool matches_search =
            search.empty() || hay.find(search) != std::string::npos;
        const auto tone_str = std::string(ev.tone);
        bool matches_filter = filter_index == 0 ||
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
      for (auto &e : page_events)
        trace_events->push_back(std::move(e));

      // Gantt segments from real timestamps
      gantt->clear();
      if (!photons.empty()) {
        const auto t0 = photons.front().committed_at_ms;
        const auto t1 = std::max(t0 + 1, photons.back().committed_at_ms);
        const double span = static_cast<double>(t1 - t0);
        constexpr double min_frac = 0.02;
        int count = 0;
        for (const auto &photon : photons) {
          if (++count > 200)
            break;
          const double start = std::clamp(
              static_cast<double>(photon.committed_at_ms - t0) / span, 0.0,
              0.98);
          const auto kind = std::string(photon.kind);
          const auto act_kind = kind.starts_with("act.")
                                    ? act_field(photon, "kind")
                                    : std::string{};
          int row = 0;
          slint::Color tint = slint::Color::from_rgb_uint8(0x6B, 0x72, 0x80);
          if (kind == "user.input" || kind == "user.message") {
            row = 0;
            tint = slint::Color::from_rgb_uint8(0x6B, 0x72, 0x80);
          } else if (kind.find("context") != std::string::npos ||
                     kind == "system.prompt") {
            row = 0;
            tint = slint::Color::from_rgb_uint8(0x3B, 0x82, 0xF6);
          } else if (kind == "assistant.message" || kind == "model.completed") {
            row = 0;
            tint = slint::Color::from_rgb_uint8(0x22, 0xC5, 0x5E);
          } else if (kind == "tool.result" || kind.starts_with("fs.") ||
                     kind.starts_with("process.") ||
                     (kind.starts_with("act.") && act_kind != "model.call")) {
            row = 2;
            tint = slint::Color::from_rgb_uint8(0xF9, 0x73, 0x16);
          } else if (kind.starts_with("model.")) {
            row = 1;
            tint = slint::Color::from_rgb_uint8(0xA8, 0x55, 0xF7);
          } else if (kind.starts_with("act.") && act_kind == "model.call") {
            row = 1;
            tint = slint::Color::from_rgb_uint8(0xA8, 0x55, 0xF7);
          }
          GanttSegment seg;
          seg.row = row;
          seg.start = static_cast<float>(start);
          seg.span = static_cast<float>(
              std::max(min_frac, 1.0 / std::max(1.0, span / 1000)));
          seg.span = static_cast<float>(
              std::min(0.98 - start, static_cast<double>(seg.span)));
          seg.tint = tint;
          gantt->push_back(seg);
        }
      }

      // Time ticks
      if (!photons.empty()) {
        const auto total_s =
            (photons.back().committed_at_ms - photons.front().committed_at_ms) /
            1000;
        std::vector<slint::SharedString> ticks;
        for (int i = 0; i < 7; ++i) {
          const auto sec = total_s * i / 6;
          if (sec < 60)
            ticks.push_back(slint::SharedString(std::to_string(sec) + "s"));
          else
            ticks.push_back(
                slint::SharedString(std::to_string(sec / 60) + "m " +
                                    std::to_string(sec % 60) + "s"));
        }
        auto tick_model =
            std::make_shared<slint::VectorModel<slint::SharedString>>(ticks);
        handle->set_trace_ticks(tick_model);
      }

      // Token labels with thousands separators
      auto group_digits = [](std::int64_t value) {
        auto str = std::to_string(value);
        std::string out;
        int pos = 0;
        for (auto it = str.rbegin(); it != str.rend(); ++it) {
          if (pos > 0 && pos % 3 == 0)
            out += ',';
          out += *it;
          ++pos;
        }
        return std::string(out.rbegin(), out.rend());
      };
      std::int64_t in_toks = 0;
      std::int64_t out_toks = 0;
      for (const auto &photon : photons) {
        if (photon.kind != "model.usage")
          continue;
        if (const auto *value =
                tokmon::cbor::find(photon.payload, "input_tokens"))
          in_toks += value->as_integer();
        if (const auto *value =
                tokmon::cbor::find(photon.payload, "output_tokens"))
          out_toks += value->as_integer();
      }
      const auto tot = in_toks + out_toks;
      handle->set_trace_total_label(display_string(group_digits(tot)));
      if (tot > 0) {
        handle->set_trace_prompt_label(
            display_string(group_digits(in_toks) + " (" +
                           std::to_string(in_toks * 100 / tot) + "%)"));
        handle->set_trace_completion_label(
            display_string(group_digits(out_toks) + " (" +
                           std::to_string(out_toks * 100 / tot) + "%)"));
      }

      // Workflow counters
      int explored = 0, ran = 0;
      for (const auto &p : photons) {
        if (p.kind == "fs.read-completed" || p.kind == "fs.changed" ||
            p.kind == "artifact.previewed")
          ++explored;
        if (p.kind == "process.exited" || p.kind == "tool.result")
          ++ran;
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
      if (!locked)
        return;
      auto handle = *locked;
      const auto search =
          display_utf8(std::string_view(handle->get_trace_search()));
      const auto filter_index = handle->get_trace_filter_index();
      try {
        auto dir = workspace / "exports";
        std::filesystem::create_directories(dir);
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
        auto file = dir / ("tokmon-trace-" + std::to_string(ms) + ".json");
        std::ofstream out(file, std::ios::binary);
        if (!out) {
          handle->set_settings_status("导出失败：无法写入文件");
          return;
        }
        out << "{\"events\":[";
        int num = 0;
        bool first = true;
        for (const auto &photon : photons) {
          ++num;
          const auto kind = std::string(photon.kind);
          std::string tone =
              kind == "user.input" || kind == "user.message" ? "USER"
              : kind.find("context") != std::string::npos    ? "CONTEXT"
              : kind == "assistant.message" || kind == "model.completed"
                  ? "ASSISTANT"
              : kind.find("failed") != std::string::npos ? "ERROR"
              : kind == "model.tool-call" || kind.starts_with("fs.") ||
                      kind.starts_with("process.") || kind == "tool.result"
                  ? "TOOL"
                  : "OTHER";
          bool match = filter_index == 0 ||
                       (filter_index == 1 && tone == "USER") ||
                       (filter_index == 2 && tone == "CONTEXT") ||
                       (filter_index == 3 && tone == "ASSISTANT") ||
                       (filter_index == 4 && tone == "TOOL");
          if (!match)
            continue;
          auto detail = tokmon::cbor::diagnostic(photon.payload);
          for (auto &ch : detail)
            if (ch == '"' || ch == '\\')
              ch = ' ';
          if (!first)
            out << ",";
          first = false;
          out << "\n{\"num\":" << num << ",\"kind\":\"" << kind
              << "\",\"tone\":\"" << tone
              << "\",\"time\":" << photon.committed_at_ms << ",\"detail\":\""
              << detail << "\"}";
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

  [[nodiscard]] const std::filesystem::path &
  current_workspace() const noexcept {
    return current_workspace_;
  }

  void select_session_file(const int index) {
    std::string name;
    std::string content;
    int added_lines = 0;
    std::vector<CodeLine> preview;
    {
      std::scoped_lock lock(files_mutex_);
      if (index < 0 || index >= static_cast<int>(session_files_.size()))
        return;
      const auto &chosen = session_files_[static_cast<std::size_t>(index)];
      name = chosen.name;
      content = chosen.content;
      added_lines =
          chosen.written
              ? static_cast<int>(std::count(chosen.content.begin(),
                                            chosen.content.end(), '\n')) +
                    1
              : 0;
      full_preview_lines_ = code_lines_from_text(content);
      preview = full_preview_lines_;
    }
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, preview = std::move(preview),
                                         name, content, added_lines]() mutable {
      auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
      for (auto &line : preview)
        preview_model->push_back(std::move(line));
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
    for (auto &character : query)
      character = static_cast<char>(
          std::tolower(static_cast<unsigned char>(character)));
    std::vector<CodeLine> preview;
    {
      std::scoped_lock lock(files_mutex_);
      for (const auto &line : full_preview_lines_) {
        if (query.empty()) {
          preview.push_back(line);
          continue;
        }
        auto text = std::string(line.text);
        for (auto &character : text)
          character = static_cast<char>(
              std::tolower(static_cast<unsigned char>(character)));
        if (text.find(query) != std::string::npos)
          preview.push_back(line);
      }
    }
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [window, preview = std::move(preview)]() mutable {
          auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
          for (auto &line : preview)
            preview_model->push_back(std::move(line));
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
    std::vector<std::filesystem::path> preset_workspaces;
    for (std::size_t index = 0; index < navigation_->size(); ++index) {
      const auto &item = (*navigation_)[index];
      if (std::string(item.kind) == "group") {
        bool has_projects = false;
        for (auto next = index + 1; next < navigation_->size() &&
                                    (*navigation_)[next].indent > item.indent;
             ++next)
          if (std::string((*navigation_)[next].kind) == "project")
            has_projects = true;
        if (has_projects)
          groups.push_back(item.title);
      }
      if (std::string(item.kind) == "project" &&
          !std::string(item.workspace).empty()) {
        const auto preset_workspace =
            path_from_utf8(std::string(item.workspace));
        if (std::ranges::any_of(
                preset_workspaces, [&preset_workspace](const auto &known) {
                  return same_workspace(known, preset_workspace);
                }))
          continue;
        preset_workspaces.push_back(preset_workspace);
        ProjectFile preset;
        preset.name = item.title;
        preset.path = display_string(std::string(item.workspace));
        for (auto previous = index; previous > 0;) {
          --previous;
          if ((*navigation_)[previous].indent >= item.indent)
            continue;
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
            for (auto &group : groups)
              group_model->push_back(std::move(group));
            handle->set_group_options(group_model);
            auto preset_model =
                std::make_shared<slint::VectorModel<ProjectFile>>();
            for (auto &preset : presets)
              preset_model->push_back(std::move(preset));
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
      if (!initializing_)
        return;
      initializing_ = false;
      while (!deferred_user_commands_.empty()) {
        commands_.push_back(std::move(deferred_user_commands_.front()));
        deferred_user_commands_.pop_front();
      }
    }
    condition_.notify_one();
  }

  void publish_pending(const std::string &text) {
    TimelineItem item;
    item.time =
        time_label(std::chrono::duration_cast<std::chrono::milliseconds>(
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
    if (active_ray_.empty())
      return;
    const auto ray = active_ray_;
    auto navigation = navigation_;
    auto navigation_model = navigation_model_;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [this, ray, navigation, navigation_model, window] {
          bool changed = false;
          for (auto &item : *navigation) {
            if (!item.selected || item.kind != "session")
              continue;
            if (std::string(item.ray) != ray) {
              item.ray = display_string(ray);
              changed = true;
            }
            break;
          }
          if (!changed)
            return;
          auto query = std::string{};
          if (auto locked = window.lock())
            query = std::string((*locked)->get_search_text());
          refresh_navigation(navigation_model, navigation, std::move(query));
          save_navigation();
        });
  }

  std::vector<tokmon::Photon>
  photons_from(const tokmon::SnowMessage &response) {
    std::vector<tokmon::Photon> result;
    const auto *field = tokmon::cbor::find(response.payload, "photons");
    if (field == nullptr || field->as_array() == nullptr)
      return result;
    for (const auto &encoded : *field->as_array()) {
      auto photon = tokmon::photon_from_cbor(encoded);
      if (photon)
        result.push_back(std::move(*photon));
    }
    return result;
  }

  std::vector<tokmon::Photon>
  photons_from_surface(const tokmon::SnowMessage &response) const {
    const auto *encoded = tokmon::cbor::find(response.payload, "surface");
    if (!encoded)
      return {};
    auto surface = tokmon::surface_from_cbor(*encoded);
    if (!surface)
      return {};
    const tokmon::SurfaceContribution *selected = nullptr;
    for (const auto &contribution : surface->contributions) {
      if (contribution.channel != "ui.trajectory" ||
          contribution.value.as_array() == nullptr)
        continue;
      if (!selected || contribution.priority > selected->priority)
        selected = &contribution;
    }
    if (!selected)
      return {};
    std::vector<tokmon::Photon> result;
    result.reserve(selected->value.as_array()->size());
    for (const auto &item : *selected->value.as_array()) {
      tokmon::Photon photon;
      photon.sequence = static_cast<std::uint64_t>(
          tokmon::cbor::find(item, "sequence")
              ? tokmon::cbor::find(item, "sequence")->as_integer()
              : 0);
      photon.id = tokmon::cbor::find(item, "id")
                      ? std::string(tokmon::cbor::find(item, "id")->as_string())
                      : std::string{};
      photon.ray = active_ray_;
      photon.kind =
          tokmon::cbor::find(item, "kind")
              ? std::string(tokmon::cbor::find(item, "kind")->as_string())
              : std::string{};
      photon.schema =
          tokmon::cbor::find(item, "schema")
              ? std::string(tokmon::cbor::find(item, "schema")->as_string())
              : std::string{};
      if (const auto *payload = tokmon::cbor::find(item, "payload"))
        photon.payload = *payload;
      photon.committed_at_ms =
          tokmon::cbor::find(item, "time")
              ? tokmon::cbor::find(item, "time")->as_integer()
              : 0;
      photon.caused_by_act =
          tokmon::cbor::find(item, "caused_by_act")
              ? std::string(
                    tokmon::cbor::find(item, "caused_by_act")->as_string())
              : std::string{};
      if (!photon.id.empty() && !photon.kind.empty())
        result.push_back(std::move(photon));
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
      for (const auto &file : session_files_)
        files.push_back(ProjectFile{
            display_string(file.name), display_string(file.path), {}});
      if (!session_files_.empty()) {
        const auto &chosen = session_files_.front();
        selected_name = chosen.name;
        selected_content = chosen.content;
        added_lines =
            chosen.written
                ? static_cast<int>(std::count(chosen.content.begin(),
                                              chosen.content.end(), '\n')) +
                      1
                : 0;
      }
      full_preview_lines_ = code_lines_from_text(selected_content);
      preview = full_preview_lines_;
    }
    (void)slint::invoke_from_event_loop(
        [window, files = std::move(files), preview = std::move(preview),
         selected_name, selected_content, added_lines, select_first]() mutable {
          auto files_model =
              std::make_shared<slint::VectorModel<ProjectFile>>();
          for (auto &file : files)
            files_model->push_back(std::move(file));
          auto preview_model = std::make_shared<slint::VectorModel<CodeLine>>();
          for (auto &line : preview)
            preview_model->push_back(std::move(line));
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
    if (replace)
      photons_.clear();
    for (auto &photon : incoming) {
      const auto found =
          std::ranges::find(photons_, photon.id, &tokmon::Photon::id);
      if (found == photons_.end())
        photons_.push_back(std::move(photon));
      else
        *found = std::move(photon);
    }
    std::ranges::sort(photons_, {}, &tokmon::Photon::sequence);
    std::vector<TimelineItem> items;
    items.reserve(photons_.size());
    for (const auto &photon : photons_)
      items.push_back(timeline_item(photon));
    auto workflow_items = conversation_workflow_from(photons_);
    const bool workflow_complete =
        std::ranges::any_of(workflow_items, [](const TimelineItem &item) {
          return std::string(item.kind) == "task.completed";
        });
    const auto trace = trace_summary_from(photons_);
    auto lines = code_lines_from(photons_);
    {
      std::scoped_lock lock(files_mutex_);
      session_files_.clear();
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend();
           ++iterator) {
        const auto kind = std::string(iterator->kind);
        if (!kind.starts_with("fs.") && kind != "artifact.previewed")
          continue;
        auto file = session_file_from_photon(*iterator);
        if (file.name.empty())
          continue;
        bool merged = false;
        for (auto &existing : session_files_) {
          if (existing.path != file.path)
            continue;
          if (!file.content.empty())
            existing.content = file.content;
          existing.written = existing.written || file.written;
          merged = true;
          break;
        }
        if (!merged)
          session_files_.push_back(std::move(file));
        if (session_files_.size() >= 24u)
          break;
      }
      for (auto &file : session_files_) {
        if (!file.content.empty() || file.path.empty())
          continue;
        std::error_code error;
        std::ifstream stream(path_from_utf8(file.path), std::ios::binary);
        if (stream &&
            std::filesystem::exists(path_from_utf8(file.path), error)) {
          std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
          if (text.size() <= 262'144u)
            file.content = std::move(text);
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
    for (auto iterator = photons_.rbegin(); iterator != photons_.rend();
         ++iterator) {
      if (iterator->kind != "user.input" && iterator->kind != "user.message")
        continue;
      turn_start_sequence = iterator->sequence;
      current_turn_time = std::string(time_label(iterator->committed_at_ms));
      if (const auto *text = tokmon::cbor::find(iterator->payload, "text"))
        user_message = std::string(text->as_string());
      break;
    }
    std::string state = "正在沿光路执行";
    for (auto iterator = photons_.rbegin(); iterator != photons_.rend();
         ++iterator) {
      if (iterator->sequence < turn_start_sequence)
        continue;
      if (assistant.empty() && iterator->kind == "assistant.message") {
        if (const auto *text = tokmon::cbor::find(iterator->payload, "text"))
          assistant = std::string(text->as_string());
      }
      if (iterator->kind == "ray.darkened" ||
          iterator->kind == "assistant.message")
        state = "审阅完成";
    }
    if (assistant.empty())
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend();
           ++iterator)
        if (iterator->sequence >= turn_start_sequence &&
            iterator->kind == "tool.result") {
          assistant =
              "真实工具已执行：" + tokmon::cbor::diagnostic(iterator->payload);
          break;
        }
    if (assistant.empty())
      for (auto iterator = photons_.rbegin(); iterator != photons_.rend();
           ++iterator)
        if (iterator->sequence >= turn_start_sequence &&
            (iterator->kind == "act.failed" ||
             iterator->kind == "model.failed")) {
          const auto *detail = tokmon::cbor::find(iterator->payload, "detail");
          if (!detail)
            detail = tokmon::cbor::find(iterator->payload, "error");
          assistant = "执行失败：" +
                      (detail ? std::string(detail->as_string())
                              : std::string("请在轨迹中查看失败 Photon"));
          state = "执行失败";
          break;
        }
    (void)slint::invoke_from_event_loop(
        [timeline, workflow, code, window, items = std::move(items),
         workflow_items = std::move(workflow_items), lines = std::move(lines),
         trace, assistant = std::move(assistant),
         user_message = std::move(user_message),
         current_turn_time = std::move(current_turn_time),
         state = std::move(state), workflow_complete, replace]() mutable {
          timeline->clear();
          for (auto &item : items)
            timeline->push_back(std::move(item));
          workflow->clear();
          for (auto &item : workflow_items)
            workflow->push_back(std::move(item));
          code->clear();
          for (auto &line : lines)
            code->push_back(std::move(line));
          if (auto locked = window.lock()) {
            auto handle = *locked;
            if (replace || !assistant.empty())
              handle->set_assistant_text(display_string(assistant));
            if (replace || !user_message.empty())
              handle->set_last_message(display_string(user_message));
            handle->set_status_text(display_string(state));
            handle->set_chat_empty(items.empty() && workflow_items.empty() &&
                                   assistant.empty());
            if (!user_message.empty())
              handle->set_workspace_locked(true);
            handle->set_chat_time(display_string(current_turn_time));
            handle->set_trace_duration(display_string(trace.duration));
            handle->set_workflow_duration(display_string(trace.turn_duration));
            handle->set_trace_turns(trace.turns);
            handle->set_trace_calls(trace.calls);
            handle->set_trace_input_tokens(
                static_cast<int>(std::min<std::int64_t>(
                    trace.input_tokens, std::numeric_limits<int>::max())));
            handle->set_trace_output_tokens(
                static_cast<int>(std::min<std::int64_t>(
                    trace.output_tokens, std::numeric_limits<int>::max())));
            handle->set_trace_provider(display_string(trace.provider));
            handle->set_trace_model(display_string(trace.model));
            handle->set_trace_result(display_string(trace.result));
            handle->set_workflow_done(workflow_complete ? 1 : 0);
            handle->set_daemon_state("后台服务已连接");
          }
        });
  }

  void apply_settings(tokmon::cbor::Value values,
                      const bool include_navigation) {
    auto window = window_;
    auto navigation = navigation_;
    auto navigation_model = navigation_model_;
    const auto assets = assets_;
    const auto workspace = current_workspace_;
    const auto navigation_workspace = navigation_workspace_;
    (void)slint::invoke_from_event_loop([this, window, navigation,
                                         navigation_model, assets, workspace,
                                         navigation_workspace,
                                         include_navigation,
                                         values = std::move(values)] {
      auto locked = window.lock();
      if (!locked || !values.as_map())
        return;
      auto handle = *locked;
      const auto string_value =
          [&values](const char *key) -> std::optional<slint::SharedString> {
        const auto *field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<std::string>(field->data))
          return std::nullopt;
        return display_string(field->as_string());
      };
      const auto bool_value =
          [&values](const char *key) -> std::optional<bool> {
        const auto *field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<bool>(field->data))
          return std::nullopt;
        return field->as_bool();
      };
      const auto int_value = [&values](const char *key) -> std::optional<int> {
        const auto *field = tokmon::cbor::find(values, key);
        if (!field || !std::holds_alternative<std::int64_t>(field->data))
          return std::nullopt;
        return static_cast<int>(field->as_integer());
      };
      if (auto value = string_value("language"))
        handle->set_setting_language(*value);
      if (auto value = string_value("startup"))
        handle->set_setting_startup(*value);
      if (auto value = string_value("autosave"))
        handle->set_setting_autosave(*value);
      else if (auto value = bool_value("autosave"))
        handle->set_setting_autosave(
            display_string(*value ? "5 分钟" : "关闭"));
      if (auto value = string_value("provider"))
        handle->set_setting_provider(*value);
      if (auto value = string_value("main_model")) {
        handle->set_setting_main_model(*value);
        handle->set_model_name(*value);
      }
      if (auto value = string_value("reasoning"))
        handle->set_setting_reasoning(*value);
      if (auto value = string_value("reasoning"))
        handle->set_effort(*value);
      if (auto value = string_value("command_approval"))
        handle->set_setting_command_approval(*value);
      if (auto value = bool_value("network"))
        handle->set_setting_network(*value);
      if (auto value = bool_value("high_risk_confirmation"))
        handle->set_setting_high_risk(*value);
      if (auto value = string_value("index_mode"))
        handle->set_setting_index_mode(*value);
      if (auto value = bool_value("workspace_sync"))
        handle->set_setting_workspace_sync(*value);
      if (auto value = bool_value("git"))
        handle->set_setting_git(*value);
      if (auto value = bool_value("notifications"))
        handle->set_setting_notifications(*value);
      if (auto value = bool_value("desktop_notifications"))
        handle->set_setting_desktop_notifications(*value);
      if (auto value = bool_value("message_alerts"))
        handle->set_setting_message_alerts(*value);
      if (auto value = string_value("quiet_hours"))
        handle->set_setting_quiet_hours(*value);
      else if (auto value = bool_value("quiet_hours"))
        handle->set_setting_quiet_hours(
            display_string(*value ? "22:00 - 08:00" : "关闭"));
      if (auto value = bool_value("dark_theme"))
        handle->set_setting_dark_theme(*value);
      if (auto value = int_value("accent"))
        handle->set_setting_accent(*value);
      if (auto value = string_value("density"))
        handle->set_setting_density(*value);
      if (auto value = int_value("font_scale"))
        handle->set_setting_font_scale(*value);
      if (auto value = int_value("ui_scale")) {
        const auto scale = std::clamp(*value, 70, 200);
        handle->set_setting_ui_scale(scale);
        handle->set_applied_ui_scale_percent(scale);
      }
      if (auto value = string_value("nickname"))
        handle->set_setting_nickname(*value);
      if (auto value = string_value("email"))
        handle->set_setting_email(*value);
      if (auto value = bool_value("cloud_sync"))
        handle->set_setting_cloud_sync(*value);
      if (auto value = bool_value("sidebar_visible"))
        handle->set_sidebar_visible(*value);
      if (auto value = bool_value("code_visible"))
        handle->invoke_set_code_panel_visible(*value);
      if (auto value = bool_value("task_expanded"))
        handle->set_task_expanded(*value);
      if (auto value = string_value("update_channel"))
        handle->set_setting_channel(*value);
      if (auto value = string_value("file_access"))
        handle->set_setting_file_access(*value);
      handle->set_setting_workspace(display_string(path_to_utf8(workspace)));
      if (include_navigation) {
        const auto *encoded = tokmon::cbor::find(values, "navigation");
        if (encoded) {
          if (auto decoded =
                  navigation_items(*encoded, assets, navigation_workspace)) {
            *navigation = std::move(*decoded);
            refresh_navigation(navigation_model, navigation,
                               std::string(handle->get_search_text()));
            save_navigation();
            bool selection_changed = false;
            for (std::size_t index = 0; index < navigation->size(); ++index) {
              auto &item = (*navigation)[index];
              const auto target = navigation_workspace_at(*navigation, index,
                                                          navigation_workspace);
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
      if (include_navigation)
        finish_initialization();
    });
    publish_workspace_info();
  }

  void apply_providers(const tokmon::cbor::Value &payload) {
    const auto *selected = tokmon::cbor::find(payload, "default");
    const auto *providers = tokmon::cbor::find(payload, "providers");
    if (!selected || !providers || !providers->as_array())
      return;
    tokmon::cbor::Value chosen;
    std::vector<ModelChoice> choices;
    for (const auto &provider : *providers->as_array()) {
      const auto *id = tokmon::cbor::find(provider, "id");
      const auto *model = tokmon::cbor::find(provider, "model");
      const auto *enabled = tokmon::cbor::find(provider, "enabled");
      if (id && model && (!enabled || enabled->as_bool())) {
        ModelChoice choice;
        choice.provider = display_string(id->as_string());
        choice.model = display_string(model->as_string());
        choice.label = display_string(std::string(id->as_string()) + " · " +
                                      std::string(model->as_string()));
        choices.push_back(std::move(choice));
      }
      if (const auto *id = tokmon::cbor::find(provider, "id");
          id && id->as_string() == selected->as_string())
        chosen = provider;
    }
    if (!chosen.as_map())
      return;
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, chosen = std::move(chosen),
                                         choices =
                                             std::move(choices)]() mutable {
      auto locked = window.lock();
      if (!locked)
        return;
      auto handle = *locked;
      auto model = std::make_shared<slint::VectorModel<ModelChoice>>();
      for (auto &choice : choices)
        model->push_back(std::move(choice));
      handle->set_model_choices(model);
      const auto string_field = [&chosen](const char *key) {
        const auto *value = tokmon::cbor::find(chosen, key);
        return display_string(value ? value->as_string() : std::string_view{});
      };
      handle->set_setting_provider(string_field("id"));
      handle->set_setting_provider_protocol(string_field("protocol"));
      handle->set_setting_provider_endpoint(string_field("endpoint"));
      handle->set_setting_provider_auth(string_field("auth"));
      handle->set_setting_main_model(string_field("model"));
      handle->set_model_name(string_field("model"));
      const auto *thinking = tokmon::cbor::find(chosen, "thinking");
      handle->set_setting_provider_thinking(thinking && thinking->as_bool());
      const auto *credential = tokmon::cbor::find(chosen, "credential_present");
      handle->set_setting_provider_credential(
          credential && credential->as_bool() ? "凭据已安全保存（输入可轮换）"
                                              : "尚未配置 API Key");
      handle->set_settings_status("provider 配置已由后台服务验证并载入");
    });
  }

  void update_daemon_state(const slint::SharedString &state) {
    auto window = window_;
    (void)slint::invoke_from_event_loop([window, state] {
      if (auto locked = window.lock()) {
        auto handle = *locked;
        handle->set_daemon_state(state);
      }
    });
  }

  void apply_command_response(const tokmon::cbor::Value &payload) {
    const auto read_string = [&payload](const char *key) {
      const auto *value = tokmon::cbor::find(payload, key);
      return value ? std::string(value->as_string()) : std::string{};
    };
    const auto read_bool = [&payload](const char *key) {
      const auto *value = tokmon::cbor::find(payload, key);
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
    if (!copied.empty())
      copy_to_clipboard(copied);
    auto timeline = timeline_;
    auto workflow = conversation_workflow_;
    auto code = code_;
    auto window = window_;
    (void)slint::invoke_from_event_loop(
        [timeline, workflow, code, window, display, title, model, provider,
         effort, access, clear, settings, close, copied]() {
          if (clear) {
            timeline->clear();
            workflow->clear();
            code->clear();
          }
          if (auto locked = window.lock()) {
            auto handle = *locked;
            if (!display.empty())
              handle->set_assistant_text(display_string(display));
            if (!title.empty())
              handle->set_session_title(display_string(title));
            if (!model.empty())
              handle->set_model_name(display_string(model));
            if (!provider.empty())
              handle->set_setting_provider(display_string(provider));
            if (!effort.empty())
              handle->set_effort(effort == "low"      ? "低"
                                 : effort == "medium" ? "标准"
                                 : effort == "high"   ? "高"
                                                      : "最高");
            if (!access.empty())
              handle->set_access_mode(access == "full"         ? "完全访问"
                                      : access == "restricted" ? "受限访问"
                                                               : "只读模式");
            if (settings)
              handle->set_settings_open(true);
            handle->set_status_text(copied.empty() ? "命令已完成"
                                                   : "内容已复制到剪贴板");
          }
          if (close)
            slint::quit_event_loop();
        });
  }

  void show_error(std::string message) {
    TimelineItem item;
    item.time = "now";
    item.kind = "snow.error";
    item.title = "配置或后台服务错误";
    item.detail = display_string(message);
    item.progress = -1;
    item.tone = "danger";
    auto model = timeline_;
    auto window = window_;
    update_daemon_state("配置或后台服务错误");
    (void)slint::invoke_from_event_loop(
        [model, window, item = std::move(item),
         message = std::move(message)]() mutable {
          model->push_back(std::move(item));
          if (auto locked = window.lock()) {
            (*locked)->set_error_dialog_title("配置或后台服务错误");
            (*locked)->set_error_dialog_message(display_string(message));
            (*locked)->set_error_dialog_fatal(false);
            (*locked)->set_error_dialog_open(true);
          }
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
        [model, window, item = std::move(item),
         message = std::move(message)]() mutable {
          model->push_back(std::move(item));
          if (auto locked = window.lock()) {
            (*locked)->set_daemon_state("原工作空间仍连接");
            (*locked)->set_settings_status(display_string(message));
            (*locked)->set_error_dialog_title("工作空间错误");
            (*locked)->set_error_dialog_message(display_string(message));
            (*locked)->set_error_dialog_fatal(false);
            (*locked)->set_error_dialog_open(true);
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
    auto validated_config = tokmon::load_config(*target);
    if (!validated_config) {
      show_workspace_error("配置文件无效：" +
                           validated_config.error().describe());
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
      auto connected = tokmon::ensure_daemon(
          tokmon::DaemonLaunchOptions{.endpoint = target_endpoint,
                                      .workspace = *target,
                                      .executable = daemon_executable_});
      if (!connected) {
        show_workspace_error("无法启动工作空间后台服务：" +
                             connected.error().describe());
        return false;
      }
      started = connected->started;
      auto attached =
          tokmon::DaemonClientLease::attach(tokmon::DaemonClientOptions{
              .endpoint = target_endpoint,
              .client_id = tokmon::make_id("desktop-workspace-client"),
              .client_kind = "desktop",
              .shutdown_when_idle = true,
              .idle_timeout = std::chrono::milliseconds(250),
              .lease_ttl = std::chrono::seconds(6)});
      if (!attached) {
        show_workspace_error("无法附着工作空间后台服务：" +
                             attached.error().describe());
        return false;
      }
      next_lease.emplace(std::move(*attached));
    }

    if (active_workspace_lease_) {
      (void)active_workspace_lease_->detach();
      active_workspace_lease_.reset();
    }
    if (next_lease)
      active_workspace_lease_.emplace(std::move(*next_lease));
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
    const auto state = slint::SharedString(
        started ? "工作空间后台服务已自动启动" : "工作空间后台服务已连接");
    (void)slint::invoke_from_event_loop(
        [timeline, workflow, code, window, display, state] {
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
            handle->set_settings_status(
                "已切换工作空间；正在载入项目级 .tokmon/config.yaml");
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
        const auto queued =
            condition_.wait_for(lock, stop, std::chrono::milliseconds(500),
                                [this] { return !commands_.empty(); });
        if (stop.stop_requested())
          return;
        if (queued) {
          command = std::move(commands_.front());
          commands_.pop_front();
        } else {
          // Do not replay the causal tail while idle, but keep validating the
          // watched YAML so an external edit produces a Desktop dialog even
          // before the user submits another action.
          const auto now = std::chrono::steady_clock::now();
          if (now - last_config_validation_ < std::chrono::seconds(1))
            continue;
          last_config_validation_ = now;
          command = Command{"config-validate", {}};
        }
      }
      if (command.kind == "workspace-refresh") {
        publish_workspace_info();
        continue;
      }
      if (command.kind == "switch-workspace") {
        (void)activate_workspace(command.text);
        continue;
      }
      if (command.kind == "new-session" || command.kind == "open-session") {
        if (const auto *expected =
                tokmon::cbor::find(command.payload, "workspace");
            expected && !expected->as_string().empty()) {
          auto target = normalize_workspace_path(expected->as_string(),
                                                 navigation_workspace_);
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
        auto timeline = timeline_;
        auto workflow = conversation_workflow_;
        auto code = code_;
        auto window = window_;
        const bool reset_environment_panel = command.kind == "new-session";
        (void)slint::invoke_from_event_loop(
            [timeline, workflow, code, window, reset_environment_panel] {
              timeline->clear();
              workflow->clear();
              code->clear();
              if (auto locked = window.lock()) {
                auto handle = *locked;
                handle->set_assistant_text("");
                handle->set_last_message("");
                handle->set_chat_time("");
                handle->set_status_text("等待输入");
                handle->set_chat_empty(true);
                handle->set_workspace_locked(false);
                if (reset_environment_panel)
                  handle->set_environment_panel_open(false);
                handle->set_selected_file_name("");
                handle->set_file_added_lines(0);
                handle->set_preview_content("");
              }
            });
        if (command.kind == "new-session" || command.text.empty())
          continue;
        active_ray_ = command.text;
      }
      if (command.kind == "chat" || command.kind == "slash-command")
        publish_pending(command.text);
      // A session without a Ray is persisted by navigation-save. Once a Ray
      // exists, mirror the title into its immutable session metadata below.
      if (command.kind == "rename-session" && active_ray_.empty())
        continue;
      tokmon::SnowMessage request;
      request.request_id = tokmon::next_snow_request_id();
      if (command.kind == "snapshot") {
        request.kind = tokmon::SnowMessageKind::snapshot_request;
        request.cursor = cursor_;
      } else {
        request.kind = tokmon::SnowMessageKind::intent;
        if (command.kind == "chat") {
          request.payload = tokmon::cbor::object({{"action", "chat"},
                                                  {"text", command.text},
                                                  {"ray", active_ray_}});
          if (const auto *selection = command.payload.as_map())
            for (const auto &[key, value] : *selection)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "slash-command") {
          request.payload = tokmon::cbor::object({{"action", "command.execute"},
                                                  {"text", command.text},
                                                  {"ray", active_ray_}});
          if (const auto *selection = command.payload.as_map())
            for (const auto &[key, value] : *selection)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "rename-session") {
          request.payload = tokmon::cbor::object(
              {{"action", "command.execute"},
               {"text", "/rename " + command.text},
               {"ray", active_ray_},
               {"surface", "desktop-ui"}});
        } else if (command.kind == "settings-load")
          request.payload = tokmon::cbor::object({{"action", "settings.get"}});
        else if (command.kind == "providers-load")
          request.payload =
              tokmon::cbor::object({{"action", "model.providers"}});
        else if (command.kind == "settings-save")
          request.payload =
              tokmon::cbor::object({{"action", "settings.save"},
                                    {"values", std::move(command.payload)}});
        else if (command.kind == "navigation-save")
          request.payload =
              tokmon::cbor::object({{"action", "navigation.save"},
                                    {"items", std::move(command.payload)}});
        else if (command.kind == "open-session")
          request.payload = tokmon::cbor::object(
              {{"action", "surface"}, {"ray", command.text}});
        else if (command.kind == "provider-configure") {
          request.payload =
              tokmon::cbor::object({{"action", "model.provider.configure"}});
          if (const auto *values = command.payload.as_map())
            for (const auto &[key, value] : *values)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-use") {
          request.payload =
              tokmon::cbor::object({{"action", "model.provider.use"}});
          if (const auto *values = command.payload.as_map())
            for (const auto &[key, value] : *values)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-secret") {
          request.payload =
              tokmon::cbor::object({{"action", "model.provider.secret.set"}});
          if (const auto *values = command.payload.as_map())
            for (const auto &[key, value] : *values)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "provider-test") {
          request.payload =
              tokmon::cbor::object({{"action", "model.provider.test"}});
          if (const auto *values = command.payload.as_map())
            for (const auto &[key, value] : *values)
              (*request.payload.as_map())[key] = value;
        } else if (command.kind == "config-validate") {
          request.payload =
              tokmon::cbor::object({{"action", "config.validate"}});
        } else
          request.payload =
              tokmon::cbor::object({{"action", "lens.reconcile"}});
      }
      const auto &request_endpoint =
          command.kind == "navigation-save" ? navigation_endpoint_ : endpoint_;
      tokmon::SnowClient client(request_endpoint);
      auto response = client.request(request);
      if (!response) {
        const auto message = response.error().describe();
        if (message != last_error_) {
          last_error_ = message;
          show_error(message);
        }
        if (command.kind == "settings-load") {
          const auto *include =
              tokmon::cbor::find(command.payload, "include_navigation");
          if (include && include->as_bool())
            finish_initialization();
        }
        continue;
      }
      if (response->kind == tokmon::SnowMessageKind::error) {
        const auto *message = tokmon::cbor::find(response->payload, "message");
        const auto error_message = message ? std::string(message->as_string())
                                           : std::string("后台服务拒绝了请求");
        if (error_message != last_error_) {
          last_error_ = error_message;
          show_error(error_message);
        }
        if (command.kind == "settings-load") {
          const auto *include =
              tokmon::cbor::find(command.payload, "include_navigation");
          if (include && include->as_bool())
            finish_initialization();
        }
        continue;
      }
      last_error_.clear();
      update_daemon_state("后台服务已连接");
      if (command.kind != "navigation-save")
        cursor_ = std::max(cursor_, response->cursor);
      if (command.kind == "settings-load") {
        const auto *include =
            tokmon::cbor::find(command.payload, "include_navigation");
        if (const auto *values =
                tokmon::cbor::find(response->payload, "values"))
          apply_settings(*values, include && include->as_bool());
        continue;
      }
      if (command.kind == "providers-load") {
        apply_providers(response->payload);
        continue;
      }
      if (command.kind == "config-validate")
        continue;
      if (command.kind == "settings-save") {
        auto window = window_;
        (void)slint::invoke_from_event_loop([window] {
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_settings_status(
                "已原子保存到项目级 .tokmon/config.yaml");
          }
        });
        continue;
      }
      if (command.kind == "navigation-save")
        continue;
      if (command.kind == "open-session") {
        auto active = photons_from_surface(*response);
        apply_photons(std::move(active), true);
        continue;
      }
      if (command.kind == "provider-configure" ||
          command.kind == "provider-secret" || command.kind == "provider-use") {
        auto window = window_;
        const auto status =
            command.kind == "provider-configure"
                ? slint::SharedString("平台 YAML 已原子保存并完成热重载")
            : command.kind == "provider-secret"
                ? slint::SharedString(
                      "API Key 已写入系统凭据库；未进入 YAML/Photon/日志")
                : slint::SharedString("默认模型平台已切换并完成热重载");
        (void)slint::invoke_from_event_loop([window, status] {
          if (auto locked = window.lock())
            (*locked)->set_settings_status(status);
        });
        load_providers();
        continue;
      }
      if (command.kind == "reconcile")
        continue;
      if (command.kind == "rename-session")
        continue;
      if (command.kind == "slash-command") {
        if (const auto *ray = tokmon::cbor::find(response->payload, "ray"))
          active_ray_ = std::string(ray->as_string());
        bind_active_ray_to_selected_session();
        if (tokmon::cbor::find(response->payload, "clear_session") &&
            tokmon::cbor::find(response->payload, "clear_session")->as_bool()) {
          photons_.clear();
        } else {
          auto photons = photons_from(*response);
          if (!photons.empty())
            apply_photons(std::move(photons), true);
        }
        apply_command_response(response->payload);
        continue;
      }
      if (command.kind == "chat" || command.kind == "provider-test")
        if (const auto *ray = tokmon::cbor::find(response->payload, "ray"))
          active_ray_ = std::string(ray->as_string());
      if (command.kind == "chat")
        bind_active_ray_to_selected_session();
      auto photons = photons_from(*response);
      const bool received_full_photons = !photons.empty();
      if (!photons.empty())
        apply_photons(std::move(photons),
                      response->kind == tokmon::SnowMessageKind::snapshot);
      if (command.kind == "provider-test") {
        auto window = window_;
        (void)slint::invoke_from_event_loop([window] {
          if (auto locked = window.lock())
            (*locked)->set_settings_status(
                "真实 provider 请求已完成；结果已投影到对话与轨迹");
        });
      }
      if (!received_full_photons &&
          (command.kind == "chat" || command.kind == "provider-test") &&
          !active_ray_.empty()) {
        tokmon::SnowMessage surface_request;
        surface_request.kind = tokmon::SnowMessageKind::intent;
        surface_request.request_id = tokmon::next_snow_request_id();
        surface_request.cursor = cursor_;
        surface_request.payload =
            tokmon::cbor::object({{"action", "surface"}, {"ray", active_ray_}});
        auto projected = client.request(surface_request);
        if (projected && projected->kind != tokmon::SnowMessageKind::error) {
          cursor_ = std::max(cursor_, projected->cursor);
          auto active = photons_from_surface(*projected);
          if (!active.empty())
            apply_photons(std::move(active), true);
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
  std::chrono::steady_clock::time_point last_config_validation_{};
  std::jthread worker_;
};

} // namespace

std::unique_ptr<UiController> make_ui_controller(
    std::filesystem::path endpoint, std::filesystem::path workspace,
    std::filesystem::path daemon_executable,
    std::shared_ptr<slint::VectorModel<TimelineItem>> timeline,
    std::shared_ptr<slint::VectorModel<TimelineItem>> conversation_workflow,
    std::shared_ptr<slint::VectorModel<CodeLine>> code,
    std::shared_ptr<slint::VectorModel<TraceEvent>> trace_events,
    std::shared_ptr<slint::VectorModel<GanttSegment>> gantt,
    std::shared_ptr<slint::VectorModel<NavigationItem>> navigation_model,
    std::shared_ptr<std::vector<NavigationItem>> navigation,
    std::filesystem::path assets, slint::ComponentWeakHandle<MainWindow> window,
    const bool restore_initial_workspace) {
  return std::make_unique<UiControllerImpl>(
      std::move(endpoint), std::move(workspace), std::move(daemon_executable),
      std::move(timeline), std::move(conversation_workflow), std::move(code),
      std::move(trace_events), std::move(gantt), std::move(navigation_model),
      std::move(navigation), std::move(assets), std::move(window),
      restore_initial_workspace);
}

} // namespace tokmon::desktop
