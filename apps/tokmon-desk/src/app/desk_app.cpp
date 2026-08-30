#include "app/desk_app.hpp"

#include "integration/daemon_client.hpp"
#include "fonts/font_manager.hpp"
#include "platform/desk_app_paths.hpp"
#include "platform/sdl_platform.hpp"
#include "render/rml_render_interface_skia.hpp"
#include "render/skia_device.hpp"
#include "state/desk_state_store.hpp"
#include "ui/desk_controller.hpp"
#include "ui/elements/element_code_surface.hpp"
#include "ui/elements/element_diff_surface.hpp"
#include "ui/elements/element_file_tree.hpp"
#include "ui/elements/element_terminal.hpp"

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
  std::filesystem::path workspace = std::filesystem::current_path();
  bool smoke_test{false};
  bool software_renderer{false};
  int idle_test_ms{0};
  int ui_scale_percent{0};
  int content_scale_percent{0};
  int logical_width{1440};
  int logical_height{900};
  std::filesystem::path screenshot;
  std::filesystem::path acceptance_report;
  std::filesystem::path interaction_report;
  std::filesystem::path ui_contract_report;
  std::string visual_state;
};

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
    else if (argument == "--visual-state" && index + 1 < argc)
      result.visual_state = argv[++index];
  }
  return result;
}

std::filesystem::path resource_root() {
  if (const char* base = SDL_GetBasePath()) {
    const std::filesystem::path candidate(base);
    if (std::filesystem::exists(candidate / "rml" / "documents" / "main.rml"))
      return candidate;
  }
#ifdef TOKMON_DESK_SOURCE_DIR
  const std::filesystem::path source(TOKMON_DESK_SOURCE_DIR);
  if (std::filesystem::exists(source / "rml" / "documents" / "main.rml"))
    return source;
#endif
  return std::filesystem::current_path() / "apps" / "tokmon-desk";
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
  return true;
}

bool prepare_visual_state(Rml::Context& context,
                          Rml::ElementDocument& document,
                          const std::string_view state,
                          std::string& error) {
  context.Update();
  std::string detail;
  const auto click = [&](const char* id) {
    return click_element(context, document.GetElementById(id), detail);
  };
  const auto open_right = [&] {
    auto* panel = document.GetElementById("right-panel");
    return panel && (!panel->IsClassSet("hidden") || click("right-toggle"));
  };
  if (state.empty() || state == "default")
    return true;
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
    return open_right() && click("files-tab");
  } else if (state == "disabled") {
    if (auto* send = document.GetElementById("send-button")) {
      send->SetAttribute("disabled", "");
      send->SetClass("acceptance-disabled", true);
      return true;
    }
  } else if (state == "loading") {
    if (!open_right())
      return false;
    if (auto* empty = document.GetElementById("review-empty")) {
      empty->SetClass("hidden", false);
      empty->SetInnerRML(
          "<strong>正在载入工作区更改…</strong><span>Git 状态与差异在后台计算，界面保持可交互</span>");
      return true;
    }
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
  } else if (state == "trajectory") {
    return click("trajectory-mode");
  } else if (state == "environment") {
    return click("environment-toggle");
  } else if (state == "new-session") {
    return click("new-session-button");
  } else if (state == "right-review") {
    return open_right() && click("review-tab");
  } else if (state == "right-files") {
    return open_right() && click("files-tab");
  } else if (state == "right-terminal") {
    return open_right() && click("terminal-tab");
  } else if (state.starts_with("settings-")) {
    if (!click("settings-button"))
      return false;
    const auto page = state.substr(std::string_view("settings-").size());
    Rml::ElementList pages;
    document.QuerySelectorAll(pages, "[data-page]");
    for (auto* item : pages)
      if (item->GetAttribute<Rml::String>("data-page", "") == page)
        return click_element(context, item, detail);
  }
  error = "unknown or unavailable visual state: " + std::string(state) +
          (detail.empty() ? "" : " (" + detail + ")");
  return false;
}

