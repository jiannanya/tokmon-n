#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

#include "desktop_application.hpp"
#include "navigation_state.hpp"
#include "platform_utils.hpp"
#include "ui_controller.hpp"
#include "ui_projection.hpp"

namespace tokmon::desktop {

namespace {

constexpr std::size_t kAutomaticTitleCharacters = 32;

std::string default_new_session_title(
    const std::vector<NavigationItem> &navigation) {
  return "新会话 " +
         std::to_string(std::ranges::count_if(
                            navigation, [](const NavigationItem &item) {
                              return item.kind == "session";
                            }) +
                        1);
}

std::string automatic_session_title(const std::string_view message) {
  const auto valid = display_utf8(message);
  std::string title;
  title.reserve(std::min(valid.size(), kAutomaticTitleCharacters * 4u));
  std::size_t characters = 0;
  bool pending_space = false;
  bool truncated = false;
  for (std::size_t index = 0; index < valid.size();) {
    const auto lead = static_cast<unsigned char>(valid[index]);
    if (lead <= 0x7fu && std::isspace(lead)) {
      pending_space = !title.empty();
      ++index;
      continue;
    }
    if (characters == kAutomaticTitleCharacters) {
      truncated = true;
      break;
    }
    const auto width = lead <= 0x7fu   ? 1u
                       : lead <= 0xdfu ? 2u
                       : lead <= 0xefu ? 3u
                                       : 4u;
    if (pending_space) {
      title.push_back(' ');
      pending_space = false;
    }
    title.append(valid, index, width);
    index += width;
    ++characters;
  }
  if (truncated)
    title += "…";
  return title;
}

int run_fatal_desktop_error(slint::ComponentHandle<MainWindow> window,
                            const std::string_view title,
                            const std::string_view message) {
  window->set_daemon_state("配置校验失败");
  window->set_status_text("配置错误");
  window->set_error_dialog_title(display_string(title));
  window->set_error_dialog_message(display_string(message));
  window->set_error_dialog_fatal(true);
  window->set_error_dialog_open(true);
  window->on_error_dialog_dismissed([] { slint::quit_event_loop(); });
  window->on_close_window([] { slint::quit_event_loop(); });
  window->show();
#if defined(_WIN32)
  make_current_process_window_frameless();
  slint::Timer::single_shot(std::chrono::milliseconds(1), [] {
    set_current_process_window_topmost(true);
    activate_current_process_window();
  });
#endif
  slint::run_event_loop();
  window->hide();
  return 2;
}

} // namespace

int run_application(int argc, char **argv) {
  std::optional<std::filesystem::path> workspace;
  bool open_settings = false;
  int settings_page = 0;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--workspace" && index + 1 < argc)
      workspace = argv[++index];
    else if (std::string_view(argv[index]) == "--open-settings")
      open_settings = true;
    else if (std::string_view(argv[index]) == "--settings-page" &&
             index + 1 < argc) {
      try {
        settings_page = std::clamp(std::stoi(argv[++index]), 0, 7);
      } catch (...) {
        settings_page = 0;
      }
    }
  }
  auto window = MainWindow::create();
  const auto default_ui_scale_percent =
      default_ui_scale_percent_for_primary_display();
  window->set_default_ui_scale_percent(default_ui_scale_percent);
  window->set_setting_font_scale(default_ui_scale_percent);
  window->set_setting_ui_scale(default_ui_scale_percent);
  window->set_applied_ui_scale_percent(default_ui_scale_percent);
  window->set_settings_page(settings_page);
  window->set_settings_open(open_settings);

  auto paths = tokmon::resolve_paths(workspace);
  if (!paths)
    return run_fatal_desktop_error(window, "Tokmon 配置错误",
                                   paths.error().describe());
  // The daemon performs the authoritative configuration parse/validate; the
  // frontend intentionally does not duplicate that work anymore. A rejected
  // configuration surfaces through the post-attach probe below instead of a
  // blocked first frame.

  // First frame precedes every process/network/configuration operation.
  window->set_daemon_state("正在连接后台服务");
  window->set_status_text("正在连接后台服务");
  window->show();
#if defined(_WIN32)
  make_current_process_window_frameless();
#endif

