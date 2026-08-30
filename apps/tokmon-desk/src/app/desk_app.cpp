#include "app/desk_app.hpp"

#include "integration/daemon_client.hpp"
#include "fonts/font_manager.hpp"
#include "platform/desk_app_paths.hpp"
#include "platform/desk_resource_paths.hpp"
#include "platform/sdl_platform.hpp"
#include "render/rml_render_interface_skia.hpp"
#include "render/skia_device.hpp"
#include "state/desk_state_store.hpp"
#include "ui/desk_controller.hpp"
#include "ui/desk_view_model.hpp"
#include "ui/elements/element_code_surface.hpp"
#include "ui/elements/element_diff_surface.hpp"
#include "ui/elements/element_file_tree.hpp"
#include "ui/elements/element_terminal.hpp"
#include "ui/theme_palette.hpp"

#include "tokmon/config.hpp"
#include "tokmon/daemon_lifecycle.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace tokmon::desk {
namespace {

struct Arguments {
  std::filesystem::path workspace;
  std::filesystem::path app_data_root;
  bool smoke_test{false};
  bool software_renderer{false};
  int idle_test_ms{0};
  int ui_scale_percent{0};
  int content_scale_percent{0};
  // Match the established desktop shell at 150% Windows display scale.  The
  // RmlUi document is then rendered at the independently persisted 125%
  // content scale, producing the same 1318x900 legacy design viewport.
  int logical_width{1648};
  int logical_height{1125};
  std::filesystem::path screenshot;
  std::filesystem::path acceptance_report;
  std::filesystem::path interaction_report;
  std::filesystem::path ui_contract_report;
  std::filesystem::path device_recovery_report;
  std::string visual_state;
};

// The Slint shell's unscaled 8 px native inset becomes 6.4 dp inside its
// persisted 125% content zoom (8 / 1.25).  Keeping this conversion explicit
// prevents the whole RmlUi surface from drifting by one or two device pixels.
constexpr float legacy_frame_inset = 6.4f;
constexpr float legacy_launcher_width = 219.f;

struct BackendConnection {
  std::optional<tokmon::DaemonConnection> connection;
  std::optional<tokmon::DaemonClientLease> lease;
  std::string error;
};

bool parse_viewport(const std::string_view value, int& width, int& height) {
  const auto separator = value.find_first_of("xX");
  if (separator == std::string_view::npos)
    return false;
  try {
    const auto parsed_width = std::stoi(std::string(value.substr(0, separator)));
    const auto parsed_height = std::stoi(std::string(value.substr(separator + 1)));
    if (parsed_width < 980 || parsed_height < 620 || parsed_width > 3840 ||
        parsed_height > 2160)
      return false;
    width = parsed_width;
    height = parsed_height;
    return true;
  } catch (...) {
    return false;
  }
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--workspace" && index + 1 < argc)
      result.workspace = std::filesystem::absolute(argv[++index]);
    else if (argument == "--app-data-root" && index + 1 < argc)
      result.app_data_root = std::filesystem::absolute(argv[++index]);
    else if (argument == "--smoke-test")
      result.smoke_test = true;
    else if (argument == "--software-renderer")
      result.software_renderer = true;
    else if (argument == "--idle-test-ms" && index + 1 < argc)
      result.idle_test_ms = std::clamp(std::stoi(argv[++index]), 500, 10000);
    else if (argument == "--ui-scale" && index + 1 < argc)
      result.ui_scale_percent = std::clamp(std::stoi(argv[++index]), 70, 200);
    else if (argument == "--content-scale" && index + 1 < argc)
      result.content_scale_percent =
          std::clamp(std::stoi(argv[++index]), 70, 200);
    else if (argument == "--viewport" && index + 1 < argc) {
      const std::string_view viewport(argv[++index]);
      if (!parse_viewport(viewport, result.logical_width, result.logical_height))
        throw std::invalid_argument(
            "--viewport must be WIDTHxHEIGHT within 980x620..3840x2160");
    }
    else if (argument == "--screenshot" && index + 1 < argc)
      result.screenshot = std::filesystem::absolute(argv[++index]);
    else if (argument == "--acceptance-report" && index + 1 < argc)
      result.acceptance_report = std::filesystem::absolute(argv[++index]);
    else if (argument == "--interaction-report" && index + 1 < argc)
      result.interaction_report = std::filesystem::absolute(argv[++index]);
    else if (argument == "--ui-contract-report" && index + 1 < argc)
      result.ui_contract_report = std::filesystem::absolute(argv[++index]);
    else if (argument == "--device-recovery-report" && index + 1 < argc)
      result.device_recovery_report = std::filesystem::absolute(argv[++index]);
    else if (argument == "--visual-state" && index + 1 < argc)
      result.visual_state = argv[++index];
  }
  return result;
}

int stored_content_scale_percent(const DeskAppPaths& paths,
                                 const int fallback) {
  std::string warning;
  const auto settings = DeskStateStore(paths).load_settings(warning);
  const auto* value = tokmon::cbor::find(settings, "ui_scale");
  if (!value || !std::holds_alternative<std::int64_t>(value->data))
    return fallback;
  return static_cast<int>(std::clamp<std::int64_t>(
      value->as_integer(), 70, 200));
}

std::optional<std::filesystem::path> readable_workspace(
    const std::string_view encoded) {
  if (encoded.empty() || encoded.size() > 4096 ||
      encoded.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::error_code error;
  auto path = std::filesystem::weakly_canonical(
      std::filesystem::path(std::string(encoded)), error);
  if (error || !std::filesystem::is_directory(path, error) || error)
    return std::nullopt;
  return path;
}

std::filesystem::path restored_workspace(const DeskAppPaths& paths) {
  DeskStateStore state(paths);
  std::string warning;
  const auto settings = state.load_settings(warning);
  if (const auto* value = tokmon::cbor::find(settings, "last_workspace"))
    if (const auto restored = readable_workspace(value->as_string()))
      return *restored;

  // Older tokmon-desk builds did not persist last_workspace. Recover it from
  // the selected navigation row and its nearest project ancestor instead.
  const auto navigation = state.load_navigation(warning);
  if (const auto* items = navigation.as_array()) {
    std::optional<std::filesystem::path> first_project;
    std::optional<std::filesystem::path> active_project;
    for (const auto& item : *items) {
      const auto* kind = tokmon::cbor::find(item, "kind");
      const auto* workspace = tokmon::cbor::find(item, "workspace");
      if (kind && kind->as_string() == "project" && workspace) {
        active_project = readable_workspace(workspace->as_string());
        if (!first_project && active_project)
          first_project = active_project;
      }
      const auto* selected = tokmon::cbor::find(item, "selected");
      if (selected && selected->as_bool()) {
        if (workspace)
          if (const auto direct = readable_workspace(workspace->as_string()))
            return *direct;
        if (active_project)
          return *active_project;
      }
    }
    if (first_project)
      return *first_project;
  }
  return std::filesystem::current_path();
}

void set_text(Rml::ElementDocument& document, const char* id, const std::string& value) {
  if (auto* element = document.GetElementById(id))
    element->SetInnerRML(value);
}

std::size_t descendant_count(const Rml::Element& element) {
  std::size_t result = static_cast<std::size_t>(
      std::max(0, element.GetNumChildren()));
  for (int index = 0; index < element.GetNumChildren(); ++index)
    if (const auto* child = element.GetChild(index))
      result += descendant_count(*child);
  return result;
}

std::string json_escape(const std::string_view value) {
  std::ostringstream result;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
    case '\"': result << "\\\""; break;
    case '\\': result << "\\\\"; break;
    case '\b': result << "\\b"; break;
    case '\f': result << "\\f"; break;
    case '\n': result << "\\n"; break;
    case '\r': result << "\\r"; break;
    case '\t': result << "\\t"; break;
    default:
      if (character < 0x20u) {
        constexpr char digits[] = "0123456789abcdef";
        result << "\\u00" << digits[character >> 4u]
               << digits[character & 0x0fu];
      } else {
        result << static_cast<char>(character);
      }
    }
  }
  return result.str();
}

struct InteractionCheck {
  std::string id;
  std::string description;
  bool passed{false};
  std::string detail;
};

bool click_element(Rml::Context& context, Rml::Element* element,
                   std::string& detail) {
  if (!element) {
    detail = "element is missing";
    return false;
  }
  context.Update();
  const auto offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
  const auto width = element->GetOffsetWidth();
  const auto height = element->GetOffsetHeight();
  if (width <= 0.f || height <= 0.f) {
    detail = "element has no clickable geometry";
    return false;
  }
  const int x = static_cast<int>(std::lround(offset.x + width * 0.5f));
  const int y = static_cast<int>(std::lround(offset.y + height * 0.5f));
  const bool moved = context.ProcessMouseMove(x, y, 0);
  const auto* hit = context.GetElementAtPoint(
      {static_cast<float>(x), static_cast<float>(y)});
  bool hit_target = false;
  for (const auto* cursor = hit; cursor; cursor = cursor->GetParentNode()) {
    if (cursor == element) {
      hit_target = true;
      break;
    }
  }
  const std::string hit_description = hit
      ? (hit->GetTagName() + "#" + hit->GetId()) : std::string("<none>");
  const bool pressed = context.ProcessMouseButtonDown(0, 0);
  const bool released = context.ProcessMouseButtonUp(0, 0);
  context.Update();
  detail = "click at " + std::to_string(x) + "," + std::to_string(y) +
           " (move=" + (moved ? "handled" : "unhandled") +
           ", down=" + (pressed ? "handled" : "unhandled") +
           ", up=" + (released ? "handled" : "unhandled") +
           ", hit=" + hit_description + ")";
  return hit_target;
}

bool prepare_visual_state(Rml::Context& context,
                          Rml::ElementDocument& document,
                          DeskViewModel& view_model,
                          DeskController& controller,
                          const std::string_view state,
                          std::string& error) {
  context.Update();
  std::string detail;
  const auto click = [&](const char* id) {
    return click_element(context, document.GetElementById(id), detail);
  };
  const auto open_right = [&] {
    auto* panel = document.GetElementById("right-panel");
    return panel && (!panel->IsClassSet("hidden") || click("right-restore"));
  };
  const auto fullscreen_right = [&] {
    auto* shell = document.GetElementById("app-shell");
    return shell && (shell->IsClassSet("right-fullscreen") ||
                     click("right-fullscreen"));
  };
  // Automated captures must not inherit the host cursor position. Otherwise
  // the same requested state changes whenever the hidden test window happens
  // to open under the pointer (most visibly, a starter card gains its hover
  // accent in the default golden).
  if (state != "hover" && state != "pressed") {
    context.ProcessMouseLeave();
    context.Update();
  }
  if (state.empty() || state == "default")
    return true;
  if (state == "legacy-reference") {
    auto& view = view_model.state();
    view.workspace_name = "tokmon-desk-work-test";
    view.session_title = "E2E 交互回归 已重命名";
    view.active_model = "bigmodel2 · glm-4.7⌄";
    view.effort = "高";
    view.access_label = "完全访问";
    view.branch = "main";
    view.navigation.clear();
    const auto add_navigation = [&](std::string id, std::string title,
                                    std::string kind, const int indent,
                                    const bool selected = false) {
      const auto icon = kind == "group" ? "icon-06.svg" :
          kind == "project" ? "icon-08.svg" : "icon-09.svg";
      view.navigation.push_back({
          std::move(id), std::move(title),
          kind + (selected ? " selected" : ""),
          std::to_string(10.5 + indent * 16) + "dp",
          kind == "session" ? "" : "⌄",
          view.asset_root + "/figma/" + icon,
          kind != "session", true,
          kind == "group", kind == "project", kind == "session",
      });
    };
    add_navigation("legacy-group-content", "内容生产", "group", 0);
    add_navigation("legacy-project-caption", "字幕制作空间", "project", 1);
    add_navigation("legacy-session-audio", "生成音频时间轴字幕", "session", 2);
    add_navigation("legacy-session-align", "字幕校对优化", "session", 2);
    add_navigation("legacy-session-quality", "批量字幕质检优化", "session", 2);
    add_navigation("legacy-session-hi", "hi", "session", 2);
    add_navigation("legacy-session-new", "新会话", "session", 2);
    add_navigation("legacy-session-e2e", "E2E 完整验收会话", "session", 2);
    add_navigation("legacy-project-clips", "音频切片处理", "project", 1);
    add_navigation("legacy-group-demo", "演示助手", "group", 0);
    add_navigation("legacy-project-ppt", "PPT 智绘项目", "project", 1);
    add_navigation("legacy-session-outline", "PPT 大纲生成", "session", 2);
    add_navigation("legacy-session-speech", "演讲稿润色", "session", 2);
    add_navigation("legacy-group-trip", "旅行计划", "group", 0);
    add_navigation("legacy-group-acceptance", "UI验收分组", "group", 0);
    add_navigation("legacy-project-acceptance", "UI验收项目", "project", 1);
    add_navigation("legacy-session-acceptance", "UI验收会话标题", "session", 2);
    add_navigation("legacy-session-7", "新会话 7", "session", 2);
    add_navigation("legacy-session-8", "新会话 8", "session", 2);
    add_navigation("legacy-session-9", "新会话 9", "session", 2);
    add_navigation("legacy-project-switch", "工作空间切换项目", "project", 1);
    add_navigation("legacy-session-inherit", "继承工作空间会话", "session", 2);
    add_navigation("legacy-session-cover", "覆盖工作空间会话", "session", 2);
    add_navigation("legacy-session-switch-new", "新会话", "session", 2);
    add_navigation("legacy-project-current", "tokmon-desk-work-test", "project", 0);
    add_navigation("legacy-session-current-a", "新会话", "session", 1);
    add_navigation("legacy-session-current-b", "新会话", "session", 1);
    add_navigation("legacy-session-current-c", "新会话", "session", 1);
    add_navigation("legacy-session-current", "E2E 交互回归 已重命名", "session", 1,
                   true);
    view.navigation_empty = false;
    view_model.dirty();
    context.Update();
    return true;
  }
  if (state == "hover" || state == "pressed") {
    auto* button = document.GetElementById("new-session-button");
    if (!button) {
      error = "new-session-button is missing";
      return false;
    }
    const auto offset = button->GetAbsoluteOffset(Rml::BoxArea::Border);
    const int x = static_cast<int>(std::lround(
        offset.x + button->GetOffsetWidth() * 0.5f));
    const int y = static_cast<int>(std::lround(
        offset.y + button->GetOffsetHeight() * 0.5f));
    context.ProcessMouseMove(x, y, 0);
    if (state == "pressed")
      context.ProcessMouseButtonDown(0, 0);
    context.Update();
    return true;
  }
  if (state == "focus") {
    if (auto* composer = document.GetElementById("composer")) {
      composer->Focus();
      context.Update();
      return true;
    }
  } else if (state == "selected") {
    return open_right() && click("launcher-files");
  } else if (state == "disabled") {
    if (auto* send = document.GetElementById("send-button")) {
      send->SetAttribute("disabled", "");
      send->SetClass("acceptance-disabled", true);
      return true;
    }
  } else if (state == "loading") {
    if (!open_right() || !click("launcher-review"))
      return false;
    auto& view = view_model.state();
    view.review_has_files = false;
    view.review_title = "正在载入工作区更改…";
    view.review_detail = "Git 状态与差异在后台计算，界面保持可交互";
    view_model.dirty();
    context.Update();
    return true;
  } else if (state == "success" || state == "warning" || state == "error") {
    if (!open_right())
      return false;
    if (auto* toast = document.GetElementById("right-toast")) {
      toast->SetClass("hidden", false);
      toast->SetClass("acceptance-warning", state == "warning");
      toast->SetClass("acceptance-error", state == "error");
      if (state == "success")
        toast->SetInnerRML("✓ 更改已安全保存");
      else if (state == "warning")
        toast->SetInnerRML("! 检测到外部文件修改，请先确认");
      else
        toast->SetInnerRML("× 操作失败，文件内容未被覆盖");
      return true;
    }
  } else if (state == "conversation") {
    controller.seed_acceptance_conversation(3);
    context.Update();
    return true;
  } else if (state == "trajectory-empty") {
    return click("trajectory-mode");
  } else if (state == "trajectory" || state == "trajectory-error") {
    controller.seed_acceptance_trajectory(state == "trajectory-error");
    context.Update();
    return click("trajectory-mode");
  } else if (state == "environment") {
    return click("environment-toggle");
  } else if (state == "new-session") {
    return click("new-session-button");
  } else if (state == "right-review") {
    return open_right() && click("launcher-review");
  } else if (state == "right-files") {
    return open_right() && click("launcher-files");
  } else if (state == "right-terminal") {
    return open_right() && click("add-tab-button") && click("terminal-tab");
  } else if (state == "review-diff" || state == "review-split") {
    if (!open_right() || !click("launcher-review"))
      return false;
    auto* surface = dynamic_cast<ElementDiffSurface*>(
        document.GetElementById("diff-surface"));
    if (!surface) {
      error = "diff-surface is missing";
      return false;
    }
    GitFileDiff diff;
    diff.path = "src/agent/session.cpp";
    diff.hunks.push_back({
        .index = 0,
        .old_start = 38,
        .old_lines = 4,
        .new_start = 38,
        .new_lines = 6,
        .header = "@@ -38,4 +38,6 @@ void Session::resume()",
        .lines = {
            {' ', 38, 38, "  restore_workspace();"},
            {'-', 39, -1, "  refresh();"},
            {'+', -1, 39, "  refresh_navigation();"},
            {'+', -1, 40, "  refresh_review_async();"},
            {' ', 40, 41, "  focus_composer();"},
            {' ', 41, 42, "}"},
        },
    });
    surface->set_diff(std::move(diff));
    surface->set_split_view(state == "review-split");
    auto& view = view_model.state();
    view.diff_visible = true;
    view.diff_path = "src/agent/session.cpp";
    view.diff_summary = "+2 −1";
    view.diff_hunks = {{"src/agent/session.cpp", "0",
                        "@@ -38,4 +38,6 @@ void Session::resume()",
                        "stage-hunk", "暂存", true}};
    view.diff_error_visible = false;
    view_model.dirty();
    if (!fullscreen_right()) {
      error = "right panel did not enter fullscreen";
      return false;
    }
    context.Update();
    return true;
  } else if (state == "files-editor") {
    if (!open_right() || !click("launcher-files"))
      return false;
    auto* tree = dynamic_cast<ElementFileTree*>(
        document.GetElementById("file-tree"));
    auto* editor = dynamic_cast<ElementCodeSurface*>(
        document.GetElementById("file-preview"));
    if (!tree || !editor) {
      error = "file editor surfaces are missing";
      return false;
    }
    tree->set_rows({
        {"src", "src", "src", 0, true, true},
        {"src/app", "src/app", "app", 1, true, true},
        {"src/app/desk_app.cpp", "src/app/desk_app.cpp", "desk_app.cpp", 2,
         false, false},
        {"src/ui", "src/ui", "ui", 1, true, true},
        {"src/ui/desk_controller.cpp", "src/ui/desk_controller.cpp",
         "desk_controller.cpp", 2, false, false},
        {"README.md", "README.md", "README.md", 0, false, false},
    });
    tree->set_selected("src/app/desk_app.cpp");
    editor->set_document(
        "#include <string>\n\nnamespace tokmon::desk {\n\n"
        "std::string greeting(std::string name) {\n"
        "  return \"Hello, \" + name;\n}\n\n} // namespace tokmon::desk\n",
        {}, 1);
    auto& view = view_model.state();
    view.file_open = true;
    view_model.dirty();
    set_text(document, "file-path", "src/app/desk_app.cpp");
    set_text(document, "syntax-status", "C++ · 已保存");
    if (!fullscreen_right()) {
      error = "right panel did not enter fullscreen";
      return false;
    }
    context.Update();
    return true;
  } else if (state == "terminal-content") {
    if (!open_right() || !click("add-tab-button") || !click("terminal-tab"))
      return false;
    // The visual fixture must not race the real shell's startup prompt. Close
    // the just-created PTY while keeping the terminal view active, then render
    // the deterministic snapshot below into the replacement idle tab.
    if (!click("terminal-close-tab")) {
      error = "terminal fixture could not stop the live PTY";
      return false;
    }
    auto* surface = dynamic_cast<ElementTerminal*>(
        document.GetElementById("terminal-surface"));
    if (!surface) {
      error = "terminal-surface is missing";
      return false;
    }
    if (!fullscreen_right()) {
      error = "right panel did not enter fullscreen";
      return false;
    }
    // Let the terminal controller consume the settled fullscreen box first.
    // Otherwise its next update correctly resizes Ghostty and overwrites this
    // deterministic visual fixture with the replacement idle tab.
    context.Update();
    (void)controller.update();
    context.Update();
    TerminalRenderSnapshot snapshot;
    const float fixture_density = std::max(
        context.GetDensityIndependentPixelRatio(), 0.5f);
    snapshot.columns = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(surface->GetClientWidth() /
                         (8.f * fixture_density)), 20, 400));
    snapshot.rows = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(surface->GetClientHeight() /
                         (17.f * fixture_density)), 5, 200));
    snapshot.default_foreground = {
        legacy_theme::body.red,
        legacy_theme::body.green,
        legacy_theme::body.blue};
    snapshot.default_background = {
        legacy_theme::surface_warm.red,
        legacy_theme::surface_warm.green,
        legacy_theme::surface_warm.blue};
    snapshot.cursor_color = {
        legacy_theme::accent.red,
        legacy_theme::accent.green,
        legacy_theme::accent.blue};
    snapshot.cells.resize(static_cast<std::size_t>(snapshot.columns) *
                          snapshot.rows);
    for (auto& cell : snapshot.cells) {
      cell.foreground = snapshot.default_foreground;
      cell.background = snapshot.default_background;
    }
    const std::vector<std::string> lines = {
        "PowerShell 7.5 - tokmon-desk integrated terminal",
        "PS tokmon-desk-work-test> git status --short",
        " M docs/e2e-editor-ui.txt",
        "PS tokmon-desk-work-test> tokmon doctor",
        "OK workspace and daemon paths are healthy",
    };
    for (std::size_t row = 0; row < lines.size(); ++row) {
      std::size_t column = 0;
      for (const char raw_character : lines[row]) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (column >= snapshot.columns)
          break;
        auto& cell = snapshot.cells[row * snapshot.columns + column++];
        cell.grapheme.assign(1, static_cast<char>(character));
        cell.foreground = row == 4 ? TerminalColor{
                                         legacy_theme::green.red,
                                         legacy_theme::green.green,
                                         legacy_theme::green.blue}
                                   : snapshot.default_foreground;
        cell.background = snapshot.default_background;
      }
    }
    snapshot.cursor = {3, 6, true, false, TerminalCursor::Style::bar};
    surface->set_snapshot(std::move(snapshot));
    set_text(document, "terminal-status", "PowerShell · 正在运行");
    context.Update();
    return true;
  } else if (state == "commit-modal" || state == "discard-modal" ||
             state == "terminal-paste-modal") {
    const char* id = state == "commit-modal" ? "commit-overlay" :
        (state == "discard-modal" ? "discard-overlay" :
                                    "terminal-paste-overlay");
    if (auto* overlay = document.GetElementById(id)) {
      overlay->SetClass("hidden", false);
      context.Update();
      return true;
    }
  } else if (state == "slash-popover") {
    auto* composer = dynamic_cast<Rml::ElementFormControl*>(
        document.GetElementById("composer"));
    if (!composer) {
      error = "composer is missing";
      return false;
    }
    composer->SetValue("/");
    composer->DispatchEvent("input", {}, false);
    context.Update();
    return view_model.state().slash_visible;
  } else if (state == "access-popover") {
    return click("access-button");
  } else if (state == "effort-popover") {
    return click("effort-button");
  } else if (state == "chat-running" || state == "chat-stopping") {
    controller.seed_acceptance_chat_state(true, state == "chat-stopping");
    context.Update();
    return true;
  } else if (state == "navigation-context") {
    controller.seed_acceptance_navigation_context();
    context.Update();
    return true;
  } else if (state == "file-operation-modal") {
    return open_right() && click("launcher-files") && click("file-new");
  } else if (state.starts_with("settings-")) {
    if (!click("settings-button"))
      return false;
    const auto page = state.substr(std::string_view("settings-").size());
    Rml::ElementList pages;
    document.QuerySelectorAll(pages, "[setting-page]");
    for (auto* item : pages)
      if (item->GetAttribute<Rml::String>("setting-page", "") == page)
        return click_element(context, item, detail);
  }
  error = "unknown or unavailable visual state: " + std::string(state) +
          (detail.empty() ? "" : " (" + detail + ")");
  return false;
}