bool write_interaction_report(SdlPlatform& platform,
                              Rml::Context& context,
                              Rml::ElementDocument& document,
                              const std::filesystem::path& workspace,
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

  std::string detail;
  bool clicked = click_id("environment-toggle", detail);
  record("UI-008/009-a", "环境信息通过真实 RmlUi 点击打开",
         clicked && !hidden("environment-panel"), detail);
  clicked = click_id("environment-toggle", detail);
  record("UI-008/009-b", "环境信息通过真实 RmlUi 点击关闭",
         clicked && hidden("environment-panel"), detail);

  clicked = click_id("trajectory-mode", detail);
  record("UI-008/015-a", "切换到轨迹页并更新选中态",
         clicked && active("trajectory-mode") && !hidden("trajectory"), detail);
  clicked = click_id("chat-mode", detail);
  record("UI-008/015-b", "切回对话页并恢复选中态",
         clicked && active("chat-mode") && hidden("trajectory"), detail);

  Rml::ElementList starter_cards;
  document.QuerySelectorAll(starter_cards, "[data-starter]");
  auto* composer = dynamic_cast<Rml::ElementFormControl*>(
      document.GetElementById("composer"));
  const bool starter_clicked = !starter_cards.empty() &&
      click_element(context, starter_cards.front(), detail);
  const bool starter_value = composer &&
      std::string_view(composer->GetValue()).starts_with("请全面分析");
  record("UI-010", "Starter 卡片填入对应提示词",
         starter_clicked && starter_value, detail);
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
    composer_popover->QuerySelectorAll(slash_rows, "[data-command]");
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

  const bool network_before = active("network-toggle");
  clicked = click_id("network-toggle", detail);
  const bool network_changed = active("network-toggle") != network_before;
  const bool network_restore_click = click_id("network-toggle", detail);
  record("UI-012", "Composer 联网 pill 切换并恢复",
         clicked && network_changed && network_restore_click &&
             active("network-toggle") == network_before,
         detail);

  auto* shell = document.GetElementById("app-shell");
  const bool right_starts_collapsed = hidden("right-panel") && shell &&
      shell->IsClassSet("right-hidden");
  const bool right_start_open = click_id("right-toggle", detail);
  record("UI-016-startup", "右栏按旧版规则默认收起并可展开",
         right_starts_collapsed && right_start_open && !hidden("right-panel") &&
             shell && !shell->IsClassSet("right-hidden"), detail);

  const auto dispatch_pointer = [&](const Uint32 type, const float logical_x,
                                    const float logical_y) {
    SDL_Event pointer{};
    pointer.type = type;
    const float coordinate_scale = platform.input_coordinate_scale();
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
    if (!SDL_PushEvent(&pointer))
      return false;
    bool quit = false;
    bool resized = false;
    while (platform.pump_event(context, quit, resized)) {}
    context.Update();
    return !quit;
  };
  auto* sidebar = document.GetElementById("sidebar");
  auto* right_panel = document.GetElementById("right-panel");
  auto* workspace_element = document.GetElementById("workspace");
  const bool left_down = dispatch_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, 240.f, 100.f);
  const bool left_move = dispatch_pointer(SDL_EVENT_MOUSE_MOTION, 280.f, 100.f);
  const bool left_up = dispatch_pointer(SDL_EVENT_MOUSE_BUTTON_UP, 280.f, 100.f);
  const bool left_resized = sidebar && workspace_element &&
      std::abs(sidebar->GetOffsetWidth() - 280.f) <= 1.f &&
      std::abs(workspace_element->GetAbsoluteOffset(Rml::BoxArea::Border).x -
               280.f) <= 1.f;
  const bool left_restore_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN, 280.f, 100.f);
  const bool left_restore_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION, 240.f, 100.f);
  const bool left_restore_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP, 240.f, 100.f);
  const bool left_restored = sidebar &&
      std::abs(sidebar->GetOffsetWidth() - 240.f) <= 1.f;
  record("UI-002/006-resize", "左分栏经 SDL 指针事件拖拽并恢复",
         left_down && left_move && left_up && left_resized &&
             left_restore_down && left_restore_move && left_restore_up &&
             left_restored,
         "sidebar 240 -> 280 -> " +
             std::to_string(sidebar ? sidebar->GetOffsetWidth() : -1.f));

  const float viewport_width = document.GetClientWidth();
  const bool right_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN, viewport_width - 440.f, 100.f);
  const bool right_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION, viewport_width - 500.f, 100.f);
  const bool right_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP, viewport_width - 500.f, 100.f);
  const bool right_resized = right_panel &&
      std::abs(right_panel->GetOffsetWidth() - 500.f) <= 1.f;
  const bool right_restore_down = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_DOWN, viewport_width - 500.f, 100.f);
  const bool right_restore_move = dispatch_pointer(
      SDL_EVENT_MOUSE_MOTION, viewport_width - 440.f, 100.f);
  const bool right_restore_up = dispatch_pointer(
      SDL_EVENT_MOUSE_BUTTON_UP, viewport_width - 440.f, 100.f);
  const bool right_restored = right_panel &&
      std::abs(right_panel->GetOffsetWidth() - 440.f) <= 1.f;
  record("UI-002/016-resize", "右分栏经 SDL 指针事件拖拽并恢复",
         right_down && right_move && right_up && right_resized &&
             right_restore_down && right_restore_move && right_restore_up &&
             right_restored,
         "right panel 440 -> 500 -> " +
             std::to_string(right_panel ? right_panel->GetOffsetWidth() : -1.f));

  clicked = click_id("sidebar-toggle", detail);
  const bool sidebar_collapsed = clicked && hidden("sidebar") && shell &&
      shell->IsClassSet("sidebar-hidden");
  const bool sidebar_restore_click = click_id("sidebar-toggle", detail);
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

  clicked = click_id("settings-button", detail);
  const bool settings_open = clicked && !hidden("settings-overlay");
  record("UI-007/031", "设置壳通过真实点击打开",
         settings_open, detail);
  Rml::ElementList settings_pages;
  document.QuerySelectorAll(settings_pages, "[data-page]");
  const std::vector<std::tuple<std::string_view, std::string_view,
                               std::string_view>> settings_contracts{
      {"general", "UI-032", "通用"},
      {"model", "UI-033", "智能体与模型"},
      {"access", "UI-034", "权限与安全"},
      {"workspace", "UI-035", "工作区"},
      {"notifications", "UI-036", "通知"},
      {"appearance", "UI-037", "外观"},
      {"shortcuts", "UI-038", "快捷键"},
      {"account", "UI-039", "账户"},
      {"terminal", "UI-040", "终端"},
      {"about", "UI-041", "关于"}};
  auto* settings_title = document.GetElementById("settings-title");
  auto* settings_content = document.GetElementById("settings-body");
  for (const auto& [page, id, expected_title] : settings_contracts) {
    Rml::Element* navigation = nullptr;
    for (auto* page_item : settings_pages) {
      if (page_item->GetAttribute<Rml::String>("data-page", "") == page) {
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
    const auto page = page_item->GetAttribute<Rml::String>("data-page", "");
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
  const bool settings_close = click_id("close-settings", detail);
  record("UI-007/031-close", "设置通过关闭按钮退出",
         settings_close && hidden("settings-overlay"),
         detail);

  clicked = click_id("files-tab", detail);
  record("UI-017/025", "右栏切换到文件页",
         clicked && active("files-tab") && !hidden("files-view"), detail);

  const auto unique = std::to_string(static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
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

  clicked = click_id("terminal-tab", detail);
  const auto* terminal_status = document.GetElementById("terminal-status");
  const auto terminal_status_text = terminal_status
      ? terminal_status->GetInnerRML() : std::string("<missing>");
  const bool terminal_running = clicked && active("terminal-tab") &&
      !hidden("terminal-view") && terminal_status &&
      terminal_status_text.find("正在运行") != std::string::npos;
  record("UI-017/028/TERM-002", "右栏启动跨平台 Terminal profile",
         terminal_running, detail + "; active=" +
             (active("terminal-tab") ? "true" : "false") +
             "; hidden=" + (hidden("terminal-view") ? "true" : "false") +
             "; status=" + terminal_status_text);
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

  clicked = click_id("review-tab", detail);
  record("UI-017/018", "右栏切回 Review",
         clicked && active("review-tab") && !hidden("review-view"), detail);
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

  clicked = click_id("right-fullscreen", detail);
  const bool fullscreen = clicked && shell && shell->IsClassSet("right-fullscreen");
  const bool fullscreen_restore = click_id("right-fullscreen", detail);
  record("UI-016-a", "右栏全屏并恢复", fullscreen && fullscreen_restore &&
         shell && !shell->IsClassSet("right-fullscreen"), detail);
  clicked = click_id("right-collapse", detail);
  const bool right_collapsed = clicked && hidden("right-panel") && shell &&
      shell->IsClassSet("right-hidden");
  const bool right_restore = click_id("right-toggle", detail);
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
  const float workspace_width = static_cast<float>(logical_width - 680);
  const float composer_width = std::min(760.f, workspace_width - 48.f);
  const float composer_x = 240.f + (workspace_width - composer_width) / 2.f;
  const std::vector<Contract> contracts{
      {"sidebar", 0.f, 0.f, 240.f, static_cast<float>(logical_height)},
      {"workspace", 240.f, 0.f, workspace_width,
       static_cast<float>(logical_height)},
      {"right-panel", static_cast<float>(logical_width - 440), 0.f, 440.f,
       static_cast<float>(logical_height)},
      {"brand-row", 0.f, 0.f, 240.f, 48.f},
      {"workspace-titlebar", 240.f, 0.f, workspace_width, 46.f},
      {"right-titlebar", static_cast<float>(logical_width - 440), 0.f, 440.f,
       46.f},
      {"right-tabs", static_cast<float>(logical_width - 440), 46.f, 440.f,
       44.f},
      {"composer-wrap", composer_x, static_cast<float>(logical_height - 164),
       composer_width, 164.f},
      {"composer-card", composer_x, static_cast<float>(logical_height - 116),
       composer_width, 100.f}};

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
    const auto offset = element
        ? element->GetAbsoluteOffset(Rml::BoxArea::Border) : Rml::Vector2f{};
    const float width = element ? element->GetOffsetWidth() : -1.f;
    const float height = element ? element->GetOffsetHeight() : -1.f;
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

SDL_HitTestResult hit_test(SDL_Window* window, const SDL_Point* point, void*) {
  (void)window;
  // Keep the dedicated brand strip draggable, but never place a native drag
  // hit-test region over RmlUi title-bar controls. SDL consumes those pointer
  // events before RmlUi can dispatch them.
  if (point->y < 42 && point->x >= 30 && point->x < 210)
    return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}

} // namespace

int run_desk(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto paths = DeskAppPaths::resolve();
    std::string error;
    if (!paths.ensure(error)) {
      std::cerr << "tokmon-desk: " << error << '\n';
      return 2;
    }
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
    SDL_SetWindowHitTest(platform.window(), hit_test, nullptr);

    const int ui_scale_percent = arguments.ui_scale_percent > 0
                                     ? arguments.ui_scale_percent
                                     : platform.default_ui_scale_percent();
    const int content_scale_percent = arguments.content_scale_percent > 0
        ? arguments.content_scale_percent
        : arguments.ui_scale_percent > 0
              ? 100
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
    device->set_ui_scale(ui_scale);
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

    const auto resources = resource_root();
    const auto font = resources / "assets" / "fonts" / "MiSansVF.ttf";
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
#if defined(_WIN32)
    const std::filesystem::path terminal_font = "C:/Windows/Fonts/consola.ttf";
    for (const auto& fallback : {std::filesystem::path("C:/Windows/Fonts/seguisym.ttf"),
                                 std::filesystem::path("C:/Windows/Fonts/seguiemj.ttf")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#elif defined(__APPLE__)
    const std::filesystem::path terminal_font = "/System/Library/Fonts/Menlo.ttc";
    for (const auto& fallback : {std::filesystem::path("/System/Library/Fonts/Apple Symbols.ttf"),
                                 std::filesystem::path("/System/Library/Fonts/Apple Color Emoji.ttc")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#else
    const std::filesystem::path terminal_font = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    for (const auto& fallback : {std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
                                 std::filesystem::path("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf")})
      if (std::filesystem::exists(fallback))
        (void)Rml::LoadFontFace(fallback.generic_string(), true);
#endif
    if (std::filesystem::exists(terminal_font))
      (void)Rml::LoadFontFace(terminal_font.generic_string(), false);
    auto* context = Rml::CreateContext("tokmon-desk",
        {device->logical_width(), device->logical_height()}, nullptr);
    if (!context) {
      std::cerr << "tokmon-desk: could not create RmlUi context\n";
      Rml::Shutdown();
      return 7;
    }
    context->SetDensityIndependentPixelRatio(platform.display_scale());
    auto* document = context->LoadDocument(
        (resources / "rml" / "documents" / "main.rml").generic_string());
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
    DeskController controller(*document, platform, arguments.workspace, paths,
                              daemon_endpoint);
    controller.bind(!arguments.smoke_test &&
                    arguments.ui_contract_report.empty());

    if (!arguments.visual_state.empty() &&
        !prepare_visual_state(*context, *document, arguments.visual_state,
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
                                    arguments.logical_width,
                                    arguments.logical_height,
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
      if (!write_interaction_report(platform, *context, *document, arguments.workspace,
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
      controller.prepare_legacy_three_pane_contract();
      controller.seed_acceptance_conversation(10000);
      context->Update();
      const auto conversation_turns = controller.conversation_turn_count();
      const auto conversation_dom_nodes = descendant_count(*conversation);
      const auto trajectory_dom_nodes = descendant_count(*trajectory);
      document->GetElementById("app-shell")->SetClass("right-fullscreen", true);
      files_view->SetClass("hidden", false);
      review_view->SetClass("hidden", true);
      std::vector<WorkspaceEntry> rows;
      rows.reserve(10000);
      for (std::size_t index = 0; index < 10000; ++index)
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

      review_diff->SetInnerRML(
          "<tokmon-diff-surface id='acceptance-diff' class='diff-surface'>"
          "</tokmon-diff-surface>");
      auto* diff = dynamic_cast<ElementDiffSurface*>(
          document->GetElementById("acceptance-diff"));
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
      std::error_code report_error;
      std::filesystem::create_directories(
          arguments.acceptance_report.parent_path(), report_error);
      std::ofstream report(arguments.acceptance_report,
                           std::ios::binary | std::ios::trunc);
      report << "{\n"
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
             << "  \"rmlWidth\": " << device->logical_width() << ",\n"
             << "  \"rmlHeight\": " << device->logical_height() << ",\n"
             << "  \"conversationModelTurns\": " << conversation_turns << ",\n"
             << "  \"conversationDomNodes\": " << conversation_dom_nodes << ",\n"
             << "  \"trajectoryDomNodes\": " << trajectory_dom_nodes << ",\n"
             << "  \"fileTreeModelRows\": 10000,\n"
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
             << "\n"
             << "}\n";
      const bool accepted = report && conversation_turns == 10000 &&
                            conversation_dom_nodes > 0 &&
                            conversation_dom_nodes < 2000 &&
                            trajectory_dom_nodes > 0 &&
                            trajectory_dom_nodes < 2500 &&
                            tree_rendered > 0 && tree_rendered < 200 &&
                            tree_dom_children == 0 && editor_lines >= 100000 &&
                            editor_rendered > 0 && editor_rendered < 200 &&
                            editor_dom_children == 0 && diff_lines >= 4000 &&
                            diff_rendered > 0 && diff_rendered < 200 &&
                            diff_dom_children == 0 && split_diff_rendered > 0 &&
                            split_diff_rendered < 200 &&
                            terminal_processed_bytes == terminal_stress_bytes &&
                            terminal_max_chunk_ms < 50.0 &&
                            terminal_snapshot_cells > 0 &&
                            terminal_dom_children == 0 &&
                            input_to_frame_samples_ms.size() == 64 &&
                            input_to_frame_p95_ms <= 50.0;
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
          std::filesystem::path(SDL_GetBasePath()) / "tokmon.exe";
#else
          std::filesystem::path(SDL_GetBasePath()) / "tokmon";
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
      if (resized && !device->resize(platform.pixel_width(), platform.pixel_height(), error)) {
        std::cerr << "tokmon-desk: resize failed: " << error << '\n';
        break;
      }
      if (resized)
        context->SetDimensions({device->logical_width(), device->logical_height()});
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
        device->begin_frame();
        context->Render();
        if (!device->end_frame(error)) {
          std::cerr << "tokmon-desk: present failed: " << error << '\n';
          break;
        }
        ++frames;
        redraw = false;
      }
      if (!arguments.screenshot.empty() && !screenshot_written && frames >= 3) {
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