  std::error_code path_error;
  auto executable = argc > 0 ? std::filesystem::absolute(argv[0], path_error)
                             : std::filesystem::current_path();
#if defined(_WIN32)
  std::wstring module(32'768, L'\0');
  const auto module_size = GetModuleFileNameW(
      nullptr, module.data(), static_cast<DWORD>(module.size()));
  if (module_size > 0 && module_size < module.size()) {
    module.resize(module_size);
    executable = std::filesystem::path(module);
  }
#endif
  const auto endpoint =
      tokmon::workspace_snow_endpoint(paths->run, paths->project.parent_path());
#if defined(_WIN32)
  const auto daemon_executable = executable.parent_path() / "tokmon.exe";
#else
  const auto daemon_executable = executable.parent_path() / "tokmon";
#endif
  struct BackendConnection {
    std::optional<tokmon::DaemonConnection> connection;
    std::optional<tokmon::DaemonClientLease> lease;
    std::optional<tokmon::Error> failure;
  } backend;
  auto assets = executable.parent_path() / "assets" / "figma";
  if (!std::filesystem::exists(assets))
    assets = std::filesystem::current_path() / "apps" / "tokmon-desktop" /
             "assets" / "figma";

  const auto navigation_workspace = paths->project.parent_path();
  const auto navigation_workspace_text = path_to_utf8(navigation_workspace);
  const std::vector<NavigationItem> navigation = [&assets,
                                                  &navigation_workspace_text] {
    std::vector<NavigationItem> items;
    const auto add = [&items, &assets, &navigation_workspace_text](
                         const char *title, const char *kind, int indent,
                         bool selected) {
      auto item = make_navigation_item(
          assets, tokmon::make_id("navigation"), kind, title, indent, selected,
          true, {},
          std::string_view(kind) == "project" ? navigation_workspace_text
                                              : std::string{});
      // These bundled examples are existing named sessions, not blank sessions
      // waiting for a first-message title.
      item.title_manual = std::string_view(kind) == "session";
      items.push_back(std::move(item));
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
  auto assistant_blocks_model =
      std::make_shared<slint::VectorModel<ChatBlock>>();
  auto code_model = std::make_shared<slint::VectorModel<CodeLine>>();
  auto slash_model = std::make_shared<slint::VectorModel<SlashCommandItem>>();
  auto trace_events_model = std::make_shared<slint::VectorModel<TraceEvent>>();
  auto gantt_model = std::make_shared<slint::VectorModel<GanttSegment>>();
  for (auto &line : code_lines_from({}))
    code_model->push_back(std::move(line));
  window->set_navigation(nav_model);
  window->set_timeline(timeline_model);
  window->set_conversation_workflow(conversation_workflow_model);
  window->set_assistant_blocks(assistant_blocks_model);
  window->set_code_lines(code_model);
  window->set_slash_commands(slash_model);
  window->set_trace_events(trace_events_model);
  window->set_gantt(gantt_model);
  window->set_setting_workspace(display_string(navigation_workspace_text));
  auto controller = make_ui_controller(
      endpoint, navigation_workspace, daemon_executable, timeline_model,
      conversation_workflow_model, assistant_blocks_model, code_model,
      trace_events_model, gantt_model,
      nav_model, navigation_state, assets,
      slint::ComponentWeakHandle<MainWindow>(window), !workspace.has_value());
  // Connection, daemon launch, attach and every configuration-dependent load
  // happen after show() and off the Slint event loop thread.
  std::thread backend_connect(
      [endpoint, workspace = paths->project.parent_path(),
       executable = daemon_executable, &backend, controller = controller.get(),
       window = slint::ComponentWeakHandle<MainWindow>(window)]() {
        auto connection = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
            .endpoint = endpoint, .workspace = workspace,
            .executable = executable});
        if (!connection) {
          backend.failure = connection.error();
          const auto message = backend.failure->describe();
          (void)slint::invoke_from_event_loop([window, message] {
            if (auto locked = window.lock()) {
              auto handle = *locked;
              handle->set_daemon_state("后台服务连接失败");
              handle->set_status_text("后台服务连接失败");
              handle->set_error_dialog_title("Tokmon 无法启动后台服务");
              handle->set_error_dialog_message(display_string(message));
              handle->set_error_dialog_fatal(false);
              handle->set_error_dialog_open(true);
            }
          });
          return;
        }
        const bool started = connection->started;
        backend.connection = std::move(*connection);
        auto lease = tokmon::DaemonClientLease::attach(
            tokmon::DaemonClientOptions{.endpoint = endpoint,
                .client_id = tokmon::make_id("desktop-client"),
                .client_kind = "desktop", .shutdown_when_idle = true,
                .idle_timeout = std::chrono::milliseconds(250),
                .lease_ttl = std::chrono::seconds(6)});
        if (!lease) {
          backend.failure = lease.error();
          const auto message = backend.failure->describe();
          (void)slint::invoke_from_event_loop([window, message] {
            if (auto locked = window.lock()) {
              auto handle = *locked;
              handle->set_daemon_state("后台服务连接失败");
              handle->set_status_text("后台服务连接失败");
              handle->set_error_dialog_title("Tokmon 无法连接后台服务");
              handle->set_error_dialog_message(display_string(message));
              handle->set_error_dialog_fatal(false);
              handle->set_error_dialog_open(true);
            }
          });
          return;
        }
        backend.lease = std::move(*lease);
        (void)slint::invoke_from_event_loop([window, controller, started] {
          controller->backend_connected();
          if (auto locked = window.lock()) {
            auto handle = *locked;
            handle->set_daemon_state(started ? "后台服务已启动；正在检查配置"
                                             : "后台服务已连接；正在检查配置");
            handle->set_status_text("正在检查配置");
          }
        });
      });
  window->on_slash_query_changed([slash_model,
                                  window](const slint::SharedString &text) {
    const auto query = std::string(text);
    const auto separator = query.find_first_of(" \t\r\n");
    const auto visible =
        tokmon::is_slash_command(query) && separator == std::string::npos;
    slash_model->clear();
    if (visible) {
      for (const auto *descriptor : tokmon::match_slash_commands(query, 8)) {
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
  window->on_send_message([&controller, conversation_workflow_model, assistant_blocks_model, nav_model,
                           navigation_state,
                           window](const slint::SharedString &text) {
    const auto message = std::string(text);
    const auto slash_command = tokmon::is_slash_command(message);
    std::optional<std::string> first_message_title;
    if (window->get_chat_empty() && !slash_command) {
      for (auto &item : *navigation_state) {
        if (!item.selected || item.kind != "session")
          continue;
        if (item.title_manual) {
          first_message_title = std::string(item.title);
        } else {
          auto title = automatic_session_title(message);
          if (!title.empty()) {
            item.title = display_string(title);
            window->set_session_title(item.title);
            refresh_navigation(nav_model, navigation_state,
                               std::string(window->get_search_text()),
                               slint::ComponentWeakHandle<MainWindow>(window));
            controller->save_navigation();
            first_message_title = std::move(title);
          }
        }
        break;
      }
    }
    window->set_slash_menu_visible(false);
    conversation_workflow_model->clear();
    assistant_blocks_model->clear();
    window->set_last_message(text);
    window->set_assistant_text("");
    window->set_thought_text("");
    window->set_status_text("正在提交请求");
    window->set_chat_empty(false);
    window->set_workspace_locked(true);
    if (slash_command)
      controller->slash_command(message,
                                std::string(window->get_setting_provider()),
                                std::string(window->get_model_name()),
                                std::string(window->get_access_mode()),
                                std::string(window->get_effort()));
    else
      controller->chat(message,
                       std::string(window->get_setting_provider()),
                       std::string(window->get_model_name()),
                       std::string(window->get_access_mode()),
                       std::string(window->get_effort()));
    if (first_message_title)
      // Queue this after chat: a brand-new session receives its Ray from the
      // chat response, then the silent rename can persist either its automatic
      // or already-manual title there.
      controller->rename_session(std::move(*first_message_title));
  });
  window->on_new_session([navigation_state, window, navigation_workspace] {
    std::size_t project = navigation_state->size();
    for (std::size_t index = 0; index < navigation_state->size(); ++index) {
      if (!(*navigation_state)[index].selected)
        continue;
      project = navigation_ancestor_at(*navigation_state, index, "project");
      break;
    }
    if (project == navigation_state->size())
      for (std::size_t index = 0; index < navigation_state->size(); ++index)
        if ((*navigation_state)[index].kind == "project") {
          project = index;
          break;
        }
    const auto title = default_new_session_title(*navigation_state);
    const auto workspace = navigation_workspace_at(*navigation_state, project,
                                                   navigation_workspace);
    window->set_create_navigation_kind("会话");
    window->set_create_navigation_name(display_string(title));
    window->set_create_navigation_workspace(
        display_string(path_to_utf8(workspace)));
    window->set_create_navigation_group(slint::SharedString{});
    window->set_create_navigation_error("");
    window->set_create_navigation_open(true);
  });
  window->on_quick_create([nav_model, navigation_state, window,
                           navigation_workspace](int index) {
    if (index < 0 || index >= static_cast<int>(nav_model->row_count()))
      return;
    const auto clicked = *nav_model->row_data(index);
    const auto found =
        std::ranges::find(*navigation_state, clicked.id, &NavigationItem::id);
    if (found == navigation_state->end())
      return;
    const auto state_index = static_cast<std::size_t>(
        std::distance(navigation_state->begin(), found));
    const auto workspace = navigation_workspace_at(
        *navigation_state, state_index, navigation_workspace);
    slint::SharedString group;
    for (auto previous = state_index; previous > 0;) {
      --previous;
      if ((*navigation_state)[previous].indent >= found->indent)
        continue;
      if (std::string((*navigation_state)[previous].kind) == "group") {
        group = (*navigation_state)[previous].title;
        break;
      }
    }
    const auto title = default_new_session_title(*navigation_state);
    window->set_create_navigation_kind("会话");
    window->set_create_navigation_name(display_string(title));
    window->set_create_navigation_workspace(
        display_string(path_to_utf8(workspace)));
    window->set_create_navigation_group(group);
    window->set_create_navigation_error("");
    window->set_create_navigation_open(true);
  });
  window->on_clear_search([nav_model, navigation_state, window] {
    window->set_search_text(slint::SharedString{});
    refresh_navigation(nav_model, navigation_state, {},
                       slint::ComponentWeakHandle<MainWindow>(window));
  });
  window->on_select_navigation([nav_model, navigation_state, window,
                                &controller, navigation_workspace](int index) {
    if (index < 0 || index >= static_cast<int>(nav_model->row_count()))
      return;
    const auto clicked = *nav_model->row_data(index);
    const auto found =
        std::ranges::find(*navigation_state, clicked.id, &NavigationItem::id);
    if (found == navigation_state->end())
      return;
    const auto state_index = static_cast<std::size_t>(
        std::distance(navigation_state->begin(), found));
    for (auto &candidate : *navigation_state)
      candidate.selected = false;
    found->selected = true;
    if (found->expandable)
      found->expanded = !found->expanded;
    const auto kind = std::string(found->kind);
    const auto ray = std::string(found->ray);
    const auto title = found->title;
    const auto workspace = navigation_workspace_at(
        *navigation_state, state_index, navigation_workspace);
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    controller->save_navigation();
    if (kind == "session") {
      window->set_session_title(title);
      const auto target = path_to_utf8(workspace);
      controller->switch_workspace(target);
      controller->open_session(ray, target);
    } else if (kind == "project") {
      window->set_session_title(title);
      const auto target = path_to_utf8(workspace);
      controller->switch_workspace(target);
      controller->new_session(target);
    }
  });
  window->on_search_changed(
      [nav_model, navigation_state, window](const slint::SharedString &text) {
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
  window->on_identify_project([](const slint::SharedString &path_value) {
    return display_string(path_basename_utf8(std::string(path_value)));
  });
  window->on_project_in_group(
      [navigation_state](const slint::SharedString &group_value,
                         const slint::SharedString &path_value) -> bool {
        const auto group = std::string(group_value);
        const auto name = path_basename_utf8(std::string(path_value));
        if (name.empty())
          return false;
        std::size_t index = 0;
        while (index < navigation_state->size()) {
          const auto &item = (*navigation_state)[index];
          if (std::string(item.kind) == "group" &&
              std::string(item.title) == group) {
            for (auto next = index + 1;
                 next < navigation_state->size() &&
                 (*navigation_state)[next].indent > item.indent;
                 ++next) {
              if (std::string((*navigation_state)[next].kind) != "project")
                continue;
              const auto project_title = path_basename_utf8(
                  std::string((*navigation_state)[next].workspace));
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
  window->on_create_navigation([nav_model, navigation_state, assets, window,
                                &controller, navigation_workspace](
                                   const slint::SharedString &kind_value,
                                   const slint::SharedString &title_value,
                                   const slint::SharedString &group_value,
                                   const slint::SharedString &workspace_value)
                                   -> bool {
    const auto kind = std::string(kind_value) == "会话"   ? "session"
                      : std::string(kind_value) == "项目" ? "project"
                      : std::string(kind_value) == "分组"
                          ? "group"
                          : std::string(kind_value);
    const auto title = std::string(title_value);
    const auto requested_group = std::string(group_value);
    if ((kind != "group" && kind != "project" && kind != "session") ||
        title.empty() || title.size() > 256) {
      window->set_create_navigation_error("名称必须为 1–256 个字符");
      return false;
    }
    std::size_t parent = navigation_state->size();
    std::size_t host_group = navigation_state->size();
    const auto locate_group =
        [&](const std::string_view preferred) -> std::size_t {
      if (!preferred.empty())
        for (std::size_t index = 0; index < navigation_state->size(); ++index)
          if (std::string((*navigation_state)[index].kind) == "group" &&
              std::string((*navigation_state)[index].title) == preferred)
            return index;
      for (std::size_t index = 0; index < navigation_state->size(); ++index)
        if ((*navigation_state)[index].kind == "group")
          return index;
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
      const auto project_name = path_basename_utf8(
          normalized_requested.empty() ? std::string(workspace_value)
                                       : normalized_requested);
      if (host_group != navigation_state->size()) {
        for (auto next = host_group + 1;
             next < navigation_state->size() &&
             (*navigation_state)[next].indent >
                 (*navigation_state)[host_group].indent;
             ++next) {
          if (std::string((*navigation_state)[next].kind) != "project")
            continue;
          const auto stored = std::string((*navigation_state)[next].workspace);
          const auto candidate_name = path_basename_utf8(
              stored.empty() ? std::string((*navigation_state)[next].title)
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
          for (auto &item : *navigation_state)
            item.selected = false;
          auto hosting = make_navigation_item(
              assets, tokmon::make_id("navigation"), "project", project_name,
              (*navigation_state)[host_group].indent + 1, true, true, {},
              normalized_requested);
          std::size_t insertion = host_group + 1;
          while (insertion < navigation_state->size() &&
                 (*navigation_state)[insertion].indent >
                     (*navigation_state)[host_group].indent)
            ++insertion;
          navigation_state->insert(navigation_state->begin() +
                                       static_cast<std::ptrdiff_t>(insertion),
                                   std::move(hosting));
          host_group = insertion;
        }
      }
      if (parent == navigation_state->size())
        parent = host_group;
    }

    std::filesystem::path workspace = navigation_workspace;
    std::string stored_workspace;
    if (kind != "group") {
      auto normalized = normalize_workspace_path(std::string(workspace_value),
                                                 navigation_workspace);
      if (!normalized) {
        window->set_create_navigation_error("请选择或输入有效的工作空间路径");
        return false;
      }
      std::error_code directory_error;
      std::filesystem::create_directories(*normalized, directory_error);
      if (directory_error) {
        window->set_create_navigation_error(
            display_string("无法创建工作空间：" + directory_error.message()));
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
    for (auto &item : *navigation_state)
      item.selected = false;
    auto created = make_navigation_item(
        assets, tokmon::make_id(kind == "session" ? "session" : "navigation"),
        kind, title,
        parent == navigation_state->size()
            ? 0
            : (*navigation_state)[parent].indent + 1,
        true, true, {}, stored_workspace);
    if (kind == "session")
      created.title_manual =
          title != default_new_session_title(*navigation_state);
    std::size_t insertion = navigation_state->size();
    if (parent != navigation_state->size()) {
      (*navigation_state)[parent].expanded = true;
      insertion = parent + 1;
      while (insertion < navigation_state->size() &&
             (*navigation_state)[insertion].indent >
                 (*navigation_state)[parent].indent)
        ++insertion;
    }
    navigation_state->insert(navigation_state->begin() +
                                 static_cast<std::ptrdiff_t>(insertion),
                             std::move(created));
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    window->set_session_title(display_string(title));
    controller->save_navigation();
    if (kind != "group") {
      const auto target = path_to_utf8(workspace);
      controller->switch_workspace(target);
      controller->new_session(target);
    }
    return true;
  });
  window->on_rename_session([navigation_state, nav_model, window, &controller](
                                const slint::SharedString &title_value) {
    const auto title = std::string(title_value);
    if (title.empty() || title.size() > 256)
      return;
    bool found_session = false;
    bool title_changed = false;
    for (auto &item : *navigation_state)
      if (item.selected && item.kind == "session") {
        found_session = true;
        item.title_manual = true;
        if (item.title != title_value) {
          item.title = title_value;
          title_changed = true;
        }
        break;
      }
    if (!found_session)
      return;
    refresh_navigation(nav_model, navigation_state,
                       std::string(window->get_search_text()),
                       slint::ComponentWeakHandle<MainWindow>(window));
    controller->save_navigation();
    if (title_changed)
      controller->rename_session(title);
  });
  window->on_choose_attachment([](bool directory) {
    return display_string(choose_attachment(directory));
  });
  window->on_open_change_workspace([&controller, window] {
    window->set_change_workspace_path(
        display_string(path_to_utf8(controller->current_workspace())));
    window->set_change_workspace_open(true);
  });
  window->on_confirm_change_workspace(
      [&controller](const slint::SharedString &path_value) {
        const auto target = std::string(path_value);
        if (target.empty())
          return;
        controller->switch_workspace(target);
      });
  window->on_select_project_file(
      [&controller](int index) { controller->select_session_file(index); });
  window->on_copy_text([&controller](const slint::SharedString &text) {
    copy_to_clipboard(std::string_view(text));
    controller->notify_copied();
  });
  window->on_filter_preview([&controller](const slint::SharedString &query) {
    controller->filter_preview_lines(std::string(query));
  });
  window->on_cancel_settings([&controller] {
    controller->load_settings();
    controller->load_providers();
  });
  window->on_reconcile([&controller] { controller->reconcile(); });
  window->on_refresh_workspace(
      [&controller] { controller->refresh_workspace(); });
  window->on_configure_provider(
      [&controller](
          const slint::SharedString &id, const slint::SharedString &protocol,
          const slint::SharedString &endpoint, const slint::SharedString &model,
          const slint::SharedString &auth, bool thinking,
          const slint::SharedString &effort) {
        const auto effort_value = std::string(effort) == "低"     ? "low"
                                  : std::string(effort) == "标准" ? "medium"
                                  : std::string(effort) == "高"   ? "high"
                                                                  : "max";
        controller->configure_provider(
            tokmon::cbor::object({{"id", std::string(id)},
                                  {"protocol", std::string(protocol)},
                                  {"endpoint", std::string(endpoint)},
                                  {"model", std::string(model)},
                                  {"auth", std::string(auth)},
                                  {"thinking", thinking},
                                  {"default", true},
                                  {"reasoning_effort", effort_value},
                                  {"max_output_tokens", 4096},
                                  {"max_attempts", 6},
                                  {"retry_backoff_ms", 5'000}}));
      });
  window->on_store_provider_secret(
      [&controller](const slint::SharedString &id,
                    const slint::SharedString &secret) {
        controller->store_provider_secret(std::string(id), std::string(secret));
      });
  window->on_test_provider([&controller](const slint::SharedString &id) {
    controller->test_provider(std::string(id));
  });
  window->on_save_settings([window, &controller] {
    window->set_applied_ui_scale_percent(window->get_setting_ui_scale());
    controller->save_settings(tokmon::cbor::object(
        {{"language", std::string(window->get_setting_language())},
         {"startup", std::string(window->get_setting_startup())},
         {"autosave", std::string(window->get_setting_autosave())},
         {"provider", std::string(window->get_setting_provider())},
         {"main_model", std::string(window->get_setting_main_model())},
         {"reasoning", std::string(window->get_setting_reasoning())},
         {"command_approval",
          std::string(window->get_setting_command_approval())},
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
         {"font_scale",
          static_cast<std::int64_t>(window->get_setting_font_scale())},
         {"ui_scale",
          static_cast<std::int64_t>(window->get_setting_ui_scale())},
         {"nickname", std::string(window->get_setting_nickname())},
         {"email", std::string(window->get_setting_email())},
         {"cloud_sync", window->get_setting_cloud_sync()},
         {"sidebar_visible", window->get_sidebar_visible()},
         {"code_visible", window->get_code_visible()},
         {"task_expanded", window->get_task_expanded()},
         {"update_channel", std::string(window->get_setting_channel())},
         {"file_access", std::string(window->get_setting_file_access())}}));
    controller->select_provider(std::string(window->get_setting_provider()));
    window->set_model_name(window->get_setting_main_model());
    window->set_effort(window->get_setting_reasoning());
    window->set_settings_status("正在通过后台服务原子保存…");
  });
  window->on_reset_settings([window, default_ui_scale_percent] {
    window->set_setting_font_scale(default_ui_scale_percent);
    window->set_setting_ui_scale(default_ui_scale_percent);
    window->set_applied_ui_scale_percent(default_ui_scale_percent);
    window->set_settings_status("已恢复默认值；点击“保存更改”后写入");
  });
  window->on_drag_window([] { drag_current_process_window(); });
  window->on_update_window_drag([] { update_current_process_window_drag(); });
  window->on_end_window_drag([] { end_current_process_window_drag(); });
  window->on_minimize_window(
      [window] { window->window().set_minimized(true); });
  window->on_toggle_maximize_window([window] {
    const auto maximized = !window->window().is_maximized();
    window->window().set_maximized(maximized);
    window->set_window_maximized(maximized);
  });
  window->on_close_window([] { slint::quit_event_loop(); });
  window->on_error_dialog_dismissed([window] {
    window->set_error_dialog_open(false);
  });
  window->on_set_code_panel_visible([window](bool visible) {
    if (visible == window->get_code_visible())
      return;

    const auto maximized = window->window().is_maximized();
    auto physical_size = window->window().size();
    const auto scale = window->window().scale_factor();
    const auto panel_delta = static_cast<std::uint32_t>(
        std::lround((window->get_code_panel_width() + 1.0f) * scale));

    window->set_code_visible(visible);
    if (!maximized) {
      if (visible) {
        physical_size.width += panel_delta;
      } else {
        physical_size.width = physical_size.width > panel_delta
                                  ? physical_size.width - panel_delta
                                  : physical_size.width;
      }
      window->window().set_size(physical_size);
    }
  });
  window->on_refresh_trace([&controller] { controller->publish_trace_view(); });
  window->on_export_trace([&controller] { controller->export_trace(); });
  window->on_settings_searched([window](const slint::SharedString &text) {
    const auto query = display_utf8(std::string_view(text));
    if (query.empty())
      return;
    static const std::pair<int, std::vector<std::string>> table[] = {
        {0, {"语言", "启动", "自动保存", "更新通道", "通用"}},
        {1, {"智能体", "模型", "提供方", "推理", "主模型", "协议"}},
        {2, {"文件访问", "命令审批", "网络", "高风险", "权限", "安全"}},
        {3, {"工作区", "索引", "同步", "Git", "git"}},
        {4, {"通知", "桌面", "消息提醒", "免打扰"}},
        {5, {"外观", "主题", "强调色", "密度", "字体", "缩放", "分辨率", "比例", "屏幕", "dpi", "DPI", "放大", "缩小"}},
        {6, {"快捷键", "新建会话", "发送消息", "命令面板"}},
        {7, {"账户", "昵称", "邮箱", "方案", "云同步"}},
    };
    for (const auto &[page, keywords] : table) {
      for (const auto &keyword : keywords) {
        if (keyword.find(query) != std::string::npos ||
            query.find(keyword) != std::string::npos) {
          window->set_settings_page(page);
          window->set_settings_status(slint::SharedString(
              "已定位到设置页 " + std::to_string(page + 1)));
          return;
        }
      }
    }
    window->set_settings_status(slint::SharedString("未找到匹配的设置项"));
  });

#if defined(_WIN32)
  // Defer activation until Slint's event loop is actually dispatching native
  // focus messages. Activating before run_event_loop() makes Windows report the
  // HWND as foreground, but winit can still treat the first click as activation.
  slint::Timer::single_shot(std::chrono::milliseconds(1), [] {
    activate_current_process_window();
  });
#endif
  slint::run_event_loop();
  window->hide();
  if (backend_connect.joinable()) backend_connect.join();
  return 0;
}

} // namespace tokmon::desktop