bool write_interaction_report(SdlPlatform& platform,
                              Rml::Context& context,
                              Rml::ElementDocument& document,
                              DeskController& controller,
                              const std::filesystem::path& workspace,
                              const DeskAppPaths& app_paths,
                              const std::filesystem::path& path,
                              std::string_view renderer,
                              std::string& error) {
  std::vector<InteractionCheck> checks;
  auto record = [&](std::string id, std::string description, const bool passed,
                    std::string detail) {
    checks.push_back({std::move(id), std::move(description), passed,
                      std::move(detail)});
  };
  auto click_id = [&](const char* id, std::string& detail) {
    return click_element(context, document.GetElementById(id), detail);
  };
  auto hidden = [&](const char* id) {
    const auto* element = document.GetElementById(id);
    return element && element->IsClassSet("hidden");
  };
  auto active = [&](const char* id) {
    const auto* element = document.GetElementById(id);
    return element && element->IsClassSet("active");
  };
  const auto settle = [&] (const int milliseconds = 1500) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    do {
      (void)controller.update();
      context.Update();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
  };

  std::string detail;
  const auto unique = std::to_string(static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto scan_hit_box = [&context](const float left, const float top,
                                       const float right, const float bottom,
                                       const float step, auto&& predicate)
      -> Rml::Element* {
    for (float y = top; y <= bottom; y += step) {
      for (float x = left; x <= right; x += step) {
        auto* hit = context.GetElementAtPoint({x, y});
        for (auto* cursor = hit; cursor; cursor = cursor->GetParentNode())
          if (predicate(*cursor))
            return cursor;
      }
    }
    return nullptr;
  };
  auto* shell_bounds = document.GetElementById("app-shell");
  auto* titlebar = document.GetElementById("workspace-titlebar");
  auto* global_controls = document.GetElementById("global-window-controls");
  const bool window_shell_complete = shell_bounds && titlebar && global_controls &&
      document.GetElementById("minimize-button") &&
      document.GetElementById("maximize-button") &&
      document.GetElementById("close-button") &&
      std::abs(titlebar->GetOffsetHeight() /
                   std::max(context.GetDensityIndependentPixelRatio(), 0.01f) -
               46.f) <= 1.f;
  record("UI-001/002", "无边框三栏窗口壳、拖拽标题区和窗口控件完整",
         window_shell_complete,
         "titlebar=" + std::to_string(titlebar ? titlebar->GetOffsetHeight() : -1));

  auto* navigation_search = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("navigation-search"));
  if (navigation_search) {
    navigation_search->SetValue("绝无匹配的验收查询");
    navigation_search->DispatchEvent("input", {}, false);
    context.Update();
  }
  const bool navigation_no_result = navigation_search &&
      document.GetElementById("navigation-tree") &&
      document.GetElementById("navigation-tree")->GetInnerRML().find(
          "没有匹配") != std::string::npos;
  if (navigation_search) {
    navigation_search->SetValue("");
    navigation_search->DispatchEvent("input", {}, false);
    context.Update();
  }
  Rml::ElementList navigation_rows;
  document.QuerySelectorAll(navigation_rows, "[nav-id]");
  record("UI-004/005-filter", "导航搜索过滤、无结果与清空恢复层级",
         navigation_no_result && navigation_rows.size() >= 11,
         "rows-after-clear=" + std::to_string(navigation_rows.size()));
  auto* navigation_tree = document.GetElementById("navigation-tree");
  const float navigation_width = navigation_tree
      ? navigation_tree->GetOffsetWidth() : 0.f;
  bool navigation_rows_full_width = navigation_width > 0.f &&
      !navigation_rows.empty();
  float narrowest_navigation_row = navigation_width;
  for (auto* row : navigation_rows) {
    narrowest_navigation_row = std::min(
        narrowest_navigation_row, static_cast<float>(row->GetOffsetWidth()));
    navigation_rows_full_width = navigation_rows_full_width &&
        row->GetOffsetWidth() >= navigation_width - 2.f;
  }
  record("UI-004/005-geometry",
         "导航行覆盖树容器全宽，悬停、焦点和选中背景不会残缺",
         navigation_rows_full_width,
         "tree-width=" + std::to_string(navigation_width) +
             ", narrowest-row=" + std::to_string(narrowest_navigation_row));
  bool navigation_keyboard_activated = false;
  std::string navigation_keyboard_detail = "no navigation row";
  if (!navigation_rows.empty()) {
    const auto keyboard_target_id = navigation_rows.back()->GetAttribute<
        Rml::String>("nav-id", "");
    navigation_rows.back()->Focus();
    const bool key_down = context.ProcessKeyDown(Rml::Input::KI_SPACE, 0);
    const bool key_up = context.ProcessKeyUp(Rml::Input::KI_SPACE, 0);
    context.Update();
    Rml::ElementList rows_after_keyboard;
    document.QuerySelectorAll(rows_after_keyboard, "[nav-id]");
    for (const auto* row : rows_after_keyboard) {
      if (row->GetAttribute<Rml::String>("nav-id", "") ==
              keyboard_target_id &&
          row->IsClassSet("selected")) {
        navigation_keyboard_activated = true;
        break;
      }
    }
    navigation_keyboard_detail = "down=" + std::to_string(key_down) +
        ", up=" + std::to_string(key_up) +
        ", target=" + std::string(keyboard_target_id);
  }
  record("UI-005/045-keyboard",
         "导航行可聚焦并由 Space 激活，选中态覆盖整行",
         navigation_keyboard_activated, navigation_keyboard_detail);

  auto* focus_start = document.GetElementById("new-session-button");
  if (focus_start)
    focus_start->Focus(true);
  const auto* focus_before_tab = context.GetFocusElement();
  context.ProcessKeyDown(Rml::Input::KI_TAB, 0);
  context.ProcessKeyUp(Rml::Input::KI_TAB, 0);
  context.Update();
  const auto* focus_after_tab = context.GetFocusElement();
  context.ProcessKeyDown(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT);
  context.ProcessKeyUp(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT);
  context.Update();
  const auto* focus_after_reverse = context.GetFocusElement();
  record("UI-045-tab", "Tab/Shift+Tab 在可见控件间正反向移动键盘焦点",
         focus_before_tab && focus_after_tab &&
             focus_after_tab != focus_before_tab &&
             focus_after_reverse == focus_before_tab,
         "before=" + std::string(focus_before_tab
             ? focus_before_tab->GetId() : "<none>") +
         "; after=" + std::string(focus_after_tab
             ? focus_after_tab->GetId() : "<none>") +
         "; reverse=" + std::string(focus_after_reverse
             ? focus_after_reverse->GetId() : "<none>"));

  auto right_click = [&context](Rml::Element* target,
                                std::string& right_detail) {
    if (!target)
      return false;
    const auto offset = target->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto x = static_cast<int>(std::lround(
        offset.x + target->GetOffsetWidth() * 0.72f));
    const auto y = static_cast<int>(std::lround(
        offset.y + target->GetOffsetHeight() * 0.5f));
    context.ProcessMouseMove(x, y, 0);
    context.ProcessMouseButtonDown(1, 0);
    context.ProcessMouseButtonUp(1, 0);
    context.Update();
    right_detail = "right click at " + std::to_string(x) + "," +
                   std::to_string(y);
    return true;
  };
  Rml::ElementList context_rows;
  document.QuerySelectorAll(context_rows, "[nav-id]");
  Rml::Element* context_session = nullptr;
  for (auto* row : context_rows)
    if (row->IsClassSet("session")) {
      context_session = row;
      break;
    }
  const auto session_count_before = static_cast<std::size_t>(
      std::ranges::count_if(context_rows, [](const auto* row) {
        return row->IsClassSet("session");
      }));
  const bool context_opened = right_click(context_session, detail) &&
      !hidden("navigation-context-menu");
  const bool context_created = context_opened &&
      click_id("navigation-context-new", detail);
  context.Update();
  Rml::ElementList rows_after_context_create;
  document.QuerySelectorAll(rows_after_context_create, "[nav-id]");
  Rml::Element* created_context_session = nullptr;
  for (auto* row : rows_after_context_create)
    if (row->IsClassSet("session") && row->IsClassSet("selected")) {
      created_context_session = row;
      break;
    }
  const auto session_count_after_create = static_cast<std::size_t>(
      std::ranges::count_if(rows_after_context_create, [](const auto* row) {
        return row->IsClassSet("session");
      }));
  const bool context_delete_opened = right_click(created_context_session, detail) &&
      !hidden("navigation-context-menu");
  const bool context_deleted = context_delete_opened &&
      click_id("navigation-context-delete", detail);
  context.Update();
  Rml::ElementList rows_after_context_delete;
  document.QuerySelectorAll(rows_after_context_delete, "[nav-id]");
  const auto session_count_after_delete = static_cast<std::size_t>(
      std::ranges::count_if(rows_after_context_delete, [](const auto* row) {
        return row->IsClassSet("session");
      }));
  record("UI-005-context", "导航树右键菜单可新建并删除精确会话",
         context_created && context_deleted &&
             session_count_after_create == session_count_before + 1 &&
             session_count_after_delete == session_count_before &&
             hidden("navigation-context-menu"),
         "sessions=" + std::to_string(session_count_before) + "/" +
             std::to_string(session_count_after_create) + "/" +
             std::to_string(session_count_after_delete));

  bool clicked = click_id("environment-toggle", detail);
  record("UI-008/009-a", "环境信息通过真实 RmlUi 点击打开",
         clicked && !hidden("environment-panel"), detail);
  clicked = click_id("environment-close", detail);
  record("UI-008/009-b", "环境信息通过真实 RmlUi 点击关闭",
         clicked && hidden("environment-panel"), detail);

  clicked = click_id("trajectory-mode", detail);
  record("UI-008/015-a", "切换到轨迹页并更新选中态",
         clicked && active("trajectory-mode") && !hidden("trajectory"), detail);
  clicked = click_id("chat-mode", detail);
  record("UI-008/015-b", "切回对话页并恢复选中态",
         clicked && active("chat-mode") && hidden("trajectory"), detail);

  Rml::ElementList starter_cards;
  document.QuerySelectorAll(starter_cards, "[starter-kind]");
  const auto* initial_send = document.GetElementById("send-button");
  const bool initial_empty_send_disabled =
      initial_send && initial_send->IsClassSet("disabled");
  auto* composer = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("composer"));
  const std::array<std::string_view, 4> starter_prefixes{
      "请全面分析", "我想为当前工作空间构建", "请对当前项目代码进行全面审查",
      "请帮我诊断当前工作空间"};
  bool every_starter = starter_cards.size() == starter_prefixes.size();
  for (std::size_t index = 0;
       index < starter_cards.size() && index < starter_prefixes.size(); ++index) {
    const bool starter_clicked = click_element(context, starter_cards[index], detail);
    every_starter = every_starter && starter_clicked && composer &&
        std::string_view(composer->GetValue()).starts_with(starter_prefixes[index]);
  }
  record("UI-010", "四张 Starter 卡片分别填入对应提示词并聚焦 Composer",
         every_starter && composer && composer->IsPseudoClassSet("focus"), detail);
  if (composer) {
    composer->SetValue("");
    composer->Focus();
  }

  const bool slash_text = context.ProcessTextInput("/");
  context.Update();
  auto* composer_popover = document.GetElementById("composer-popover");
  const bool slash_open = composer_popover &&
      !composer_popover->IsClassSet("hidden");
  const bool slash_down = context.ProcessKeyDown(Rml::Input::KI_DOWN, 0);
  context.Update();
  Rml::ElementList slash_rows;
  if (composer_popover)
    composer_popover->QuerySelectorAll(slash_rows, "[command-name]");
  const bool slash_second_selected = slash_rows.size() > 1 &&
      slash_rows[1]->IsClassSet("selected");
  const bool slash_enter = context.ProcessKeyDown(Rml::Input::KI_RETURN, 0);
  context.Update();
  record("UI-013-a", "Slash 命令输入过滤、方向键与 Enter 选择",
         slash_open && slash_second_selected && composer &&
             composer->GetValue() == "/fix " &&
             composer_popover && composer_popover->IsClassSet("hidden"),
         "dispatch text/down/enter=" + std::string(slash_text ? "1" : "0") +
             "/" + (slash_down ? "1" : "0") + "/" +
             (slash_enter ? "1" : "0") + "; open=" +
             (slash_open ? "1" : "0") + "; rows=" +
             std::to_string(slash_rows.size()) + "; second-selected=" +
             (slash_second_selected ? "1" : "0") + "; value=" +
             (composer ? std::string(composer->GetValue()) : "<missing>"));

  if (composer) {
    composer->SetValue("");
    composer->Focus();
  }
  const bool slash_reopen = context.ProcessTextInput("/");
  context.Update();
  const bool slash_escape = context.ProcessKeyDown(Rml::Input::KI_ESCAPE, 0);
  context.Update();
  record("UI-013-b", "Slash 命令 Esc 关闭",
         composer_popover &&
             composer_popover->IsClassSet("hidden"),
         "dispatch text/escape=" + std::string(slash_reopen ? "1" : "0") +
             "/" + (slash_escape ? "1" : "0") + "; hidden=" +
             (composer_popover && composer_popover->IsClassSet("hidden")
                  ? "1" : "0"));
  if (composer)
    composer->SetValue("");

  clicked = click_id("access-button", detail);
  const bool access_popover_open = clicked && composer_popover &&
      !composer_popover->IsClassSet("hidden");
  record("UI-012", "Composer 空输入禁用发送且权限菜单可打开",
         initial_empty_send_disabled && access_popover_open, detail);
  Rml::ElementList access_choices;
  if (composer_popover)
    composer_popover->QuerySelectorAll(access_choices, "[choice-kind]");
  Rml::Element* restricted_choice = nullptr;
  for (auto* choice : access_choices)
    if (choice->GetAttribute<Rml::String>("choice-kind", "") == "access" &&
        choice->GetAttribute<Rml::String>("choice-value", "") == "restricted")
      restricted_choice = choice;
  const bool access_selected = click_element(context, restricted_choice, detail);
  context.Update();
  const auto* access_button = document.GetElementById("access-button");
  const bool access_label_updated = access_button &&
      access_button->GetInnerRML().find("受限访问") != std::string::npos;
  const bool effort_opened = click_id("effort-button", detail);
  Rml::ElementList effort_choices;
  if (composer_popover)
    composer_popover->QuerySelectorAll(effort_choices, "[choice-kind]");
  Rml::Element* highest_choice = nullptr;
  for (auto* choice : effort_choices)
    if (choice->GetAttribute<Rml::String>("choice-kind", "") == "effort" &&
        choice->GetAttribute<Rml::String>("choice-value", "") == "最高")
      highest_choice = choice;
  const bool effort_selected = click_element(context, highest_choice, detail);
  context.Update();
  const auto* effort_button = document.GetElementById("effort-button");
  record("UI-012-popovers", "权限与 effort Popover 选择后即时更新 Composer",
         access_selected && access_label_updated && effort_opened &&
             effort_selected && effort_button &&
             effort_button->GetInnerRML().find("最高") != std::string::npos &&
             composer_popover && composer_popover->IsClassSet("hidden"),
         detail);
  if (composer) {
    composer->SetValue("E2E composer input");
    composer->DispatchEvent("input", {}, false);
    context.Update();
  }
  const auto* enabled_send = document.GetElementById("send-button");
  const bool nonempty_send_enabled = enabled_send &&
      !enabled_send->IsClassSet("disabled");
  if (composer) {
    composer->SetValue("");
    composer->DispatchEvent("input", {}, false);
    context.Update();
  }
  record("UI-012-input", "Composer 输入启用发送、清空后重新禁用",
         nonempty_send_enabled && enabled_send &&
             enabled_send->IsClassSet("disabled"), "input state toggled");
  controller.seed_acceptance_chat_state(true, false);
  context.Update();
  const auto* running_send = document.GetElementById("send-button");
  const bool running_state = running_send &&
      running_send->IsClassSet("running") &&
      !running_send->IsClassSet("disabled") &&
      running_send->GetInnerRML().find("stop-glyph") != std::string::npos;
  controller.seed_acceptance_chat_state(true, true);
  context.Update();
  const bool stopping_state = running_send &&
      running_send->IsClassSet("stopping");
  controller.seed_acceptance_chat_state(false, false);
  context.Update();
  record("UI-012-stop", "运行中发送按钮切换为停止/停止中状态；请求 ID 取消由 Core fixture 验证",
         running_state && stopping_state && running_send &&
             !running_send->IsClassSet("running"),
         "running/stopping/reset=" +
             std::to_string(running_state) + "/" +
             std::to_string(stopping_state) + "/1");

  // Keep the code-bearing first turn inside the virtualized window while the
  // exact clipboard contract is exercised. Stress/tail behavior follows.
  controller.seed_acceptance_conversation(20);
  context.Update();
  (void)controller.update();
  context.Update();
  auto* conversation = document.GetElementById("conversation");
  Rml::ElementList markdown_copy_buttons;
  if (conversation)
    conversation->QuerySelectorAll(markdown_copy_buttons,
                                   "[data-copy-markdown]");
  if (conversation)
    conversation->SetScrollTop(0.f);
  context.Update();
  Rml::Element* visible_message_copy = nullptr;
  std::size_t visible_copy_controls = 0;
  Rml::ElementList assistant_messages;
  if (conversation)
    conversation->QuerySelectorAll(assistant_messages, ".assistant-message");
  Rml::Element* visible_assistant = nullptr;
  for (auto* message : assistant_messages)
    if (message->GetOffsetWidth() > 0.f && message->GetOffsetHeight() > 0.f) {
      visible_assistant = message;
      break;
    }
  if (!visible_assistant && conversation) {
    const auto bounds = conversation->GetAbsoluteOffset(Rml::BoxArea::Content);
    visible_assistant = scan_hit_box(
        bounds.x, bounds.y,
        bounds.x + conversation->GetClientWidth(),
        bounds.y + conversation->GetClientHeight(), 12.f,
        [](Rml::Element& candidate) {
          return candidate.IsClassSet("assistant-message");
        });
  }
  if (visible_assistant) {
    visible_assistant->ScrollIntoView(true);
    context.Update();
    const auto message_offset = visible_assistant->GetAbsoluteOffset(
        Rml::BoxArea::Border);
    context.ProcessMouseMove(
        static_cast<int>(std::lround(message_offset.x +
            visible_assistant->GetOffsetWidth() * 0.5f)),
        static_cast<int>(std::lround(message_offset.y + 2.f)), 0);
    context.Update();
    Rml::ElementList local_copies;
    visible_assistant->QuerySelectorAll(local_copies,
                                        "[data-copy-markdown]");
    for (auto* copy_button : local_copies)
      if (!copy_button->IsClassSet("code-copy") &&
          copy_button->GetOffsetWidth() > 0.f &&
          copy_button->GetOffsetHeight() > 0.f) {
        visible_message_copy = copy_button;
        break;
      }
    if (!visible_message_copy) {
      const auto offset = visible_assistant->GetAbsoluteOffset(
          Rml::BoxArea::Border);
      visible_message_copy = scan_hit_box(
          offset.x - 8.f, offset.y - 36.f,
          offset.x + visible_assistant->GetOffsetWidth() + 8.f,
          offset.y + 36.f, 3.f,
          [](Rml::Element& candidate) {
            return candidate.GetAttribute<Rml::String>(
                       "data-copy-markdown", "").size() &&
                !candidate.IsClassSet("code-copy");
          });
    }
  }
  for (auto* copy_button : markdown_copy_buttons) {
    if (copy_button->GetOffsetWidth() > 0.f &&
        copy_button->GetOffsetHeight() > 0.f)
      ++visible_copy_controls;
    if (!visible_message_copy && !copy_button->IsClassSet("code-copy") &&
        copy_button->GetOffsetWidth() > 0.f &&
        copy_button->GetOffsetHeight() > 0.f)
      visible_message_copy = copy_button;
  }
  if (visible_message_copy) {
    visible_message_copy->ScrollIntoView(true);
    context.Update();
  }
  std::string message_copy_detail;
  bool copy_clicked = !markdown_copy_buttons.empty() &&
      click_element(context, visible_message_copy, message_copy_detail);
  if (!copy_clicked) {
    for (auto* copy_button : markdown_copy_buttons) {
      if (!copy_button->IsClassSet("code-copy") &&
          !copy_button->GetAttribute<Rml::String>(
              "data-copy-markdown", "").empty()) {
        copy_clicked = copy_button->DispatchEvent("click", {}, true);
        context.Update();
        message_copy_detail += "; data-bound proxy click";
        break;
      }
    }
  }
  Rml::String copied_markdown;
  platform.GetClipboardText(copied_markdown);
  Rml::Element* code_copy_button = nullptr;
  Rml::ElementList code_wrappers;
  if (visible_assistant)
    visible_assistant->QuerySelectorAll(code_wrappers, ".code-block-wrap");
  if (visible_assistant && code_wrappers.empty()) {
    const auto offset = visible_assistant->GetAbsoluteOffset(
        Rml::BoxArea::Border);
    if (auto* wrapper = scan_hit_box(
            offset.x, offset.y,
            offset.x + visible_assistant->GetOffsetWidth(),
            offset.y + visible_assistant->GetOffsetHeight(), 8.f,
            [](Rml::Element& candidate) {
              return candidate.IsClassSet("code-block-wrap");
            }))
      code_wrappers.push_back(wrapper);
  }
  for (auto* wrapper : code_wrappers) {
    if (wrapper->GetOffsetWidth() <= 0.f || wrapper->GetOffsetHeight() <= 0.f)
      continue;
    const auto wrapper_offset = wrapper->GetAbsoluteOffset(Rml::BoxArea::Border);
    context.ProcessMouseMove(
        static_cast<int>(std::lround(wrapper_offset.x +
                                     wrapper->GetOffsetWidth() * 0.5f)),
        static_cast<int>(std::lround(wrapper_offset.y + 2.f)), 0);
    context.Update();
    Rml::ElementList local_code_copies;
    wrapper->QuerySelectorAll(local_code_copies, ".code-copy");
    for (auto* copy_button : local_code_copies)
      if (copy_button->GetOffsetWidth() > 0.f &&
          copy_button->GetOffsetHeight() > 0.f) {
        code_copy_button = copy_button;
        break;
      }
    if (!code_copy_button) {
      code_copy_button = scan_hit_box(
          wrapper_offset.x,
          wrapper_offset.y,
          wrapper_offset.x + wrapper->GetOffsetWidth(),
          wrapper_offset.y + std::min(42.f, wrapper->GetOffsetHeight()), 3.f,
          [](Rml::Element& candidate) {
            return candidate.IsClassSet("code-copy") &&
                candidate.GetAttribute<Rml::String>(
                    "data-copy-markdown", "").size();
          });
    }
    if (code_copy_button)
      break;
  }
  for (auto* copy_button : markdown_copy_buttons)
    if (!code_copy_button && copy_button->IsClassSet("code-copy") &&
        copy_button->GetOffsetWidth() > 0.f &&
        copy_button->GetOffsetHeight() > 0.f) {
      code_copy_button = copy_button;
      break;
    }
  if (code_copy_button) {
    code_copy_button->ScrollIntoView(true);
    context.Update();
  }
  std::string code_copy_detail;
  bool code_copy_clicked = click_element(context, code_copy_button,
                                         code_copy_detail);
  if (!code_copy_clicked) {
    for (auto* copy_button : markdown_copy_buttons) {
      if (copy_button->IsClassSet("code-copy") &&
          !copy_button->GetAttribute<Rml::String>(
              "data-copy-markdown", "").empty()) {
        code_copy_clicked = copy_button->DispatchEvent("click", {}, true);
        context.Update();
        code_copy_detail += "; data-bound proxy click";
        break;
      }
    }
  }
  Rml::String copied_code;
  platform.GetClipboardText(copied_code);
  std::string normalized_code = copied_code;
  for (std::size_t offset = 0;
       (offset = normalized_code.find("\r\n", offset)) != std::string::npos;)
    normalized_code.replace(offset, 2, "\n");
  record("UI-014/CHAT-003", "会话 Markdown 整条消息及代码块独立复制纯文本",
         copy_clicked && copied_markdown == "验收会话消息 0" &&
              code_copy_clicked && normalized_code == "int answer = 42;\n",
          "copy-buttons=" + std::to_string(markdown_copy_buttons.size()) +
              "; visible-copy-buttons=" +
              std::to_string(visible_copy_controls) +
              "; visible-assistant=" +
              std::to_string(visible_assistant != nullptr) +
              "; code-wrappers=" + std::to_string(code_wrappers.size()) +
              "; markdown-bytes=" + std::to_string(copied_markdown.size()) +
              "; code-bytes=" + std::to_string(copied_code.size()) +
              "; code=" + copied_code + "; message-click=" +
              message_copy_detail + "; code-click=" + code_copy_detail);
  controller.seed_acceptance_conversation(120);
  context.Update();
  (void)controller.update();
  context.Update();
  const float tail_before = conversation ? conversation->GetScrollHeight() : 0.f;
  if (conversation)
    conversation->SetScrollTop(tail_before);
  controller.seed_acceptance_conversation(121);
  context.Update();
  (void)controller.update();
  context.Update();
  const bool followed_tail = conversation &&
      conversation->GetScrollTop() + conversation->GetClientHeight() >=
          conversation->GetScrollHeight() - 2.f;
  if (conversation)
    conversation->SetScrollTop(0.f);
  controller.seed_acceptance_conversation(122);
  context.Update();
  (void)controller.update();
  context.Update();
  const bool preserved_manual_scroll = conversation &&
      conversation->GetScrollTop() <= 2.f;
  record("UI-014/CHAT-002", "会话尾部自动跟随且手动离底后不抢滚动",
         followed_tail && preserved_manual_scroll,
         "tail=" + std::to_string(followed_tail) +
             "; manual=" + std::to_string(preserved_manual_scroll));

  const auto export_directory = workspace / "exports";
  const bool export_directory_existed =
      std::filesystem::is_directory(export_directory);
  std::vector<std::filesystem::path> exports_before;
  std::error_code export_error;
  if (std::filesystem::is_directory(export_directory, export_error))
    for (const auto& entry : std::filesystem::directory_iterator(export_directory))
      if (entry.path().filename().string().starts_with("tokmon-trace-"))
        exports_before.push_back(entry.path());
  controller.seed_acceptance_trajectory(true);
  context.Update();
  const bool trajectory_opened = click_id("trajectory-mode", detail);

  Rml::ElementList trajectory_rows;
  Rml::ElementList trajectory_segments;
  document.QuerySelectorAll(trajectory_rows, ".trajectory-cell");
  document.QuerySelectorAll(trajectory_segments, ".trajectory-span");
  const bool trajectory_ledger_complete = trajectory_rows.size() == 8 &&
      trajectory_segments.size() == 8 &&
      document.GetElementById("trajectory-table-scroll") &&
      document.GetElementById("trajectory-search");
  record("UI-015-ledger", "轨迹使用 Turn ledger、三泳道概览与有界事件窗口",
         trajectory_opened && trajectory_ledger_complete,
         "rows=" + std::to_string(trajectory_rows.size()) +
             "; segments=" + std::to_string(trajectory_segments.size()));

  Rml::ElementList failed_rows;
  document.QuerySelectorAll(failed_rows, ".trajectory-cell.failed");
  std::string trajectory_detail_click;
  const bool failed_selected = failed_rows.size() == 1 &&
      click_element(context, failed_rows.front(), trajectory_detail_click);
  auto* detail_panel = document.GetElementById("trajectory-detail-close");
  Rml::ElementList failed_status;
  document.QuerySelectorAll(failed_status, ".trajectory-status.failed");
  record("UI-015-detail-error", "错误事件可选中并打开 FAILED 局部检查器",
         failed_selected && detail_panel && failed_status.size() == 1,
         trajectory_detail_click);

  Rml::Element* payload_tab = nullptr;
  Rml::ElementList detail_tabs;
  document.QuerySelectorAll(detail_tabs, "[trajectory-detail-tab]");
  for (auto* tab : detail_tabs)
    if (tab->GetAttribute<Rml::String>("trajectory-detail-tab", "") ==
        "payload")
      payload_tab = tab;
  const bool payload_clicked = click_element(context, payload_tab, detail);
  Rml::ElementList payload_panels;
  document.QuerySelectorAll(payload_panels, ".trajectory-detail-payload");
  const auto visible_payload_panels = std::ranges::count_if(
      payload_panels, [](Rml::Element* panel) {
        return panel->GetOffsetWidth() > 0.f && panel->GetOffsetHeight() > 0.f;
      });
  Rml::ElementList active_detail_tabs;
  document.QuerySelectorAll(active_detail_tabs,
                            ".trajectory-detail-tabs button.active");
  const bool payload_active = active_detail_tabs.size() == 1 &&
      active_detail_tabs.front()->GetAttribute<Rml::String>(
          "trajectory-detail-tab", "") == "payload";
  record("UI-015-detail-tabs", "事件检查器 Summary/Payload/Result/Schema/Timing 可切换",
         payload_clicked && payload_active && visible_payload_panels == 1,
         "tabs=" + std::to_string(detail_tabs.size()) +
             "; activeTabs=" + std::to_string(active_detail_tabs.size()) +
             "; payloadPanels=" + std::to_string(payload_panels.size()) +
             "/" + std::to_string(visible_payload_panels) + " visible" +
             "; " + detail);

  auto* trajectory_search = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("trajectory-search"));
  if (trajectory_search) {
    trajectory_search->SetValue("tool.result");
    trajectory_search->DispatchEvent("input", {}, false);
    context.Update();
  }
  Rml::ElementList searched_rows;
  document.QuerySelectorAll(searched_rows, ".trajectory-cell");
  const bool search_filtered = trajectory_search && searched_rows.size() == 1 &&
      searched_rows.front()->GetInnerRML().find("tool.result") !=
          std::string::npos;
  if (trajectory_search) {
    trajectory_search->SetValue("");
    trajectory_search->DispatchEvent("input", {}, false);
    context.Update();
  }
  bool filter_clicks = true;
  for (int index = 0; index < 5; ++index)
    filter_clicks = click_id("trajectory-filter", detail) && filter_clicks;
  failed_rows.clear();
  document.QuerySelectorAll(failed_rows, ".trajectory-cell.failed");
  const auto* filter_button = document.GetElementById("trajectory-filter");
  const bool error_filtered = filter_clicks && filter_button &&
      filter_button->GetInnerRML().find("ERROR") != std::string::npos &&
      failed_rows.size() == 1;
  const bool filter_reset = click_id("trajectory-filter", detail);
  record("UI-015-search-filter", "实时搜索和 USER/CONTEXT/ASSISTANT/TOOL/ERROR 筛选有效",
         search_filtered && error_filtered && filter_reset,
         "searchRows=" + std::to_string(searched_rows.size()) +
             "; errorRows=" + std::to_string(failed_rows.size()));

  std::string duration_equal_detail;
  const bool duration_equal_click = click_id(
      "trajectory-duration-toggle", duration_equal_detail);
  const bool duration_equal = duration_equal_click &&
      !active("trajectory-duration-toggle");
  std::string duration_actual_detail;
  const bool duration_actual_click = click_id(
      "trajectory-duration-toggle", duration_actual_detail);
  const bool duration_actual = duration_actual_click &&
      active("trajectory-duration-toggle");
  std::string turns_collapse_detail;
  const bool turns_collapse_click = click_id(
      "trajectory-turns-toggle", turns_collapse_detail);
  const bool turns_collapsed = turns_collapse_click &&
      active("trajectory-turns-toggle");
  Rml::ElementList collapsed_rows;
  document.QuerySelectorAll(collapsed_rows, ".trajectory-cell");
  Rml::ElementList turn_records;
  document.QuerySelectorAll(turn_records, ".trajectory-record");
  const auto hidden_turn_records = std::ranges::count_if(
      turn_records, [](Rml::Element* row) { return row->IsClassSet("hidden"); });
  const auto visible_collapsed_rows = std::ranges::count_if(
      collapsed_rows, [](Rml::Element* row) {
        return row->GetOffsetWidth() > 0.f && row->GetOffsetHeight() > 0.f;
      });
  std::string turns_expand_detail;
  const bool turns_expand_click = click_id(
      "trajectory-turns-toggle", turns_expand_detail);
  const bool turns_expanded = turns_expand_click &&
      !active("trajectory-turns-toggle");
  std::string calls_collapse_detail;
  const bool calls_collapse_click = click_id(
      "trajectory-calls-toggle", calls_collapse_detail);
  const bool calls_collapsed = calls_collapse_click &&
      active("trajectory-calls-toggle");
  Rml::ElementList call_collapsed_rows;
  document.QuerySelectorAll(call_collapsed_rows, ".trajectory-cell");
  Rml::ElementList call_records;
  document.QuerySelectorAll(call_records, ".trajectory-record");
  const auto hidden_call_records = std::ranges::count_if(
      call_records, [](Rml::Element* row) { return row->IsClassSet("hidden"); });
  const auto visible_call_collapsed_rows = std::ranges::count_if(
      call_collapsed_rows, [](Rml::Element* row) {
        return row->GetOffsetWidth() > 0.f && row->GetOffsetHeight() > 0.f;
      });
  std::string calls_expand_detail;
  const bool calls_expand_click = click_id(
      "trajectory-calls-toggle", calls_expand_detail);
  const bool calls_expanded = calls_expand_click &&
      !active("trajectory-calls-toggle");
  record("UI-015-controls", "真实耗时/等宽、Turn 折叠和 Call 折叠均可逆",
         duration_equal && duration_actual && turns_collapsed &&
             hidden_turn_records == 7 && turns_expanded && calls_collapsed &&
             hidden_call_records == 2 && calls_expanded,
         "turnRows=" + std::to_string(collapsed_rows.size()) +
             "/" + std::to_string(visible_collapsed_rows) + " visible" +
             "/" + std::to_string(hidden_turn_records) + " hidden records" +
             "; callRows=" + std::to_string(call_collapsed_rows.size()) +
             "/" + std::to_string(visible_call_collapsed_rows) + " visible" +
             "/" + std::to_string(hidden_call_records) + " hidden records" +
             "; state=" + (duration_equal ? "E" : "-") +
             (duration_actual ? "A" : "-") +
             (turns_collapsed ? "T" : "-") +
             (turns_expanded ? "t" : "-") +
             (calls_collapsed ? "C" : "-") +
             (calls_expanded ? "c" : "-") +
             "; equal=" + duration_equal_detail +
             "; actual=" + duration_actual_detail +
             "; turns-=" + turns_collapse_detail +
             "; turns+=" + turns_expand_detail +
             "; calls-=" + calls_collapse_detail +
             "; calls+=" + calls_expand_detail);

  auto* timeline_track = document.GetElementById("trajectory-lane-track");
  bool timeline_zoomed = false;
  bool timeline_range_focused = false;
  bool timeline_pan_kept_zoom = false;
  bool timeline_reset = false;
  bool first_timeline_escape = false;
  bool second_timeline_escape = false;
  std::size_t timeline_outside_after_reset = 0;
  bool timeline_zoom_visible_after_reset = false;
  if (timeline_track) {
    const auto offset = timeline_track->GetAbsoluteOffset(Rml::BoxArea::Content);
    const auto width = timeline_track->GetClientWidth();
    const auto height = timeline_track->GetClientHeight();
    const int center_y = static_cast<int>(std::lround(offset.y + height * 0.5f));
    const auto point_x = [&](const float fraction) {
      return static_cast<int>(std::lround(offset.x + width * fraction));
    };
    context.ProcessMouseMove(point_x(0.5f), center_y, 0);
    context.ProcessMouseWheel(Rml::Vector2f{0.f, -2.f}, 0);
    context.Update();
    auto* zoom_status = document.GetElementById("trajectory-reset-timeline");
    timeline_zoomed = zoom_status && zoom_status->GetOffsetWidth() > 0.f;

    context.ProcessMouseMove(point_x(0.24f), center_y, 0);
    context.ProcessMouseButtonDown(0, 0);
    context.ProcessMouseMove(point_x(0.68f), center_y, 0);
    context.ProcessMouseButtonUp(0, 0);
    context.Update();
    Rml::ElementList outside_range;
    document.QuerySelectorAll(outside_range, ".trajectory-cell.outside-range");
    timeline_range_focused = !outside_range.empty();

    context.ProcessMouseMove(point_x(0.5f), center_y, 0);
    context.ProcessMouseButtonDown(1, 0);
    context.ProcessMouseMove(point_x(0.62f), center_y, 0);
    context.ProcessMouseButtonUp(1, 0);
    context.Update();
    zoom_status = document.GetElementById("trajectory-reset-timeline");
    timeline_pan_kept_zoom = zoom_status && zoom_status->GetOffsetWidth() > 0.f;

    SDL_Event escape_event{};
    escape_event.type = SDL_EVENT_KEY_DOWN;
    escape_event.key.key = SDLK_ESCAPE;
    first_timeline_escape = controller.handle_raw_event(escape_event);
    second_timeline_escape = controller.handle_raw_event(escape_event);
    context.Update();
    outside_range.clear();
    document.QuerySelectorAll(outside_range, ".trajectory-cell.outside-range");
    timeline_outside_after_reset = outside_range.size();
    zoom_status = document.GetElementById("trajectory-reset-timeline");
    timeline_zoom_visible_after_reset = zoom_status &&
        zoom_status->GetInnerRML().find("1×") == std::string::npos;
    timeline_reset = (first_timeline_escape || second_timeline_escape) &&
        outside_range.empty() &&
        !timeline_zoom_visible_after_reset;
  }
  record("UI-015-timeline", "Overview 支持区间聚焦、滚轮缩放、右键平移和 Esc 复位",
         timeline_track && timeline_zoomed && timeline_range_focused &&
             timeline_pan_kept_zoom && timeline_reset,
         "zoom/range/pan/reset=" +
             std::string(timeline_zoomed ? "1" : "0") + "/" +
             (timeline_range_focused ? "1" : "0") + "/" +
             (timeline_pan_kept_zoom ? "1" : "0") + "/" +
             (timeline_reset ? "1" : "0") +
             "; escapes=" + (first_timeline_escape ? "1" : "0") + "/" +
             (second_timeline_escape ? "1" : "0") +
             "; outside=" + std::to_string(timeline_outside_after_reset) +
             "; zoomVisible=" +
             (timeline_zoom_visible_after_reset ? "1" : "0"));

  const bool trajectory_refreshed = click_id("refresh-trajectory", detail);
  record("UI-015-refresh", "轨迹刷新按钮真实可点击且保持 ledger 可用",
         trajectory_refreshed && document.GetElementById("trajectory-table-scroll"),
         detail);
  auto* export_button = document.GetElementById("export-trajectory");
  if (export_button) {
    export_button->ScrollIntoView(true);
    context.Update();
  }
  std::string export_detail;
  const bool export_clicked = click_element(context, export_button,
                                             export_detail);
  std::vector<std::filesystem::path> generated_exports;
  if (std::filesystem::is_directory(export_directory, export_error))
    for (const auto& entry : std::filesystem::directory_iterator(export_directory))
      if (entry.path().filename().string().starts_with("tokmon-trace-") &&
          std::ranges::find(exports_before, entry.path()) == exports_before.end())
        generated_exports.push_back(entry.path());
  bool export_valid = generated_exports.size() == 1;
  if (export_valid) {
    std::ifstream exported(generated_exports.front(), std::ios::binary);
    std::stringstream exported_text;
    exported_text << exported.rdbuf();
    export_valid = exported_text.str().find("\"ray\": \"acceptance-ray\"") !=
            std::string::npos &&
        exported_text.str().find("\"kind\": \"user.message\"") !=
            std::string::npos &&
        exported_text.str().find("\"kind\": \"assistant.message\"") !=
            std::string::npos;
  }
  for (const auto& generated : generated_exports)
    std::filesystem::remove(generated, export_error);
  if (!export_directory_existed)
    std::filesystem::remove(export_directory, export_error);
  record("UI-015", "轨迹事件、因果摘要与 JSON 导出",
         trajectory_opened && export_clicked && export_valid,
          "generated=" + std::to_string(generated_exports.size()) +
              "; click=" + export_detail);
  const bool returned_to_chat = click_id("chat-mode", detail);
  record("UI-015-return", "轨迹导出后可返回对话并保持消息",
         returned_to_chat && conversation && !conversation->IsClassSet("hidden"),
         detail);

  auto* shell = document.GetElementById("app-shell");
  const bool right_starts_launcher = !hidden("right-panel") &&
      !hidden("right-launcher") && hidden("review-view") && shell &&
      !shell->IsClassSet("right-hidden");
  const bool launcher_review = click_id("launcher-review", detail);
  record("UI-016-startup", "右栏按旧版规则默认显示启动器并可打开审查",
         right_starts_launcher && launcher_review && hidden("right-launcher") &&
             !hidden("review-view"), detail);

  // Resize the compact legacy launcher, not an active feature page. In the
  // real UI an opened Review/File/Terminal surface may intentionally widen
  // the panel, so dragging at the compact launcher edge while Review is open
  // tests an unrelated coordinate rather than the divider.
  const bool review_closed_for_resize = click_id("right-tab-close", detail);

  const auto dispatch_pointer = [&](const Uint32 type, const float logical_x,
                                    const float logical_y) {
    SDL_Event pointer{};
    pointer.type = type;
    const float coordinate_scale = platform.design_coordinate_scale();
    const float physical_x = logical_x / coordinate_scale;
    const float physical_y = logical_y / coordinate_scale;
    if (type == SDL_EVENT_MOUSE_MOTION) {
      pointer.motion.windowID = SDL_GetWindowID(platform.window());
      pointer.motion.state = SDL_BUTTON_LMASK;
      pointer.motion.x = physical_x;
      pointer.motion.y = physical_y;
    } else {
      pointer.button.windowID = SDL_GetWindowID(platform.window());
      pointer.button.button = SDL_BUTTON_LEFT;
      pointer.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      pointer.button.x = physical_x;
      pointer.button.y = physical_y;
    }
    // Exercise the same raw SDL callback installed in SdlPlatform without
    // allowing SDL_CaptureMouse to mix the host's real cursor motion into the
    // deterministic divider drag at fractional DPI scales.
    const bool handled = controller.handle_raw_event(pointer);
    context.Update();
    return handled;
  };
  const auto dispatch_key = [&](const SDL_Keycode key,
                                const SDL_Keymod modifiers = SDL_KMOD_NONE) {
    bool quit = false;
    bool resized = false;
    for (const Uint32 type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
      SDL_Event event{};
      event.type = type;
      event.key.windowID = SDL_GetWindowID(platform.window());
      event.key.down = type == SDL_EVENT_KEY_DOWN;
      event.key.key = key;
      event.key.mod = modifiers;
      if (!SDL_PushEvent(&event))
        return false;
      while (platform.pump_event(context, quit, resized)) {}
      if (quit)
        return false;
    }
    context.Update();
    return true;
  };
  const auto dispatch_text = [&](const Uint32 type, const char* value,
                                 const int start = 0,
                                 const int length = 0) {
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_TEXT_INPUT) {
      event.text.windowID = SDL_GetWindowID(platform.window());
      event.text.text = value;
    } else {
      event.edit.windowID = SDL_GetWindowID(platform.window());
      event.edit.start = start;
      event.edit.length = length;
      event.edit.text = value;
    }
    if (!SDL_PushEvent(&event))
      return false;
    bool quit = false;
    bool resized = false;
    while (platform.pump_event(context, quit, resized)) {}
    context.Update();
    return !quit;
  };
  const auto click_point = [&](Rml::Element* element, const float local_x,
                               const float local_y, std::string& click_detail) {
    if (!element) {
      click_detail = "element is missing";
      return false;
    }
    context.Update();
    const auto offset = element->GetAbsoluteOffset(Rml::BoxArea::Content);
    const int x = static_cast<int>(std::lround(offset.x + local_x));
    const int y = static_cast<int>(std::lround(offset.y + local_y));
    context.ProcessMouseMove(x, y, 0);
    const auto* hit = context.GetElementAtPoint(
        {static_cast<float>(x), static_cast<float>(y)});
    context.ProcessMouseButtonDown(0, 0);
    context.ProcessMouseButtonUp(0, 0);
    context.Update();
    click_detail = "click point " + std::to_string(x) + "," +
        std::to_string(y) + " hit=" +
        (hit ? hit->GetTagName() + "#" + hit->GetId() : "<none>");
    return true;
  };
  auto* sidebar = document.GetElementById("sidebar");
  auto* right_panel = document.GetElementById("right-panel");
  auto* workspace_element = document.GetElementById("workspace");
  const float density = std::max(context.GetDensityIndependentPixelRatio(),
                                 0.01f);
  const auto logical = [density](const float value) {
    return value / density;
  };
  const bool left_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN, legacy_frame_inset + 240.f, 100.f);
  const bool left_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION, legacy_frame_inset + 280.f, 100.f);
  const bool left_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP, legacy_frame_inset + 280.f, 100.f);
  const float left_resized_width = sidebar
      ? logical(sidebar->GetOffsetWidth()) : -1.f;
  const float workspace_left_after_resize = workspace_element
      ? logical(workspace_element->GetAbsoluteOffset(
            Rml::BoxArea::Border).x) : -1.f;
  const bool left_resized = sidebar && workspace_element &&
      std::abs(left_resized_width - 280.f) <= 1.f &&
      std::abs(workspace_left_after_resize -
               legacy_frame_inset - 280.5f) <= 1.f;
  const bool left_restore_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN, legacy_frame_inset + 280.f, 100.f);
  const bool left_restore_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION, legacy_frame_inset + 240.f, 100.f);
  const bool left_restore_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP, legacy_frame_inset + 240.f, 100.f);
  const bool left_restored = sidebar &&
      std::abs(logical(sidebar->GetOffsetWidth()) - 240.f) <= 1.f;
  record("UI-002/006-resize", "左分栏经 SDL 指针事件拖拽并恢复",
         left_down && left_move && left_up && left_resized &&
             left_restore_down && left_restore_move && left_restore_up &&
             left_restored,
         "down/move/up/resized/restore-down/restore-move/restore-up/restored=" +
             std::to_string(left_down) + "/" + std::to_string(left_move) + "/" +
             std::to_string(left_up) + "/" + std::to_string(left_resized) + "/" +
             std::to_string(left_restore_down) + "/" +
             std::to_string(left_restore_move) + "/" +
             std::to_string(left_restore_up) + "/" +
             std::to_string(left_restored) + "; sidebar 240 -> 280 -> " +
             std::to_string(sidebar ? logical(sidebar->GetOffsetWidth()) : -1.f) +
             "; resized-width=" + std::to_string(left_resized_width) +
             ", workspace-left=" +
             std::to_string(workspace_left_after_resize));

  const float viewport_width = logical(document.GetClientWidth());
  const bool right_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN,
      viewport_width - legacy_frame_inset - legacy_launcher_width, 100.f);
  const bool right_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION,
      viewport_width - legacy_frame_inset - 280.f, 100.f);
  const bool right_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP,
      viewport_width - legacy_frame_inset - 280.f, 100.f);
  const float right_resized_width = right_panel
      ? logical(right_panel->GetOffsetWidth()) : -1.f;
  const bool right_resized = right_panel &&
      std::abs(right_resized_width - 280.f) <= 1.f;
  const bool right_restore_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN,
      viewport_width - legacy_frame_inset - 280.f, 100.f);
  const bool right_restore_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION,
      viewport_width - legacy_frame_inset - legacy_launcher_width, 100.f);
  const bool right_restore_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP,
      viewport_width - legacy_frame_inset - legacy_launcher_width, 100.f);
  const bool right_restored = right_panel &&
      std::abs(logical(right_panel->GetOffsetWidth()) -
               legacy_launcher_width) <= 1.f;
  const bool review_reopened_after_resize = click_id("launcher-review", detail);
  record("UI-002/016-resize", "右分栏经 SDL 指针事件拖拽并恢复",
         review_closed_for_resize && right_down && right_move && right_up && right_resized &&
             right_restore_down && right_restore_move && right_restore_up &&
             right_restored && review_reopened_after_resize,
         "close/down/move/up/resized/restore-down/restore-move/restore-up/"
         "restored/reopen=" +
             std::string(review_closed_for_resize ? "1" : "0") + "/" +
             (right_down ? "1" : "0") + "/" + (right_move ? "1" : "0") +
             "/" + (right_up ? "1" : "0") + "/" +
             (right_resized ? "1" : "0") + "/" +
             (right_restore_down ? "1" : "0") + "/" +
             (right_restore_move ? "1" : "0") + "/" +
             (right_restore_up ? "1" : "0") + "/" +
             (right_restored ? "1" : "0") + "/" +
             (review_reopened_after_resize ? "1" : "0") +
             "; right panel 219 -> " + std::to_string(right_resized_width) +
             " -> " +
             std::to_string(right_panel ? logical(right_panel->GetOffsetWidth())
                                        : -1.f));

  clicked = click_id("sidebar-toggle", detail);
  const bool sidebar_collapsed = clicked && hidden("sidebar") && shell &&
      shell->IsClassSet("sidebar-hidden");
  const bool sidebar_restore_click = click_id("sidebar-restore", detail);
  record("UI-006", "左侧栏折叠并恢复",
         sidebar_collapsed && sidebar_restore_click && !hidden("sidebar") &&
             !shell->IsClassSet("sidebar-hidden"),
         detail);

  clicked = click_id("new-session-button", detail);
  const bool new_session_open = clicked && !hidden("new-session-overlay");
  auto* new_session_title = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("new-session-title"));
  if (new_session_title)
    new_session_title->SetValue("");
  const bool invalid_session_confirm =
      click_id("confirm-new-session", detail);
  const auto* new_session_error =
      document.GetElementById("new-session-error");
  const bool invalid_session_rejected = invalid_session_confirm &&
      !hidden("new-session-overlay") && new_session_error &&
      !new_session_error->GetInnerRML().empty();
  const bool new_session_cancel = click_id("cancel-new-session", detail);
  record("UI-003/042", "新建会话弹窗打开、拒绝空标题并取消",
         new_session_open && invalid_session_rejected && new_session_cancel &&
             hidden("new-session-overlay"),
         detail);

  const std::string acceptance_session = "E2E 完整验收会话 " + unique;
  const bool valid_session_open = click_id("new-session-button", detail);
  if (new_session_title)
    new_session_title->SetValue(acceptance_session);
  const bool valid_session_create = click_id("confirm-new-session", detail);
  context.Update();
  const auto* session_title = document.GetElementById("session-title");
  const bool first_session_created = valid_session_open && valid_session_create &&
      hidden("new-session-overlay") && session_title &&
      session_title->GetInnerRML().find(acceptance_session) != std::string::npos;
  const bool duplicate_session_open = click_id("new-session-button", detail);
  if (new_session_title)
    new_session_title->SetValue(acceptance_session);
  const bool duplicate_session_create = click_id("confirm-new-session", detail);
  context.Update();
  navigation_rows.clear();
  document.QuerySelectorAll(navigation_rows, "[nav-id]");
  const auto duplicate_count = std::ranges::count_if(
      navigation_rows, [&](const Rml::Element* row) {
        return row->GetInnerRML().find(acceptance_session) != std::string::npos;
      });
  record("UI-042-create", "新建会话校验、创建及重复名独立会话语义",
         first_session_created && duplicate_session_open &&
             duplicate_session_create && duplicate_count == 2,
         "duplicate-visible-rows=" + std::to_string(duplicate_count));

  const bool rename_open = click_id("title-edit", detail);
  context.Update();
  auto* rename_title = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("rename-title-input"));
  if (rename_title)
    rename_title->SetValue("E2E 交互回归 已重命名");
  const bool rename_confirm = click_id("confirm-title-rename", detail);
  context.Update();
  record("UI-005/008-rename", "会话树与工作区标题同步重命名",
         rename_open && rename_confirm && session_title &&
             session_title->GetInnerRML().find("E2E 交互回归 已重命名") !=
                 std::string::npos,
         detail);

  clicked = click_id("settings-button", detail);
  const bool settings_open = clicked && !hidden("settings-overlay");
  record("UI-007/031", "设置壳通过真实点击打开",
         settings_open, detail);
  Rml::ElementList settings_pages;
  document.QuerySelectorAll(settings_pages, "[setting-page]");
  const std::vector<std::tuple<std::string_view, std::string_view,
                               std::string_view>> settings_contracts{
       {"general", "UI-032", "通用"},
       {"model", "UI-033", "模型"},
       {"agents", "UI-033A", "智能体"},
       {"skills", "UI-033B", "Skill 技能"},
       {"rules", "UI-033C", "规则"},
       {"mcp", "UI-033D", "MCP 服务"},
       {"access", "UI-034", "权限与安全"},
      {"workspace", "UI-035", "工作区"},
      {"notifications", "UI-036", "通知"},
      {"appearance", "UI-037", "外观"},
      {"shortcuts", "UI-038", "快捷键"},
       {"account", "UI-039", "账户"},
       {"terminal", "UI-040", "终端"},
       {"browser", "UI-040B", "浏览器"},
       {"about", "UI-041", "关于"}};
  auto* settings_title = document.GetElementById("settings-title");
  auto* settings_content = document.GetElementById("settings-body");
  for (const auto& [page, id, expected_title] : settings_contracts) {
    Rml::Element* navigation = nullptr;
    for (auto* page_item : settings_pages) {
      if (page_item->GetAttribute<Rml::String>("setting-page", "") == page) {
        navigation = page_item;
        break;
      }
    }
    const bool page_clicked = click_element(context, navigation, detail);
    const bool page_visible = page_clicked && navigation &&
        navigation->IsClassSet("active") && settings_title &&
        settings_title->GetInnerRML().find(expected_title) !=
            std::string::npos &&
        settings_content && settings_content->GetNumChildren() > 0;
    record(std::string(id), "设置/" + std::string(expected_title) +
               " 页面可导航且内容完整",
           page_visible, detail);
  }

  auto* settings_search = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("settings-search"));
  if (settings_search) {
    settings_search->SetValue("");
    settings_search->Focus();
  }
  const bool search_text = context.ProcessTextInput("终端");
  context.Update();
  bool terminal_navigation_active = false;
  for (auto* page_item : settings_pages) {
    const auto page = page_item->GetAttribute<Rml::String>("setting-page", "");
    if (page == "terminal")
      terminal_navigation_active = page_item->IsClassSet("active");
  }
  record("UI-031-search", "设置搜索实时定位本轮非 Browser 页面",
         settings_search && !settings_search->GetValue().empty() &&
             terminal_navigation_active && settings_title &&
             settings_title->GetInnerRML().find("终端") != std::string::npos,
         "dispatch=" + std::string(search_text ? "handled" : "unhandled") +
             "; value=" +
             (settings_search ? std::string(settings_search->GetValue())
                              : "<missing>"));
  Rml::Element* account_navigation = nullptr;
  Rml::Element* about_navigation = nullptr;
  for (auto* page_item : settings_pages) {
    const auto page = page_item->GetAttribute<Rml::String>("setting-page", "");
    if (page == "account") account_navigation = page_item;
    if (page == "about") about_navigation = page_item;
  }
  const bool account_opened = click_element(context, account_navigation, detail);
  auto* nickname = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("setting-nickname"));
  if (nickname) {
    nickname->SetValue("");
    nickname->Focus();
    (void)context.ProcessTextInput("Tokmon Desk E2E");
    context.Update();
  }
  const bool settings_saved = click_id("save-settings", detail);
  std::string stored_warning;
  const auto stored_after_save = DeskStateStore(app_paths).load_settings(
      stored_warning);
  const auto* stored_nickname = tokmon::cbor::find(stored_after_save,
                                                   "nickname");
  const bool local_setting_persisted = stored_nickname &&
      stored_nickname->as_string() == "Tokmon Desk E2E";
  const auto* stored_theme = tokmon::cbor::find(stored_after_save,
                                                "theme_mode");
  const auto* stored_agent = tokmon::cbor::find(stored_after_save,
                                                "agent_autonomous");
  const auto* stored_skills = tokmon::cbor::find(stored_after_save,
                                                 "skills_enabled");
  const auto* stored_rules = tokmon::cbor::find(stored_after_save,
                                                "rules_enabled");
  const auto* stored_mcp = tokmon::cbor::find(stored_after_save,
                                              "mcp_auto_start");
  const bool new_settings_persisted = stored_theme &&
      stored_theme->as_string() == "浅色" && stored_agent &&
      stored_agent->as_bool() && stored_skills && stored_skills->as_bool() &&
      stored_rules && stored_rules->as_bool() && stored_mcp &&
      stored_mcp->as_bool();
  const bool settings_reopened = click_id("settings-button", detail) &&
      click_element(context, account_navigation, detail);
  nickname = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("setting-nickname"));
  const bool value_restored = nickname &&
      nickname->GetValue() == "Tokmon Desk E2E";
  const bool settings_reset = click_id("reset-settings", detail);
  context.Update();
  nickname = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("setting-nickname"));
  const bool reset_visible = nickname && nickname->GetValue().empty();
  const bool reset_saved = click_id("save-settings", detail);
  record("UI-031/032/STATE-003", "Desktop 设置保存、重开恢复、重置并写入私有状态",
         account_opened && settings_saved && local_setting_persisted &&
             new_settings_persisted &&
             settings_reopened && value_restored && settings_reset &&
             reset_visible && reset_saved && hidden("settings-overlay"),
          "account/open/save/persist/new-pages/reopen/restore/reset/visible/save=" +
              std::string(account_opened ? "1" : "0") + "/" +
              (settings_saved ? "1" : "0") + "/" +
              (local_setting_persisted ? "1" : "0") + "/" +
              (new_settings_persisted ? "1" : "0") + "/" +
              (settings_reopened ? "1" : "0") + "/" +
              (value_restored ? "1" : "0") + "/" +
              (settings_reset ? "1" : "0") + "/" +
              (reset_visible ? "1" : "0") + "/" +
              (reset_saved ? "1" : "0") +
              (stored_warning.empty() ? "" : "; " + stored_warning));

  const bool about_opened = click_id("settings-button", detail) &&
      click_element(context, about_navigation, detail);
  const auto notices_txt = std::filesystem::path(SDL_GetBasePath()) /
      "THIRD_PARTY_NOTICES.txt";
  const auto licenses = std::filesystem::path(SDL_GetBasePath()) / "licenses";
  const bool notices_contract = about_opened &&
      document.GetElementById("open-notices") &&
      std::filesystem::is_regular_file(notices_txt) &&
      (std::filesystem::is_directory(licenses) ||
       std::filesystem::is_regular_file(
           std::filesystem::path(SDL_GetBasePath()) /
           "THIRD_PARTY_NOTICES.md"));
  record("UI-041-license", "关于页许可入口及 HarfBuzz 等 notices 随运行态提供",
         notices_contract,
         notices_txt.generic_string());
  const bool settings_close = click_id("close-settings", detail);
  record("UI-007/031-close", "设置通过关闭按钮退出",
         settings_close && hidden("settings-overlay"), detail);

  const bool review_closed_for_files = click_id("right-tab-close", detail);
  clicked = review_closed_for_files && click_id("launcher-files", detail);
  record("UI-017/025", "右栏切换到文件页",
         clicked && hidden("right-tab-menu") && !hidden("files-view"), detail);

  const auto original_name = ".tokmon-desk-e2e-" + unique + ".txt";
  const auto renamed_name = ".tokmon-desk-e2e-" + unique + "-renamed.txt";
  const auto original_path = workspace / original_name;
  const auto renamed_path = workspace / renamed_name;
  bool created = false;
  bool renamed = false;
  bool removed = false;
  if (!std::filesystem::exists(original_path) &&
      !std::filesystem::exists(renamed_path)) {
    const bool operation_opened = click_id("file-new", detail) &&
        !hidden("file-operation-overlay");
    auto* operation_input = dynamic_cast<Rml::ElementFormControl*>(
        document.GetElementById("file-operation-name"));
    if (operation_input)
      operation_input->SetValue(original_name);
    const bool operation_confirmed = click_id("confirm-file-operation", detail);
    created = operation_opened && operation_confirmed &&
        std::filesystem::is_regular_file(original_path);
    record("UI-026/WS-005-a", "文件页通过 UI 新建文件", created, detail);

    const bool rename_opened = click_id("file-rename", detail) &&
        !hidden("file-operation-overlay");
    operation_input = dynamic_cast<Rml::ElementFormControl*>(
        document.GetElementById("file-operation-name"));
    if (operation_input)
      operation_input->SetValue(renamed_name);
    const bool rename_confirmed = click_id("confirm-file-operation", detail);
    renamed = created && rename_opened && rename_confirmed &&
        !std::filesystem::exists(original_path) &&
        std::filesystem::is_regular_file(renamed_path);
    record("UI-026/WS-005-b", "文件页通过 UI 重命名文件", renamed, detail);

    const bool delete_opened = click_id("file-delete", detail) &&
        !hidden("file-operation-overlay");
    const bool delete_confirmed = click_id("confirm-file-operation", detail);
    removed = renamed && delete_opened && delete_confirmed &&
        !std::filesystem::exists(renamed_path);
    record("UI-026/WS-005-c", "文件页通过确认弹窗删除测试文件",
           removed, detail);
  } else {
    record("UI-026/WS-005-a", "文件页通过 UI 新建文件", false,
           "unique acceptance path unexpectedly exists");
    record("UI-026/WS-005-b", "文件页通过 UI 重命名文件", false,
           "create step did not run");
    record("UI-026/WS-005-c", "文件页通过确认弹窗删除测试文件", false,
           "rename step did not run");
  }
  // Never leave a test artifact behind if a later interaction failed. These
  // are exact paths generated and owned by this acceptance run.
  std::error_code cleanup_error;
  if (created && std::filesystem::exists(original_path))
    std::filesystem::remove(original_path, cleanup_error);
  cleanup_error.clear();
  if ((created || renamed) && std::filesystem::exists(renamed_path))
    std::filesystem::remove(renamed_path, cleanup_error);

  const auto editor_token = "TOKMON_EDITOR_E2E_" + unique;
  const auto editor_name = ".tokmon-desk-editor-e2e-" + unique + ".cpp";
  const auto editor_path = workspace / editor_name;
  {
    std::ofstream editor_fixture(editor_path,
                                 std::ios::binary | std::ios::trunc);
    editor_fixture << "int main() {\n  return 0;\n}\nneedle(value);\n// "
                   << editor_token << "\n";
    for (int line = 0; line < 220; ++line)
      editor_fixture << "int fixture_line_" << line
                     << " = 123456789012345678901234567890;\n";
  }
  const bool editor_fixture_created =
      std::filesystem::is_regular_file(editor_path);
  const bool files_refreshed = click_id("right-tab-close", detail) &&
      click_id("launcher-files", detail);
  settle();
  auto* file_search = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("file-search"));
  if (file_search) {
    file_search->SetValue("stale-generation-query");
    file_search->DispatchEvent("input", {}, false);
    file_search->SetValue(editor_token);
    file_search->DispatchEvent("input", {}, false);
  }
  settle();
  auto* file_tree = dynamic_cast<ElementFileTree*>(
      document.GetElementById("file-tree"));
  const auto final_search_row = file_tree ? file_tree->row_at(2.f) :
      std::optional<WorkspaceEntry>{};
  const bool latest_search_won = final_search_row &&
      final_search_row->relative_path == editor_name;
  if (file_tree) {
    file_tree->Focus();
    context.ProcessKeyDown(Rml::Input::KI_END, 0);
    context.ProcessKeyUp(Rml::Input::KI_END, 0);
    context.ProcessKeyDown(Rml::Input::KI_HOME, 0);
    context.ProcessKeyUp(Rml::Input::KI_HOME, 0);
  }
  const bool tree_keyboard = file_tree && file_tree->selected_row() &&
      file_tree->selected_row()->relative_path == editor_name;
  const bool file_row_clicked = click_point(file_tree, 20.f, 8.f, detail);
  settle();
  auto* editor = dynamic_cast<ElementCodeSurface*>(
      document.GetElementById("file-preview"));
  const bool editor_opened = editor && editor->version() > 0 &&
      editor->text().find(editor_token) != std::string::npos;
  record("UI-025/026/WS-002/003", "文件搜索代次保护、虚拟树键盘导航并打开文件",
         editor_fixture_created && files_refreshed && latest_search_won &&
             tree_keyboard && file_row_clicked && editor_opened,
         detail + "; row=" +
             (final_search_row ? final_search_row->relative_path : "<none>"));

  const bool editor_focused = click_point(editor, 150.f, 42.f, detail);
  const bool ime_preedit = dispatch_text(SDL_EVENT_TEXT_EDITING,
                                         "zhongwen", 8, 0);
  const bool composition_visible = editor &&
      editor->composition_text() == "zhongwen";
  const bool ime_commit = dispatch_text(SDL_EVENT_TEXT_INPUT, "中文");
  const bool composition_cleared = editor &&
      editor->composition_text().empty() &&
      editor->text().find("中文") != std::string::npos;
  const bool undo_clicked = click_id("undo-file", detail);
  const bool undo_removed_ime = editor &&
      editor->text().find("中文") == std::string::npos;
  const bool redo_clicked = click_id("redo-file", detail);
  const bool redo_restored_ime = editor &&
      editor->text().find("中文") != std::string::npos;
  record("UI-027/INPUT-001/EDIT-003", "编辑器真实 SDL IME 预编辑、提交、撤销与重做",
         editor_focused && ime_preedit && composition_visible && ime_commit &&
             composition_cleared && undo_clicked && undo_removed_ime &&
             redo_clicked && redo_restored_ime,
         detail);

  auto* editor_find = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("editor-find"));
  auto* editor_replace = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("editor-replace"));
  auto* editor_line = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("editor-line"));
  if (editor_find) editor_find->SetValue("needle");
  const bool find_clicked = click_id("editor-find-next", detail);
  if (editor_replace) editor_replace->SetValue("marker");
  const bool replace_clicked = click_id("editor-replace-one", detail);
  if (editor_line) editor_line->SetValue("180");
  const bool goto_clicked = click_id("editor-go-line", detail);
  const bool goto_scrolled = editor && editor->first_visible_line() > 100;
  if (editor) {
    editor->set_caret_offset(editor->text().find('{'));
  }
  const bool bracket_clicked = click_id("editor-match-bracket", detail);
  const auto* find_status = document.GetElementById("editor-find-status");
  const bool find_replace_goto = find_clicked && replace_clicked &&
      editor && editor->text().find("marker") != std::string::npos &&
      goto_clicked && goto_scrolled && bracket_clicked && find_status;
  if (editor) {
    editor->scroll_columns(120.f);
    editor->scroll_lines(8);
  }
  const bool two_axis_scroll = editor && editor->horizontal_offset() > 0.f &&
      editor->first_visible_line() > 0;
  record("UI-027/EDIT-002/006", "编辑器查找、替换、跳转、括号匹配与双轴滚动",
         find_replace_goto && two_axis_scroll,
         find_status ? find_status->GetInnerRML() : "missing status");

  const bool save_clicked = click_id("save-file", detail);
  std::ifstream saved_editor(editor_path, std::ios::binary);
  std::stringstream saved_editor_text;
  saved_editor_text << saved_editor.rdbuf();
  saved_editor.close();
  const bool durable_ui_save = save_clicked &&
      saved_editor_text.str().find("marker") != std::string::npos &&
      saved_editor_text.str().find("中文") != std::string::npos;
  if (editor) {
    editor->set_caret_offset(editor->text().size());
    const auto pending_edit = editor->insert_text("local-dirty");
    if (pending_edit) {
      // Route through the same raw text path so DocumentStore owns the edit.
      (void)dispatch_text(SDL_EVENT_TEXT_INPUT, "local-dirty");
    }
  }
  {
    std::ofstream external(editor_path, std::ios::binary | std::ios::trunc);
    external << "external revision\n";
  }
  settle(1800);
  const auto* file_path_label = document.GetElementById("file-path");
  const bool external_conflict_visible = file_path_label &&
      file_path_label->GetInnerRML().find("外部修改冲突") != std::string::npos;
  const bool reload_clicked = click_id("reload-file", detail);
  const bool external_reload = reload_clicked && editor &&
      editor->text() == "external revision\n";
  record("UI-025/027/DOC-002/003", "UI 原子保存并在外部修改时阻止静默覆盖、允许重载",
         durable_ui_save && external_conflict_visible && external_reload,
         file_path_label ? file_path_label->GetInnerRML() : "missing path");
  std::filesystem::remove(editor_path, cleanup_error);
  if (file_search) {
    file_search->SetValue("");
    file_search->DispatchEvent("input", {}, false);
  }

  const bool files_closed_for_terminal = click_id("right-tab-close", detail);
  const bool terminal_menu_open =
      files_closed_for_terminal && click_id("add-tab-button", detail);
  clicked = terminal_menu_open && click_id("terminal-tab", detail);
  (void)controller.update();
  context.Update();
  auto* terminal_surface_element = dynamic_cast<ElementTerminal*>(
      document.GetElementById("terminal-surface"));
  const auto* terminal_status = document.GetElementById("terminal-status");
  const auto terminal_status_text = terminal_status
      ? terminal_status->GetInnerRML() : std::string("<missing>");
  const bool terminal_running = clicked &&
      !hidden("terminal-view") && terminal_status &&
      terminal_status_text.find("正在运行") != std::string::npos;
  record("UI-017/028/TERM-002", "右栏启动跨平台 Terminal profile",
         terminal_running, detail + "; hidden=" +
             (hidden("terminal-view") ? "true" : "false") +
             "; status=" + terminal_status_text);
  const float rendered_terminal_cell_width = terminal_surface_element &&
          terminal_surface_element->grid_columns() > 0
      ? terminal_surface_element->GetClientWidth() /
            terminal_surface_element->grid_columns()
      : 0.f;
  const float rendered_terminal_cell_height = terminal_surface_element &&
          terminal_surface_element->grid_rows() > 0
      ? terminal_surface_element->GetClientHeight() /
            terminal_surface_element->grid_rows()
      : 0.f;
  const float terminal_density = std::max(
      context.GetDensityIndependentPixelRatio(), 0.5f);
  const float logical_terminal_cell_width =
      rendered_terminal_cell_width / terminal_density;
  const float logical_terminal_cell_height =
      rendered_terminal_cell_height / terminal_density;
  const bool responsive_terminal_grid = terminal_surface_element &&
      terminal_surface_element->grid_columns() >= 40 &&
      terminal_surface_element->grid_rows() >= 20 &&
      logical_terminal_cell_width >= 5.f &&
      logical_terminal_cell_width <= 12.f &&
      logical_terminal_cell_height >= 10.f &&
      logical_terminal_cell_height <= 24.f;
  record("UI-028/TERM-003", "Terminal 窄栏与全屏均按固定字格响应式重排",
         responsive_terminal_grid,
         "grid=" + std::to_string(terminal_surface_element
             ? terminal_surface_element->grid_columns() : 0) + "x" +
             std::to_string(terminal_surface_element
             ? terminal_surface_element->grid_rows() : 0) +
             "; cell-px=" + std::to_string(rendered_terminal_cell_width) +
             "x" + std::to_string(rendered_terminal_cell_height) +
             "; cell-dp=" + std::to_string(logical_terminal_cell_width) +
             "x" + std::to_string(logical_terminal_cell_height));
  auto* terminal_tabs = document.GetElementById("terminal-tabs");
  const int terminal_count_before = terminal_tabs
      ? terminal_tabs->GetNumChildren() : 0;
  std::string terminal_new_detail;
  const bool terminal_new = click_id("terminal-new-tab", terminal_new_detail);
  const int terminal_count_after_new = terminal_tabs
      ? terminal_tabs->GetNumChildren() : 0;
  std::string terminal_close_detail;
  const bool terminal_close = click_id("terminal-close-tab", terminal_close_detail);
  const int terminal_count_after_close = terminal_tabs
      ? terminal_tabs->GetNumChildren() : 0;
  record("UI-028/TERM-008", "Terminal 多标签新建与关闭",
         terminal_running && terminal_new && terminal_close &&
             terminal_count_after_new == terminal_count_before + 1 &&
             terminal_count_after_close == terminal_count_before,
         "tab counts " + std::to_string(terminal_count_before) + " -> " +
             std::to_string(terminal_count_after_new) + " -> " +
             std::to_string(terminal_count_after_close) + "; new=" +
             terminal_new_detail + "; close=" + terminal_close_detail);
  auto* terminal_search = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("terminal-search"));
  if (terminal_search)
    terminal_search->SetValue("tokmon-e2e-query");
  const bool terminal_clear = click_id("terminal-clear-search", detail);
  record("UI-028/TERM-012", "Terminal 搜索框清除",
         terminal_clear && terminal_search && terminal_search->GetValue().empty(),
         detail);

  const bool terminal_pointer_click = click_element(
      context, terminal_surface_element, detail);
  const bool terminal_focused = terminal_pointer_click ||
      terminal_status_text.find("已聚焦") != std::string::npos;
  const auto set_clipboard_verified = [&](const std::string_view value) {
    platform.SetClipboardText(Rml::String(value));
    const auto normalized = [](std::string text) {
      std::erase(text, '\r');
      return text;
    };
    const auto expected = normalized(std::string(value));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(750);
    do {
      Rml::String clipboard;
      platform.GetClipboardText(clipboard);
      if (normalized(std::string(clipboard)) == expected)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  };
  const bool unsafe_clipboard_ready =
      set_clipboard_verified("echo one\necho two");
  const bool unsafe_paste_key = dispatch_key(
      SDLK_V, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
  const bool unsafe_prompt_visible = !hidden("terminal-paste-overlay");
  const bool paste_cancelled = click_id("cancel-terminal-paste", detail);
  const bool terminal_refocused = click_element(context, terminal_surface_element,
                                                detail) || terminal_focused;
  const bool safe_clipboard_ready = set_clipboard_verified("tokmon-safe-paste");
  const bool safe_paste_key = dispatch_key(
      SDLK_V, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
  const bool safe_paste_unblocked = hidden("terminal-paste-overlay");
  record("UI-030/TERM-006", "Terminal 安全文本直贴，多行文本必须确认且可取消",
          terminal_focused && unsafe_clipboard_ready && unsafe_paste_key &&
              unsafe_prompt_visible &&
              paste_cancelled && hidden("terminal-paste-overlay") &&
              terminal_refocused && safe_clipboard_ready && safe_paste_key &&
              safe_paste_unblocked,
          "focus/unsafe-clipboard/key/prompt/cancel/refocus/"
          "safe-clipboard/safe-key/hidden=" +
              std::string(terminal_focused ? "1" : "0") + "/" +
              (unsafe_clipboard_ready ? "1" : "0") + "/" +
              (unsafe_paste_key ? "1" : "0") + "/" +
              (unsafe_prompt_visible ? "1" : "0") + "/" +
              (paste_cancelled ? "1" : "0") + "/" +
              (terminal_refocused ? "1" : "0") + "/" +
              (safe_clipboard_ready ? "1" : "0") + "/" +
              (safe_paste_key ? "1" : "0") + "/" +
              (safe_paste_unblocked ? "1" : "0"));
  const bool terminal_escape = dispatch_key(SDLK_ESCAPE);
  const auto* terminal_hint = document.GetElementById("terminal-hint");
  record("UI-029/TERM-012-focus", "Terminal Esc 释放重输入焦点并恢复操作提示",
         terminal_escape && terminal_hint &&
             terminal_hint->GetInnerRML().find("点击终端") !=
                 std::string::npos,
         terminal_hint ? terminal_hint->GetInnerRML() : "missing hint");

  const bool fullscreen_clicked = click_id("right-fullscreen", detail);
  const bool fullscreen_active = shell && shell->IsClassSet("right-fullscreen");
  auto* fullscreen_panel = document.GetElementById("right-panel");
  const float fullscreen_panel_width = fullscreen_panel
      ? fullscreen_panel->GetOffsetWidth() : 0.f;
  const float context_width = static_cast<float>(context.GetDimensions().x);
  const bool fullscreen_geometry =
      fullscreen_panel_width >= context_width * 0.95f &&
      fullscreen_panel && !fullscreen_panel->IsClassSet("narrow");
  const bool fullscreen_restored = click_id("right-fullscreen", detail);
  const float restored_panel_width = fullscreen_panel
      ? fullscreen_panel->GetOffsetWidth() : context_width;
  record("UI-016-fullscreen", "右侧功能页全屏并恢复三栏布局",
         fullscreen_clicked && fullscreen_active && fullscreen_geometry &&
             fullscreen_restored && shell &&
             !shell->IsClassSet("right-fullscreen") &&
             restored_panel_width < context_width * 0.8f,
         "full/restored/context=" + std::to_string(fullscreen_panel_width) +
             "/" + std::to_string(restored_panel_width) + "/" +
             std::to_string(context_width));

  const bool terminal_closed_for_review = click_id("right-tab-close", detail);
  clicked = terminal_closed_for_review && click_id("launcher-review", detail);
  record("UI-017/018", "右栏切回 Review",
         clicked && !hidden("review-view"), detail);

  const auto review_name = ".tokmon-desk-review-e2e-" + unique + ".txt";
  const auto review_path = workspace / review_name;
  {
    std::ofstream review_fixture(review_path,
                                 std::ios::binary | std::ios::trunc);
    review_fixture << "review e2e baseline\n";
  }
  const bool review_refresh = click_id("refresh-review", detail);
  settle();
  Rml::ElementList diff_rows;
  document.QuerySelectorAll(diff_rows, "[diff-path]");
  Rml::Element* review_diff_row = nullptr;
  for (auto* row : diff_rows)
    if (row->GetAttribute<Rml::String>("diff-path", "") == review_name)
      review_diff_row = row;
  const bool diff_opened = click_element(context, review_diff_row, detail);
  settle();
  auto* diff_surface = dynamic_cast<ElementDiffSurface*>(
      document.GetElementById("diff-surface"));
  const bool real_diff_rendered = diff_opened && diff_surface &&
      diff_surface->line_count() > 0 && !hidden("review-diff");
  record("UI-018/020/021/GIT-006", "Review 刷新真实 Git 状态并在 worker 生成可滚动 Diff",
         review_refresh && review_diff_row && real_diff_rendered,
         detail + "; lines=" +
             std::to_string(diff_surface ? diff_surface->line_count() : 0));
  auto* diff_toggle = document.GetElementById("diff-view-toggle");
  const bool diff_before = diff_toggle && diff_toggle->IsClassSet("active");
  const bool diff_click = click_id("diff-view-toggle", detail);
  const bool diff_changed = diff_toggle &&
      diff_toggle->IsClassSet("active") != diff_before;
  const bool diff_restore = click_id("diff-view-toggle", detail);
  record("UI-021/022", "Unified/Split Diff 模式切换并恢复",
         diff_click && diff_changed && diff_restore && diff_toggle &&
             diff_toggle->IsClassSet("active") == diff_before,
         detail);

  const bool diff_closed = click_id("close-diff", detail);
  settle(2500);
  Rml::ElementList stage_actions;
  if (review_diff_row && review_diff_row->GetParentNode())
    review_diff_row->GetParentNode()->QuerySelectorAll(stage_actions,
                                                       "[git-action]");
  Rml::Element* stage_file_action = nullptr;
  std::size_t stage_match_count = 0;
  for (auto* action : stage_actions)
    if (action->GetAttribute<Rml::String>("git-action", "") == "stage-file" &&
        action->GetAttribute<Rml::String>("git-path", "") == review_name) {
      ++stage_match_count;
      if (!stage_file_action && action->GetOffsetWidth() > 0.f &&
          action->GetOffsetHeight() > 0.f)
        stage_file_action = action;
    }
  if (!stage_file_action && review_diff_row &&
      review_diff_row->GetParentNode()) {
    auto* row = review_diff_row->GetParentNode();
    const auto row_offset = row->GetAbsoluteOffset(Rml::BoxArea::Border);
    stage_file_action = scan_hit_box(
        row_offset.x, row_offset.y,
        row_offset.x + row->GetOffsetWidth(),
        row_offset.y + row->GetOffsetHeight(), 2.f,
        [&](Rml::Element& candidate) {
          return candidate.GetAttribute<Rml::String>("git-action", "") ==
                     "stage-file" &&
              candidate.GetAttribute<Rml::String>("git-path", "") ==
                  review_name;
        });
  }
  if (stage_file_action) {
    stage_file_action->ScrollIntoView(false);
    context.Update();
  }
  std::string stage_detail;
  bool stage_clicked = click_element(context, stage_file_action, stage_detail);
  if (!stage_clicked && stage_file_action) {
    stage_clicked = stage_file_action->DispatchEvent("click", {}, true);
    context.Update();
    stage_detail += "; data-bound proxy click";
  }
  settle(3500);
  const auto staged_snapshot = GitService(workspace).status();
  const auto staged_item = std::ranges::find(
      staged_snapshot.files, review_name, &GitFileStatus::path);
  const bool file_staged = staged_item != staged_snapshot.files.end() &&
      staged_item->index_status != ' ' && staged_item->index_status != '?';
  stage_actions.clear();
  diff_rows.clear();
  document.QuerySelectorAll(diff_rows, "[diff-path]");
  Rml::Element* staged_review_row = nullptr;
  for (auto* row : diff_rows)
    if (row->GetAttribute<Rml::String>("diff-path", "") == review_name)
      staged_review_row = row;
  if (staged_review_row && staged_review_row->GetParentNode())
    staged_review_row->GetParentNode()->QuerySelectorAll(stage_actions,
                                                         "[git-action]");
  Rml::Element* unstage_file_action = nullptr;
  for (auto* action : stage_actions)
    if (action->GetAttribute<Rml::String>("git-action", "") ==
            "unstage-file" &&
        action->GetAttribute<Rml::String>("git-path", "") == review_name)
      unstage_file_action = action;
  if (!unstage_file_action && staged_review_row &&
      staged_review_row->GetParentNode()) {
    auto* row = staged_review_row->GetParentNode();
    const auto row_offset = row->GetAbsoluteOffset(Rml::BoxArea::Border);
    unstage_file_action = scan_hit_box(
        row_offset.x, row_offset.y,
        row_offset.x + row->GetOffsetWidth(),
        row_offset.y + row->GetOffsetHeight(), 2.f,
        [&](Rml::Element& candidate) {
          return candidate.GetAttribute<Rml::String>("git-action", "") ==
                     "unstage-file" &&
              candidate.GetAttribute<Rml::String>("git-path", "") ==
                  review_name;
        });
  }
  if (unstage_file_action) {
    unstage_file_action->ScrollIntoView(false);
    context.Update();
  }
  bool unstage_clicked = click_element(context, unstage_file_action, detail);
  if (!unstage_clicked && unstage_file_action) {
    unstage_clicked = unstage_file_action->DispatchEvent("click", {}, true);
    context.Update();
  }
  settle();
  const auto unstaged_snapshot = GitService(workspace).status();
  const auto unstaged_item = std::ranges::find(
      unstaged_snapshot.files, review_name, &GitFileStatus::path);
  const bool file_unstaged = unstaged_item != unstaged_snapshot.files.end() &&
      (unstaged_item->index_status == ' ' ||
       unstaged_item->index_status == '?') &&
      unstaged_item->worktree_status != ' ';
  record("UI-019/020/GIT-001/003", "Review 通过 UI stage/unstage 精确文件",
         diff_closed && stage_clicked && file_staged && unstage_clicked &&
             file_unstaged,
         "close/stage/staged/unstage/unstaged=" +
             std::string(diff_closed ? "1" : "0") + "/" +
             (stage_clicked ? "1" : "0") + "/" +
             (file_staged ? "1" : "0") + "/" +
             (unstage_clicked ? "1" : "0") + "/" +
             (file_unstaged ? "1" : "0") +
             "; stage-matches=" + std::to_string(stage_match_count) +
             "; click=" + stage_detail +
             "; index/worktree=" +
             (staged_item != staged_snapshot.files.end()
                  ? std::string(1, staged_item->index_status) + "/" +
                        std::string(1, staged_item->worktree_status)
                  : "missing"));

  const bool branch_opened = click_id("branch-button", detail);
  settle();
  Rml::ElementList branch_rows;
  if (auto* branch_menu = document.GetElementById("branch-menu"))
    branch_menu->QuerySelectorAll(branch_rows, "[git-branch]");
  const bool branch_menu_populated = branch_opened &&
      !hidden("branch-menu") && !branch_rows.empty();
  const bool branch_closed = dispatch_key(SDLK_ESCAPE);
  record("UI-019/GIT-005", "分支菜单异步加载并可用 Esc 无损关闭",
         branch_menu_populated && branch_closed && hidden("branch-menu"),
         "branches=" + std::to_string(branch_rows.size()));

  stage_actions.clear();
  document.QuerySelectorAll(stage_actions, "[git-action]");
  Rml::Element* discard_file_action = nullptr;
  for (auto* action : stage_actions)
    if (action->GetAttribute<Rml::String>("git-action", "") ==
            "discard-file" &&
        action->GetAttribute<Rml::String>("git-path", "") == review_name)
      discard_file_action = action;
  const bool discard_requested = click_element(context, discard_file_action,
                                                detail);
  settle();
  const bool discard_confirm_visible = !hidden("discard-overlay");
  {
    std::ofstream changed_after_prompt(review_path,
                                       std::ios::binary | std::ios::trunc);
    changed_after_prompt << "external change after hash\n";
  }
  const bool discard_confirmed = click_id("confirm-discard", detail);
  settle();
  std::ifstream guarded_review(review_path, std::ios::binary);
  std::stringstream guarded_review_text;
  guarded_review_text << guarded_review.rdbuf();
  guarded_review.close();
  const bool hash_guard_preserved = std::filesystem::exists(review_path) &&
      guarded_review_text.str() == "external change after hash\n";
  record("UI-023/GIT-004", "放弃确认在提示后文件变化时由 hash guard 阻止",
         discard_requested && discard_confirm_visible && discard_confirmed &&
             hash_guard_preserved,
         detail);

  const bool commit_opened = click_id("commit-button", detail);
  auto* commit_message = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("commit-message"));
  if (commit_message) commit_message->SetValue("");
  const bool empty_commit_clicked = click_id("confirm-commit", detail);
  const bool empty_commit_rejected = commit_opened && empty_commit_clicked &&
      !hidden("commit-overlay") && commit_message &&
      commit_message->IsPseudoClassSet("focus");
  const bool commit_cancelled = click_id("cancel-commit", detail);
  record("UI-024", "提交弹窗拒绝空信息且取消不产生提交",
         empty_commit_rejected && commit_cancelled && hidden("commit-overlay"),
         detail);

  std::string cleanup_git_error;
  (void)GitService(workspace).unstage_file(review_name, cleanup_git_error);
  std::filesystem::remove(review_path, cleanup_error);
  const auto cleanup_snapshot = GitService(workspace).status();
  const bool review_artifact_removed = std::ranges::find(
      cleanup_snapshot.files, review_name, &GitFileStatus::path) ==
      cleanup_snapshot.files.end();
  record("TEST-CLEANUP-GIT", "Git E2E 仅清理本轮精确测试文件与索引项",
         review_artifact_removed, cleanup_git_error);

  clicked = click_id("right-tab-close", detail);
  const bool returned_to_launcher = clicked && !hidden("right-launcher") &&
      hidden("review-view");
  const bool launcher_reopen = click_id("launcher-review", detail);
  record("UI-016-a", "右栏关闭活动页签后返回启动器并可重新打开",
         returned_to_launcher && launcher_reopen && !hidden("review-view"),
         detail);
  clicked = click_id("right-toggle", detail);
  const bool right_collapsed = clicked && hidden("right-panel") && shell &&
      shell->IsClassSet("right-hidden");
  const std::string collapse_detail = detail;
  const bool right_restore = click_id("right-restore", detail);
  detail = "collapse=" + std::string(clicked ? "1" : "0") + "/hidden=" +
      std::string(hidden("right-panel") ? "1" : "0") + "/class=" +
      std::string(shell && shell->IsClassSet("right-hidden") ? "1" : "0") +
      "; restore=" + std::string(right_restore ? "1" : "0") + "/open=" +
      std::string(!hidden("right-panel") ? "1" : "0") + "/class=" +
      std::string(shell && !shell->IsClassSet("right-hidden") ? "1" : "0") +
      " [" + collapse_detail + "] [" + detail + "]";
  record("UI-016-b", "右栏折叠并恢复", right_collapsed && right_restore &&
         !hidden("right-panel") && shell && !shell->IsClassSet("right-hidden"),
         detail);

  const bool passed = std::ranges::all_of(
      checks, [](const InteractionCheck& check) { return check.passed; });
  std::error_code io_error;
  std::filesystem::create_directories(path.parent_path(), io_error);
  if (io_error) {
    error = "could not create interaction report directory: " +
            io_error.message();
    return false;
  }
  std::ofstream report(path, std::ios::binary | std::ios::trunc);
  if (!report) {
    error = "could not open interaction report";
    return false;
  }
  report << "{\n  \"schema\": 1,\n  \"renderer\": \""
         << json_escape(renderer) << "\",\n  \"browser\": \"DEFERRED-BROWSER\",\n"
         << "  \"workspace\": \"" << json_escape(workspace.generic_string())
         << "\",\n  \"dispatch\": \"RmlUi Context mouse move/down/up\",\n"
         << "  \"checks\": [\n";
  for (std::size_t index = 0; index < checks.size(); ++index) {
    const auto& check = checks[index];
    report << "    {\"id\": \"" << json_escape(check.id)
           << "\", \"description\": \"" << json_escape(check.description)
           << "\", \"passed\": " << (check.passed ? "true" : "false")
           << ", \"detail\": \"" << json_escape(check.detail) << "\"}"
           << (index + 1 == checks.size() ? "\n" : ",\n");
  }
  report << "  ],\n  \"passedCount\": "
         << std::ranges::count_if(checks,
              [](const InteractionCheck& check) { return check.passed; })
         << ",\n  \"failedCount\": "
         << std::ranges::count_if(checks,
              [](const InteractionCheck& check) { return !check.passed; })
         << ",\n  \"passed\": " << (passed ? "true" : "false") << "\n}\n";
  if (!report) {
    error = "could not write interaction report";
    return false;
  }
  if (!passed)
    error = "one or more real RmlUi interaction checks failed";
  return passed;
}

bool write_ui_contract_report(Rml::ElementDocument& document,
                              const SdlPlatform& platform,
                              const int logical_width,
                              const int logical_height,
                              const int ui_scale_percent,
                              const int content_scale_percent,
                              const std::string_view renderer,
                              const std::filesystem::path& path,
                              std::string& error) {
  struct Contract {
    const char* id;
    float x;
    float y;
    float width;
    float height;
  };
  constexpr float sidebar_width = 240.5f;
  constexpr float right_panel_width = legacy_launcher_width + 0.5f;
  // The 6.4dp Slint inset lands on the next device-pixel boundary in RmlUi.
  // 6.8dp is the density-independent contract value measured from the frozen
  // 150%/125% capture and remains within one logical pixel at every tested DPI.
  constexpr float frame_inset = 6.8f;
  const float inner_width = static_cast<float>(logical_width) -
                            frame_inset * 2.f;
  const float inner_height = static_cast<float>(logical_height) -
                             frame_inset * 2.f;
  const float workspace_width =
      inner_width - sidebar_width - right_panel_width;
  const float composer_width = std::min(759.f, workspace_width - 48.f);
  const float composer_x = frame_inset + sidebar_width +
      (workspace_width - composer_width) / 2.f;
  const std::vector<Contract> contracts{
      {"sidebar", frame_inset, frame_inset, sidebar_width, inner_height},
      {"workspace", frame_inset + sidebar_width, frame_inset,
       workspace_width, inner_height},
      {"right-panel", static_cast<float>(logical_width) - frame_inset -
                          right_panel_width,
       frame_inset, right_panel_width, inner_height},
      {"brand-row", frame_inset, frame_inset, sidebar_width - 1.f, 46.f},
      {"workspace-titlebar", frame_inset + sidebar_width, frame_inset,
       workspace_width, 45.5f},
      {"right-titlebar", static_cast<float>(logical_width) - frame_inset -
                             right_panel_width + 1.f,
       frame_inset, right_panel_width - 1.f, 45.5f},
      {"right-launcher", static_cast<float>(logical_width) - frame_inset -
                            right_panel_width + 1.f,
       frame_inset + 46.f, right_panel_width - 1.f, inner_height - 46.f},
      {"composer-wrap", composer_x, frame_inset + inner_height - 164.f,
       composer_width, 164.f},
      {"composer-card", composer_x, frame_inset + inner_height - 115.f,
       composer_width, 99.f}};

  std::error_code io_error;
  std::filesystem::create_directories(path.parent_path(), io_error);
  if (io_error) {
    error = "could not create UI contract report directory: " +
            io_error.message();
    return false;
  }
  std::ofstream report(path, std::ios::binary | std::ios::trunc);
  if (!report) {
    error = "could not open UI contract report";
    return false;
  }
  bool passed = true;
  const float density = document.GetContext()
      ? std::max(document.GetContext()->GetDensityIndependentPixelRatio(),
                 0.01f)
      : 1.f;
  report << "{\n  \"schema\": 1,\n"
         << "  \"renderer\": \"" << renderer << "\",\n"
         << "  \"logicalViewport\": {\"width\": " << logical_width
         << ", \"height\": " << logical_height << "},\n"
         << "  \"pixelViewport\": {\"width\": " << platform.pixel_width()
         << ", \"height\": " << platform.pixel_height() << "},\n"
         << "  \"uiScalePercent\": " << ui_scale_percent << ",\n"
         << "  \"contentScalePercent\": " << content_scale_percent << ",\n"
         << "  \"toleranceLogicalPixels\": 1,\n"
         << "  \"elements\": [\n";
  for (std::size_t index = 0; index < contracts.size(); ++index) {
    const auto& contract = contracts[index];
    auto* element = document.GetElementById(contract.id);
    const auto physical_offset = element
        ? element->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f{};
    const Rml::Vector2f offset{physical_offset.x / density,
                               physical_offset.y / density};
    const float width = element ? element->GetOffsetWidth() / density : -1.f;
    const float height = element ? element->GetOffsetHeight() / density : -1.f;
    const bool item_passed = element &&
        std::abs(offset.x - contract.x) <= 1.f &&
        std::abs(offset.y - contract.y) <= 1.f &&
        std::abs(width - contract.width) <= 1.f &&
        std::abs(height - contract.height) <= 1.f;
    passed = passed && item_passed;
    report << "    {\"id\": \"" << contract.id << "\", \"expected\": {"
           << "\"x\": " << contract.x << ", \"y\": " << contract.y
           << ", \"width\": " << contract.width << ", \"height\": "
           << contract.height << "}, \"actual\": {\"x\": " << offset.x
           << ", \"y\": " << offset.y << ", \"width\": " << width
           << ", \"height\": " << height << "}, \"passed\": "
           << (item_passed ? "true" : "false") << "}"
           << (index + 1 == contracts.size() ? "\n" : ",\n");
  }
  report << "  ],\n  \"passed\": " << (passed ? "true" : "false")
         << "\n}\n";
  if (!report) {
    error = "could not write UI contract report";
    return false;
  }
  if (!passed)
    error = "UI geometry differs from the frozen legacy contract";
  return passed;
}

SDL_HitTestResult hit_test(SDL_Window* window, const SDL_Point* point,
                           void* user_data) {
  const auto* platform = static_cast<const SdlPlatform*>(user_data);
  const float scale = platform
      ? std::max(platform->ui_scale(), 0.5f)
      : std::max(SDL_GetWindowDisplayScale(window), 0.5f);
  const float x = static_cast<float>(point->x) / scale;
  const float y = static_cast<float>(point->y) / scale;
  // Keep the dedicated brand strip draggable, but never place a native drag
  // hit-test region over RmlUi title-bar controls. SDL consumes those pointer
  // events before RmlUi can dispatch them.
  if (y >= legacy_frame_inset && y < legacy_frame_inset + 42.f &&
      x >= legacy_frame_inset + 30.f &&
      x < legacy_frame_inset + 210.f)
    return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}

} // namespace

int run_desk(int argc, char** argv) {
  try {
    auto arguments = parse_arguments(argc, argv);
    const auto paths = DeskAppPaths::resolve(arguments.app_data_root);
    std::string error;
    if (!paths.ensure(error)) {
      std::cerr << "tokmon-desk: " << error << '\n';
      return 2;
    }
    DeskRetentionReport retention;
    if (!paths.enforce_retention({}, retention, error)) {
      std::cerr << "tokmon-desk: state retention warning: " << error << '\n';
      error.clear();
    }
    if (arguments.workspace.empty())
      arguments.workspace = restored_workspace(paths);
    if (!paths.isolated_from(arguments.workspace)) {
      std::cerr << "tokmon-desk: application data paths overlap the workspace/.tokmon\n";
      return 2;
    }
    DeskInstanceLock instance_lock(paths.runtime / "tokmon-desk.lock");
    if (!instance_lock.acquired()) {
      std::cerr << "tokmon-desk: " << instance_lock.error() << '\n';
      return 12;
    }

    SdlPlatform platform;
    if (!platform.initialize("tokmon-desk — Tokmon", arguments.logical_width,
                             arguments.logical_height, error)) {
      std::cerr << "tokmon-desk: SDL initialization failed: " << error << '\n';
      return 3;
    }
    SDL_SetWindowHitTest(platform.window(), hit_test, &platform);

    const int ui_scale_percent = arguments.ui_scale_percent > 0
                                     ? arguments.ui_scale_percent
                                     : platform.default_ui_scale_percent();
    // Display scaling and the legacy application zoom are independent. An
    // explicit display scale must not silently reset the saved 125% UI zoom.
    const int content_scale_percent = arguments.content_scale_percent > 0
        ? arguments.content_scale_percent
        : stored_content_scale_percent(
              paths, platform.default_content_scale_percent());
    const float window_scale = static_cast<float>(ui_scale_percent) / 100.f;
    const float content_scale =
        static_cast<float>(content_scale_percent) / 100.f;
    const float ui_scale = window_scale * content_scale;
    platform.set_ui_scale(ui_scale);
    platform.size_window_for_ui_scale(arguments.logical_width,
                                      arguments.logical_height, window_scale);
    auto device = SkiaDevice::create(platform.window(), platform.pixel_width(),
                                     platform.pixel_height(),
                                     arguments.software_renderer, error);
    if (!device) {
      std::cerr << "tokmon-desk: Skia initialization failed: " << error << '\n';
      return 4;
    }
    // Render at native framebuffer resolution. The effective display/content
    // scale is carried by RmlUi's dp unit instead of scaling the completed
    // low-resolution canvas.
    device->set_ui_scale(1.f);
    device->set_frame_density(ui_scale);
    std::cerr << "tokmon-desk: renderer=" << device->backend_name()
              << " display-scale=" << ui_scale_percent
              << "% content-scale=" << content_scale_percent
              << "% effective-scale="
              << static_cast<int>(std::lround(ui_scale * 100.f)) << "%\n";
    RmlRenderInterfaceSkia renderer(*device);
    Rml::SetSystemInterface(&platform);
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise()) {
      std::cerr << "tokmon-desk: RmlUi initialization failed\n";
      return 5;
    }
    register_terminal_element();
    register_code_surface_element();
    register_diff_surface_element();
    register_file_tree_element();

    const char* base_path = SDL_GetBasePath();
    const auto executable_directory = base_path
        ? std::filesystem::path(base_path) : std::filesystem::path{};
    const auto resources = DeskResourcePaths::resolve(executable_directory);
    const auto& font = resources.ui_font;
    FontManager font_manager;
    if (!font_manager.load_ui_font(font, error) ||
        font_manager.shape_utf8("Tokmon 中文 Aa 123", 13.f).empty()) {
      std::cerr << "tokmon-desk: HarfBuzz/FreeType font validation failed: "
                << error << '\n';
      Rml::Shutdown();
      return 6;
    }
    if (!Rml::LoadFontFace(font.generic_string(), true)) {
      std::cerr << "tokmon-desk: required MiSans font not found: " << font << '\n';
      Rml::Shutdown();
      return 6;
    }
    const auto platform_fonts = DeskResourcePaths::platform_font_candidates();
    for (std::size_t index = 0; index < platform_fonts.size(); ++index)
      if (std::filesystem::exists(platform_fonts[index]))
        (void)Rml::LoadFontFace(platform_fonts[index].generic_string(), index != 0);
    auto* context = Rml::CreateContext("tokmon-desk",
        {device->physical_width(), device->physical_height()}, nullptr);
    if (!context) {
      std::cerr << "tokmon-desk: could not create RmlUi context\n";
      Rml::Shutdown();
      return 7;
    }
    context->SetDensityIndependentPixelRatio(ui_scale);
    DeskViewModel view_model;
    if (!view_model.bind(*context, resources.assets.generic_string())) {
      std::cerr << "tokmon-desk: could not bind the desk view model\n";
      Rml::Shutdown();
      return 8;
    }
    auto* document = context->LoadDocument(resources.main_document.generic_string());
    if (!document) {
      std::cerr << "tokmon-desk: could not load main.rml\n";
      Rml::Shutdown();
      return 8;
    }
    document->Show();
    std::filesystem::path daemon_endpoint;
    if (auto resolved = tokmon::resolve_paths(arguments.workspace); resolved)
      daemon_endpoint = tokmon::workspace_snow_endpoint(
          resolved->run, resolved->project.parent_path());
    DeskController controller(*document, platform, view_model,
                              arguments.workspace, paths,
                              daemon_endpoint);
    controller.bind(!arguments.smoke_test &&
                    arguments.ui_contract_report.empty());

    if (!arguments.visual_state.empty() &&
        !prepare_visual_state(*context, *document, view_model, controller,
                              arguments.visual_state,
                              error)) {
      std::cerr << "tokmon-desk: " << error << '\n';
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return 15;
    }

    if (!arguments.ui_contract_report.empty()) {
      controller.prepare_legacy_three_pane_contract();
      context->Update();
      if (!write_ui_contract_report(*document, platform,
                                    static_cast<int>(std::lround(
                                        static_cast<float>(device->physical_width()) /
                                            ui_scale)),
                                    static_cast<int>(std::lround(
                                        static_cast<float>(device->physical_height()) /
                                            ui_scale)),
                                    ui_scale_percent,
                                    content_scale_percent,
                                    device->backend_name(),
                                    arguments.ui_contract_report, error)) {
        std::cerr << "tokmon-desk: " << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 13;
      }
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return 0;
    }

    if (!arguments.interaction_report.empty()) {
      context->Update();
      if (!write_interaction_report(platform, *context, *document, controller,
                                    arguments.workspace, paths,
                                    arguments.interaction_report,
                                    device->backend_name(), error)) {
        std::cerr << "tokmon-desk: " << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 14;
      }
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return 0;
    }

    if (!arguments.device_recovery_report.empty()) {
      const auto before_path = arguments.device_recovery_report.parent_path() /
          "device-recovery-before.png";
      const auto after_path = arguments.device_recovery_report.parent_path() /
          "device-recovery-after.png";
      std::error_code report_error;
      std::filesystem::create_directories(
          arguments.device_recovery_report.parent_path(), report_error);
      context->Update();
      const bool first_frame = device->begin_frame() != nullptr;
      if (first_frame)
        context->Render();
      const bool first_present = first_frame && device->end_frame(error);
      const bool before_saved = first_present && device->save_png(before_path, error);
      const std::string backend_before = device->backend_name();
      const int width_before = device->physical_width();
      const int height_before = device->physical_height();
      std::string injected_error;
      const bool fault_injected =
          device->force_device_loss_for_test(injected_error);
      const bool recovered = device->recover(error);
      if (recovered) {
        renderer.reset_after_device_recovery();
        context->SetDimensions({device->physical_width(),
                                device->physical_height()});
        context->Update();
      }
      const bool second_frame = recovered && device->begin_frame() != nullptr;
      if (second_frame)
        context->Render();
      const bool second_present = second_frame && device->end_frame(error);
      const bool after_saved = second_present && device->save_png(after_path, error);
      const bool passed = !report_error && first_present && before_saved &&
          fault_injected &&
          recovered && second_present && after_saved &&
          width_before == device->physical_width() &&
          height_before == device->physical_height() &&
          backend_before == device->backend_name();
      std::ofstream report(arguments.device_recovery_report,
                           std::ios::binary | std::ios::trunc);
      report << "{\n  \"schema\": 1,\n"
             << "  \"fault\": \"intentional D3D12 device removal followed by full GPU context and swapchain recreation\",\n"
             << "  \"faultInjected\": "
             << (fault_injected ? "true" : "false")
             << ",\n  \"faultDetail\": \""
             << json_escape(injected_error) << "\",\n"
             << "  \"backendBefore\": \"" << json_escape(backend_before)
             << "\",\n  \"backendAfter\": \""
             << json_escape(device->backend_name()) << "\",\n"
             << "  \"dimensionsBefore\": [" << width_before << ", "
             << height_before << "],\n  \"dimensionsAfter\": ["
             << device->physical_width() << ", " << device->physical_height()
             << "],\n  \"firstPresent\": "
             << (first_present ? "true" : "false")
             << ",\n  \"recovered\": " << (recovered ? "true" : "false")
             << ",\n  \"secondPresent\": "
             << (second_present ? "true" : "false")
             << ",\n  \"beforeScreenshot\": \""
             << json_escape(before_path.generic_string())
             << "\",\n  \"afterScreenshot\": \""
             << json_escape(after_path.generic_string())
             << "\",\n  \"passed\": " << (passed ? "true" : "false")
             << "\n}\n";
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return report && passed ? 0 : 16;
    }

    if (!arguments.acceptance_report.empty()) {
      auto* tree = dynamic_cast<ElementFileTree*>(
          document->GetElementById("file-tree"));
      auto* editor = dynamic_cast<ElementCodeSurface*>(
          document->GetElementById("file-preview"));
      auto* files_view = document->GetElementById("files-view");
      auto* review_view = document->GetElementById("review-view");
      auto* review_diff = document->GetElementById("review-diff");
      auto* conversation = document->GetElementById("conversation");
      auto* trajectory = document->GetElementById("trajectory");
      auto* terminal_view = document->GetElementById("terminal-view");
      auto* terminal_surface = dynamic_cast<ElementTerminal*>(
          document->GetElementById("terminal-surface"));
      if (!tree || !editor || !files_view || !review_view || !review_diff ||
          !conversation || !trajectory || !terminal_view || !terminal_surface) {
        std::cerr << "tokmon-desk: acceptance elements are missing\n";
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      controller.prepare_legacy_three_pane_contract(true);
      controller.seed_acceptance_conversation(10000);
      context->Update();
      const auto conversation_turns = controller.conversation_turn_count();
      const auto conversation_dom_nodes = descendant_count(*conversation);
      const auto trajectory_dom_nodes = descendant_count(*trajectory);
      document->GetElementById("app-shell")->SetClass("right-fullscreen", true);
      files_view->SetClass("hidden", false);
      review_view->SetClass("hidden", true);
      constexpr std::size_t file_tree_stress_rows = 100000;
      std::vector<WorkspaceEntry> rows;
      rows.reserve(file_tree_stress_rows);
      for (std::size_t index = 0; index < file_tree_stress_rows; ++index)
        rows.push_back({arguments.workspace / "large-tree" /
                            ("file-" + std::to_string(index) + ".txt"),
                        "large-tree/file-" + std::to_string(index) + ".txt",
                        "file-" + std::to_string(index) + ".txt",
                        index % 4, false, false});
      tree->set_rows(std::move(rows));
      std::string large_document;
      large_document.reserve(2u * 1024u * 1024u);
      for (std::size_t index = 0; index < 100000; ++index)
        large_document += "virtual editor line " + std::to_string(index) + "\n";
      editor->set_document(std::move(large_document), {}, 1);
      view_model.state().file_open = true;
      view_model.dirty();
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance file-tree present failed: "
                  << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto tree_rendered = tree->visible_geometry_rows();
      const auto tree_dom_children = tree->GetNumChildren();
      const auto editor_lines = editor->line_count();
      const auto editor_rendered = editor->rendered_line_count();
      const auto editor_dom_children = editor->GetNumChildren();

      auto* diff = dynamic_cast<ElementDiffSurface*>(
          document->GetElementById("diff-surface"));
      if (!diff) {
        std::cerr << "tokmon-desk: acceptance diff element is missing\n";
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      GitFileDiff model;
      model.path = "large-diff.txt";
      GitDiffHunk hunk;
      hunk.header = "@@ -1,4100 +1,4100 @@";
      hunk.lines.reserve(4100);
      for (int line = 1; line <= 4100; ++line)
        hunk.lines.push_back({line % 3 == 0 ? '+' : ' ', line, line,
                              "virtual diff line " + std::to_string(line)});
      model.hunks.push_back(std::move(hunk));
      diff->set_diff(std::move(model));
      files_view->SetClass("hidden", true);
      review_view->SetClass("hidden", false);
      review_diff->SetClass("hidden", false);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance diff present failed: " << error
                  << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto diff_lines = diff->line_count();
      const auto diff_rendered = diff->rendered_line_count();
      const auto diff_dom_children = diff->GetNumChildren();
      diff->set_split_view(true);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance split diff present failed: "
                  << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto split_diff_rendered = diff->rendered_line_count();

      review_view->SetClass("hidden", true);
      terminal_view->SetClass("hidden", false);
      GhosttyVt terminal_stress(120, 36, 2000);
      std::string terminal_chunk;
      terminal_chunk.reserve(64u * 1024u);
      constexpr std::string_view stress_line =
          "tokmon terminal stress 0123456789abcdef\r\n";
      while (terminal_chunk.size() + stress_line.size() <= 64u * 1024u)
        terminal_chunk += stress_line;
      terminal_chunk.resize(64u * 1024u, 'x');
      constexpr std::size_t terminal_stress_bytes = 100u * 1024u * 1024u;
      std::size_t terminal_processed_bytes = 0;
      double terminal_max_chunk_ms = 0.0;
      const auto terminal_started_at = std::chrono::steady_clock::now();
      while (terminal_processed_bytes < terminal_stress_bytes) {
        const auto count = std::min(terminal_chunk.size(),
                                    terminal_stress_bytes -
                                        terminal_processed_bytes);
        const auto chunk_started_at = std::chrono::steady_clock::now();
        terminal_stress.append(
            std::string_view(terminal_chunk).substr(0, count));
        terminal_max_chunk_ms = std::max(
            terminal_max_chunk_ms,
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - chunk_started_at).count());
        terminal_processed_bytes += count;
      }
      const auto terminal_elapsed_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - terminal_started_at).count();
      const auto terminal_snapshot = terminal_stress.render_snapshot();
      const auto terminal_snapshot_cells = terminal_snapshot.cells.size();
      terminal_surface->set_snapshot(terminal_snapshot);
      context->Update();
      device->begin_frame();
      context->Render();
      if (!device->end_frame(error)) {
        std::cerr << "tokmon-desk: acceptance terminal present failed: "
                  << error << '\n';
        Rml::RemoveContext("tokmon-desk");
        Rml::Shutdown();
        return 11;
      }
      const auto terminal_dom_children = terminal_surface->GetNumChildren();

      // Measure the complete in-process input path used by the application:
      // SDL event queue -> SdlPlatform translation -> RmlUi update -> Skia
      // render/present. Drain unrelated window events before every sample so
      // the percentile is reproducible and cannot accidentally time a stale
      // resize or focus notification instead of the injected pointer input.
      std::vector<double> input_to_frame_samples_ms;
      input_to_frame_samples_ms.reserve(64);
      bool latency_quit = false;
      bool latency_resized = false;
      for (int sample = 0; sample < 64; ++sample) {
        while (platform.pump_event(*context, latency_quit, latency_resized)) {
        }
        latency_quit = false;
        latency_resized = false;
        SDL_Event input_event{};
        input_event.type = SDL_EVENT_MOUSE_MOTION;
        input_event.motion.windowID = SDL_GetWindowID(platform.window());
        input_event.motion.x = static_cast<float>(800 + sample % 2);
        input_event.motion.y = 500.f;
        const auto input_started_at = std::chrono::steady_clock::now();
        if (!SDL_PushEvent(&input_event) ||
            !platform.pump_event(*context, latency_quit, latency_resized)) {
          std::cerr << "tokmon-desk: acceptance input event injection failed\n";
          Rml::RemoveContext("tokmon-desk");
          Rml::Shutdown();
          return 11;
        }
        context->Update();
        device->begin_frame();
        context->Render();
        if (!device->end_frame(error)) {
          std::cerr << "tokmon-desk: acceptance input present failed: "
                    << error << '\n';
          Rml::RemoveContext("tokmon-desk");
          Rml::Shutdown();
          return 11;
        }
        input_to_frame_samples_ms.push_back(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - input_started_at).count());
      }
      std::ranges::sort(input_to_frame_samples_ms);
      const auto input_to_frame_p95_ms = input_to_frame_samples_ms[
          static_cast<std::size_t>(std::ceil(
              static_cast<double>(input_to_frame_samples_ms.size()) * 0.95)) - 1];
      const auto input_to_frame_max_ms = input_to_frame_samples_ms.back();
      int window_width = 0;
      int window_height = 0;
      SDL_GetWindowSize(platform.window(), &window_width, &window_height);
      const bool metrics_passed = conversation_turns == 10000 &&
          conversation_dom_nodes > 0 && conversation_dom_nodes < 2000 &&
          trajectory_dom_nodes > 0 && trajectory_dom_nodes < 2500 &&
          file_tree_stress_rows == 100000 && tree_rendered > 0 &&
          tree_rendered < 200 && tree_dom_children == 0 &&
          editor_lines >= 100000 && editor_rendered > 0 &&
          editor_rendered < 200 && editor_dom_children == 0 &&
          diff_lines >= 4000 && diff_rendered > 0 && diff_rendered < 200 &&
          diff_dom_children == 0 && split_diff_rendered > 0 &&
          split_diff_rendered < 200 &&
          terminal_processed_bytes == terminal_stress_bytes &&
          terminal_elapsed_ms < 5000.0 && terminal_max_chunk_ms < 50.0 &&
          terminal_snapshot_cells > 0 && terminal_dom_children == 0 &&
          input_to_frame_samples_ms.size() == 64 &&
          input_to_frame_p95_ms <= 50.0;
      std::error_code report_error;
      std::filesystem::create_directories(
          arguments.acceptance_report.parent_path(), report_error);
      std::ofstream report(arguments.acceptance_report,
                           std::ios::binary | std::ios::trunc);
      report << "{\n"
             << "  \"schema\": 1,\n"
             << "  \"browser\": \"DEFERRED-BROWSER\",\n"
             << "  \"renderer\": \"" << device->backend_name() << "\",\n"
             << "  \"windowWidth\": " << window_width << ",\n"
             << "  \"windowHeight\": " << window_height << ",\n"
             << "  \"pixelWidth\": " << platform.pixel_width() << ",\n"
             << "  \"pixelHeight\": " << platform.pixel_height() << ",\n"
             << "  \"pixelDensity\": "
             << SDL_GetWindowPixelDensity(platform.window()) << ",\n"
             << "  \"displayScale\": " << platform.display_scale() << ",\n"
             << "  \"uiScalePercent\": " << ui_scale_percent << ",\n"
             << "  \"contentScalePercent\": " << content_scale_percent
             << ",\n"
             << "  \"effectiveScalePercent\": "
             << static_cast<int>(std::lround(ui_scale * 100.f)) << ",\n"
             << "  \"rmlWidth\": "
             << static_cast<int>(std::lround(
                    static_cast<float>(device->physical_width()) /
                        std::max(ui_scale, 0.01f)))
             << ",\n  \"rmlHeight\": "
             << static_cast<int>(std::lround(
                    static_cast<float>(device->physical_height()) /
                        std::max(ui_scale, 0.01f)))
             << ",\n  \"rmlPixelWidth\": " << device->physical_width()
             << ",\n  \"rmlPixelHeight\": " << device->physical_height() << ",\n"
             << "  \"conversationModelTurns\": " << conversation_turns << ",\n"
             << "  \"conversationDomNodes\": " << conversation_dom_nodes << ",\n"
             << "  \"trajectoryDomNodes\": " << trajectory_dom_nodes << ",\n"
             << "  \"fileTreeModelRows\": " << file_tree_stress_rows << ",\n"
             << "  \"fileTreeRenderedRows\": " << tree_rendered << ",\n"
             << "  \"fileTreeDomChildren\": " << tree_dom_children << ",\n"
             << "  \"editorModelLines\": " << editor_lines << ",\n"
             << "  \"editorRenderedLines\": " << editor_rendered << ",\n"
             << "  \"editorClientWidth\": " << editor->GetClientWidth() << ",\n"
             << "  \"editorClientHeight\": " << editor->GetClientHeight() << ",\n"
             << "  \"editorDomChildren\": " << editor_dom_children << ",\n"
             << "  \"diffModelLines\": " << diff_lines << ",\n"
             << "  \"diffRenderedLines\": " << diff_rendered << ",\n"
             << "  \"diffClientWidth\": " << diff->GetClientWidth() << ",\n"
             << "  \"diffClientHeight\": " << diff->GetClientHeight() << ",\n"
             << "  \"diffDomChildren\": " << diff_dom_children << ",\n"
             << "  \"splitDiffRenderedLines\": " << split_diff_rendered << ",\n"
             << "  \"terminalStressBytes\": " << terminal_processed_bytes << ",\n"
             << "  \"terminalStressElapsedMs\": " << terminal_elapsed_ms << ",\n"
             << "  \"terminalStressMaxChunkMs\": " << terminal_max_chunk_ms << ",\n"
             << "  \"terminalSnapshotCells\": " << terminal_snapshot_cells << ",\n"
             << "  \"terminalDomChildren\": " << terminal_dom_children << ",\n"
             << "  \"inputToFramePath\": \"SDL queue -> RmlUi -> Skia present\",\n"
             << "  \"inputToFrameSamples\": "
             << input_to_frame_samples_ms.size() << ",\n"
             << "  \"inputToFrameP95Ms\": " << input_to_frame_p95_ms << ",\n"
             << "  \"inputToFrameMaxMs\": " << input_to_frame_max_ms
             << ",\n"
             << "  \"thresholds\": {\"conversationDomNodesMax\": 1999, "
                "\"trajectoryDomNodesMax\": 2499, "
                "\"renderedRowsMax\": 199, "
                "\"terminalElapsedMsMax\": 5000, "
                "\"terminalChunkMsMax\": 50, "
                "\"inputToFrameP95MsMax\": 50},\n"
             << "  \"passed\": " << (metrics_passed ? "true" : "false")
             << "\n"
             << "}\n";
      const bool accepted = report && metrics_passed;
      Rml::RemoveContext("tokmon-desk");
      Rml::Shutdown();
      return accepted ? 0 : 11;
    }

    std::future<BackendConnection> daemon_probe;
    std::optional<tokmon::DaemonClientLease> daemon_lease;
    if (!daemon_endpoint.empty() && !arguments.smoke_test &&
        arguments.idle_test_ms == 0) {
      const auto daemon_executable =
#if defined(_WIN32)
          executable_directory / "tokmon.exe";
#else
          executable_directory / "tokmon";
#endif
      daemon_probe = std::async(std::launch::async,
          [daemon_endpoint, daemon_executable,
           workspace = arguments.workspace] {
        BackendConnection backend;
        auto connection = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
            .endpoint = daemon_endpoint,
            .workspace = workspace,
            .executable = daemon_executable});
        if (!connection) {
          backend.error = connection.error().describe();
          return backend;
        }
        backend.connection = std::move(*connection);
        auto lease = tokmon::DaemonClientLease::attach(
            tokmon::DaemonClientOptions{
                .endpoint = daemon_endpoint,
                .client_id = tokmon::make_id("tokmon-desk-client"),
                .client_kind = "desktop",
                .shutdown_when_idle = true,
                .idle_timeout = std::chrono::milliseconds(250),
                .lease_ttl = std::chrono::seconds(6)});
        if (!lease) {
          backend.error = lease.error().describe();
          return backend;
        }
        backend.lease = std::move(*lease);
        return backend;
      });
    } else if (arguments.smoke_test || arguments.idle_test_ms > 0) {
      set_text(*document, "daemon-status", "UI smoke test · 后台连接已跳过");
    } else {
      set_text(*document, "daemon-status", "当前工作区未配置 Tokmon");
    }

    bool quit = false;
    bool screenshot_written = false;
    int frames = 0;
    bool redraw = true;
    const auto loop_started_at = std::chrono::steady_clock::now();
    while (!quit && !controller.quit_requested()) {
      const bool warmup = frames < 3;
      if (!redraw && !warmup && !arguments.smoke_test)
        (void)platform.wait_for_event(controller.update_poll_interval_ms());
      bool resized = false;
      bool event_received = false;
      while (platform.pump_event(*context, quit, resized))
        event_received = true;
      if (arguments.smoke_test && arguments.visual_state != "hover" &&
          arguments.visual_state != "pressed")
        context->ProcessMouseLeave();
      if (resized && !device->resize(platform.pixel_width(), platform.pixel_height(), error)) {
        const std::string resize_error = error;
        if (!device->recover(error)) {
          std::cerr << "tokmon-desk: resize failed: " << resize_error
                    << "; recovery failed: " << error << '\n';
          break;
        }
        renderer.reset_after_device_recovery();
        std::cerr << "tokmon-desk: renderer recovered after resize failure: "
                  << resize_error << '\n';
      }
      if (resized)
        context->SetDimensions({device->physical_width(),
                                device->physical_height()});
      redraw = redraw || event_received || resized || controller.update();
      if (daemon_probe.valid() &&
          daemon_probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        redraw = true;
        auto backend = daemon_probe.get();
        if (!backend.error.empty()) {
          set_text(*document, "daemon-status", "后台服务连接失败");
          std::cerr << "tokmon-desk: daemon connection failed: "
                    << backend.error << '\n';
        } else {
          const bool started = backend.connection && backend.connection->started;
          daemon_lease = std::move(backend.lease);
          set_text(*document, "daemon-status",
                   started ? "后台服务已启动；正在检查配置"
                           : "后台服务已连接；正在检查配置");
          controller.backend_connected();
        }
      }
      if (redraw || warmup || arguments.smoke_test) {
        context->Update();
        if (!device->begin_frame()) {
          const bool recovered = device->recover(error);
          if (recovered) {
            renderer.reset_after_device_recovery();
            context->SetDimensions({device->physical_width(),
                                    device->physical_height()});
            redraw = true;
            continue;
          }
          std::cerr << "tokmon-desk: begin frame and recovery failed: "
                    << error << '\n';
          break;
        }
        context->Render();
        if (!device->end_frame(error)) {
          const std::string present_error = error;
          if (!device->recover(error)) {
            std::cerr << "tokmon-desk: present failed: " << present_error
                      << "; recovery failed: " << error << '\n';
            break;
          }
          renderer.reset_after_device_recovery();
          context->SetDimensions({device->physical_width(),
                                  device->physical_height()});
          std::cerr << "tokmon-desk: renderer recovered after present failure: "
                    << present_error << '\n';
          redraw = true;
          continue;
        }
        ++frames;
        redraw = false;
      }
      // A cold first run can need one extra data-binding/layout cycle after
      // the glyph atlas and custom scrollbars become measurable. Capturing
      // frame three made the golden nondeterministic even though frame six
      // and every interactive frame were stable.
      const int screenshot_frame = arguments.smoke_test ? 6 : 3;
      if (!arguments.screenshot.empty() && !screenshot_written &&
          frames >= screenshot_frame) {
        screenshot_written = device->save_png(arguments.screenshot, error);
        if (!screenshot_written)
          std::cerr << "tokmon-desk: " << error << '\n';
      }
      if (arguments.smoke_test && frames >= 8)
        quit = true;
      if (arguments.idle_test_ms > 0 &&
          std::chrono::steady_clock::now() - loop_started_at >=
              std::chrono::milliseconds(arguments.idle_test_ms))
        quit = true;
    }

    Rml::RemoveContext("tokmon-desk");
    Rml::Shutdown();
    return arguments.smoke_test && !arguments.screenshot.empty() && !screenshot_written ? 9 : 0;
  } catch (const std::exception& exception) {
    std::cerr << "tokmon-desk: fatal: " << exception.what() << '\n';
    return 10;
  }
}

} // namespace tokmon::desk
