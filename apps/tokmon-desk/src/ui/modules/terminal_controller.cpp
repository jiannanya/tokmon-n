#include "ui/modules/terminal_controller.hpp"

#include "platform/sdl_platform.hpp"
#include "ui/desk_view_model.hpp"
#include "ui/elements/element_terminal.hpp"
#include "ui/modules/settings_controller.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace tokmon::desk {
namespace {

std::uint16_t modifiers(const SDL_Keymod value) {
  std::uint16_t result = 0;
  if (value & SDL_KMOD_SHIFT) result |= terminal_shift;
  if (value & SDL_KMOD_CTRL) result |= terminal_ctrl;
  if (value & SDL_KMOD_ALT) result |= terminal_alt;
  if (value & SDL_KMOD_GUI) result |= terminal_super;
  if (value & SDL_KMOD_CAPS) result |= terminal_caps_lock;
  if (value & SDL_KMOD_NUM) result |= terminal_num_lock;
  return result;
}

TerminalKey key_code(const SDL_Keycode value) {
  if (value >= SDLK_A && value <= SDLK_Z)
    return static_cast<TerminalKey>(static_cast<int>(TerminalKey::key_a) +
                                    (value - SDLK_A));
  if (value >= SDLK_0 && value <= SDLK_9)
    return static_cast<TerminalKey>(static_cast<int>(TerminalKey::digit_0) +
                                    (value - SDLK_0));
  switch (value) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return TerminalKey::enter;
    case SDLK_BACKSPACE: return TerminalKey::backspace;
    case SDLK_TAB: return TerminalKey::tab;
    case SDLK_ESCAPE: return TerminalKey::escape;
    case SDLK_SPACE: return TerminalKey::space;
    case SDLK_LEFT: return TerminalKey::left;
    case SDLK_RIGHT: return TerminalKey::right;
    case SDLK_UP: return TerminalKey::up;
    case SDLK_DOWN: return TerminalKey::down;
    case SDLK_HOME: return TerminalKey::home;
    case SDLK_END: return TerminalKey::end;
    case SDLK_PAGEUP: return TerminalKey::page_up;
    case SDLK_PAGEDOWN: return TerminalKey::page_down;
    case SDLK_INSERT: return TerminalKey::insert_key;
    case SDLK_DELETE: return TerminalKey::delete_key;
    case SDLK_F1: return TerminalKey::f1;
    case SDLK_F2: return TerminalKey::f2;
    case SDLK_F3: return TerminalKey::f3;
    case SDLK_F4: return TerminalKey::f4;
    case SDLK_F5: return TerminalKey::f5;
    case SDLK_F6: return TerminalKey::f6;
    case SDLK_F7: return TerminalKey::f7;
    case SDLK_F8: return TerminalKey::f8;
    case SDLK_F9: return TerminalKey::f9;
    case SDLK_F10: return TerminalKey::f10;
    case SDLK_F11: return TerminalKey::f11;
    case SDLK_F12: return TerminalKey::f12;
    default: return TerminalKey::unidentified;
  }
}

std::string key_text(const SDL_Keycode value) {
  if (value >= SDLK_A && value <= SDLK_Z)
    return std::string(1, static_cast<char>('a' + value - SDLK_A));
  if (value >= SDLK_0 && value <= SDLK_9)
    return std::string(1, static_cast<char>('0' + value - SDLK_0));
  return value == SDLK_SPACE ? " " : std::string{};
}

std::string printable(const SDL_Keycode value, const SDL_Keymod mods) {
  const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
  const bool caps = (mods & SDL_KMOD_CAPS) != 0;
  if (value >= SDLK_A && value <= SDLK_Z) {
    char character = static_cast<char>('a' + value - SDLK_A);
    if (shift != caps)
      character = static_cast<char>(std::toupper(
          static_cast<unsigned char>(character)));
    return std::string(1, character);
  }
  if (value >= SDLK_0 && value <= SDLK_9) {
    constexpr std::string_view shifted = ")!@#$%^&*(";
    return std::string(1, shift ? shifted[static_cast<std::size_t>(value - SDLK_0)]
                                : static_cast<char>('0' + value - SDLK_0));
  }
  switch (value) {
    case SDLK_SPACE: return " "; case SDLK_MINUS: return shift ? "_" : "-";
    case SDLK_EQUALS: return shift ? "+" : "=";
    case SDLK_LEFTBRACKET: return shift ? "{" : "[";
    case SDLK_RIGHTBRACKET: return shift ? "}" : "]";
    case SDLK_BACKSLASH: return shift ? "|" : "\\";
    case SDLK_SEMICOLON: return shift ? ":" : ";";
    case SDLK_APOSTROPHE: return shift ? "\"" : "'";
    case SDLK_GRAVE: return shift ? "~" : "`";
    case SDLK_COMMA: return shift ? "<" : ",";
    case SDLK_PERIOD: return shift ? ">" : ".";
    case SDLK_SLASH: return shift ? "?" : "/";
    default: return {};
  }
}

} // namespace

TerminalController::TerminalController(SdlPlatform& platform,
                                       DeskViewModel& view_model,
                                       SettingsController& settings,
                                       std::filesystem::path workspace)
    : platform_(platform), view_model_(view_model), settings_(settings),
      workspace_(std::move(workspace)) {}

TerminalController::~TerminalController() {
  for (auto& tab : tabs_)
    tab->session->stop();
}

void TerminalController::attach(Rml::ElementDocument& document) {
  document_ = &document;
  if (tabs_.empty())
    create_tab();
}

void TerminalController::set_workspace(std::filesystem::path workspace) {
  for (auto& tab : tabs_)
    tab->session->stop();
  tabs_.clear();
  active_index_ = 0;
  workspace_ = std::move(workspace);
  if (document_)
    create_tab();
}

TerminalController::Tab& TerminalController::active_tab() {
  if (tabs_.empty())
    create_tab();
  active_index_ = std::min(active_index_, tabs_.size() - 1);
  return *tabs_[active_index_];
}

TerminalSession& TerminalController::session() { return *active_tab().session; }
GhosttyVt& TerminalController::vt() { return *active_tab().vt; }

int TerminalController::cell_width_pixels(const Tab& tab) const {
  const float density = document_ && document_->GetContext()
      ? std::max(document_->GetContext()->GetDensityIndependentPixelRatio(),
                 0.5f)
      : 1.f;
  return std::max(1, static_cast<int>(std::lround(
      static_cast<float>(tab.cell_width) * density)));
}

int TerminalController::cell_height_pixels(const Tab& tab) const {
  const float density = document_ && document_->GetContext()
      ? std::max(document_->GetContext()->GetDensityIndependentPixelRatio(),
                 0.5f)
      : 1.f;
  return std::max(1, static_cast<int>(std::lround(
      static_cast<float>(tab.cell_height) * density)));
}

void TerminalController::set_status(std::string value) {
  if (document_)
    if (auto* element = document_->GetElementById("terminal-status"))
      element->SetInnerRML(std::move(value));
}

void TerminalController::set_hint(std::string value) {
  if (document_)
    if (auto* element = document_->GetElementById("terminal-hint"))
      element->SetInnerRML(std::move(value));
}

void TerminalController::start() {
  auto& tab = active_tab();
  if (tab.started && tab.session->running())
    return;
  if (!tab.launch_error.empty() || tab.launch.executable.empty()) {
    set_status("不可用：" + (tab.launch_error.empty()
        ? std::string("未配置可执行文件") : tab.launch_error));
    return;
  }
  std::string error;
  auto* terminal_session = tab.session.get();
  tab.vt->set_response_sink([terminal_session](const std::string_view response) {
    std::string ignored;
    (void)terminal_session->write(response, ignored);
  });
  (void)tab.vt->resize(tab.columns, tab.rows, cell_width_pixels(tab),
                       cell_height_pixels(tab));
  tab.started = tab.session->start_profile(tab.launch, workspace_, tab.columns,
                                           tab.rows, error);
  set_status(tab.started ? "正在运行" : "不可用：" + error);
  if (tab.started)
    (void)resize();
}

bool TerminalController::resize() {
  if (!document_)
    return false;
  auto* surface = dynamic_cast<ElementTerminal*>(
      document_->GetElementById("terminal-surface"));
  if (!surface)
    return false;
  auto& tab = active_tab();
  const int cell_width = cell_width_pixels(tab);
  const int cell_height = cell_height_pixels(tab);
  const auto client_width = surface->GetClientWidth();
  const auto client_height = surface->GetClientHeight();
  // RmlUi retains the previous box until the visibility/layout pass after a
  // right-panel tab switch. Never collapse a newly opened PTY to the 20x5
  // clamps from a hidden (zero-sized) terminal surface; the following update
  // pass will resize it from the actual visible box.
  if (client_width < static_cast<float>(cell_width) ||
      client_height < static_cast<float>(cell_height))
    return false;
  const auto columns = std::clamp(static_cast<int>(
      client_width / static_cast<float>(cell_width)), 20, 400);
  const auto rows = std::clamp(static_cast<int>(
      client_height / static_cast<float>(cell_height)), 5, 200);
  if (columns == tab.columns && rows == tab.rows)
    return false;
  tab.columns = columns;
  tab.rows = rows;
  std::string error;
  if (tab.started && !tab.session->resize(columns, rows, error))
    tab.vt->append("\r\n" + error + "\r\n");
  (void)tab.vt->resize(columns, rows, cell_width, cell_height);
  surface->set_snapshot(tab.vt->render_snapshot());
  return true;
}

void TerminalController::paste(const bool allow_unsafe) {
  if (!active_tab().started)
    start();
  if (pending_paste_.empty()) {
    Rml::String clipboard;
    platform_.GetClipboardText(clipboard);
    pending_paste_ = clipboard;
  }
  if (pending_paste_.empty())
    return;
  const auto result = vt().paste(pending_paste_, allow_unsafe);
  if (result == TerminalPasteResult::unsafe) {
    if (document_)
      if (auto* overlay = document_->GetElementById("terminal-paste-overlay"))
        overlay->SetClass("hidden", false);
    return;
  }
  if (result == TerminalPasteResult::failed)
    vt().append("\r\nTerminal paste failed\r\n");
  pending_paste_.clear();
  if (document_)
    if (auto* overlay = document_->GetElementById("terminal-paste-overlay"))
      overlay->SetClass("hidden", true);
}

void TerminalController::cancel_paste() {
  pending_paste_.clear();
  if (document_)
    if (auto* overlay = document_->GetElementById("terminal-paste-overlay"))
      overlay->SetClass("hidden", true);
}

void TerminalController::search() {
  if (!document_)
    return;
  auto* surface = dynamic_cast<ElementTerminal*>(
      document_->GetElementById("terminal-surface"));
  auto* input = dynamic_cast<Rml::ElementFormControl*>(
      document_->GetElementById("terminal-search"));
  if (!surface || !input)
    return;
  surface->set_search(input->GetValue());
  if (auto* count = document_->GetElementById("terminal-search-count"))
    count->SetInnerRML(input->GetValue().empty() ? std::string{} :
        std::to_string(surface->search_match_count()) + " 个");
}

void TerminalController::create_tab() {
  auto tab = std::make_unique<Tab>();
  tab->id = "terminal-" + std::to_string(next_id_++);
  const auto profile_id = settings_.string("terminal_profile", "auto");
  tab->font_size = static_cast<int>(std::clamp<std::int64_t>(
      settings_.integer("terminal_font_size", 13), 9, 24));
  tab->cell_width = std::max(6, static_cast<int>(std::lround(
      static_cast<double>(tab->font_size) * 0.62)));
  tab->cell_height = tab->font_size + 4;
  if (profile_id == "custom") {
    const auto executable = std::filesystem::path(
        settings_.string("terminal_executable"));
    std::error_code path_error;
    if (executable.empty() ||
        !std::filesystem::is_regular_file(executable, path_error)) {
      tab->launch_error = "自定义终端可执行文件不存在或不是普通文件";
    } else {
      std::string argument_error;
      const auto arguments = parse_terminal_arguments(
          settings_.string("terminal_arguments"), argument_error);
      if (!arguments)
        tab->launch_error = argument_error;
      else
        tab->launch = {executable, *arguments};
    }
    tab->title = "自定义 " + std::to_string(next_id_ - 1);
  } else {
    const auto profile = resolve_terminal_profile(profile_id);
    tab->title = (profile ? profile->label : "Shell") + " " +
                 std::to_string(next_id_ - 1);
    if (profile)
      tab->launch = {profile->executable, profile->arguments};
    else
      tab->launch_error = "没有可用的系统 Shell";
  }
  tab->session = std::make_unique<TerminalSession>();
  tab->vt = std::make_unique<GhosttyVt>(100, 28,
      static_cast<std::size_t>(std::clamp<std::int64_t>(
          settings_.integer("terminal_scrollback", 10000), 1000, 100000)));
  tabs_.push_back(std::move(tab));
  active_index_ = tabs_.size() - 1;
  render_tabs();
  if (document_)
    if (auto* surface = document_->GetElementById("terminal-surface"))
      surface->SetProperty("font-size",
                           std::to_string(active_tab().font_size) + "dp");
}

void TerminalController::close_tab() {
  if (tabs_.empty())
    return;
  tabs_[active_index_]->session->stop();
  tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(active_index_));
  if (tabs_.empty())
    create_tab();
  else {
    active_index_ = std::min(active_index_, tabs_.size() - 1);
    select_tab(tabs_[active_index_]->id);
  }
}

void TerminalController::select_tab(const std::string_view id) {
  const auto found = std::ranges::find_if(tabs_, [&](const auto& tab) {
    return tab->id == id;
  });
  if (found == tabs_.end())
    return;
  active_index_ = static_cast<std::size_t>(std::distance(tabs_.begin(), found));
  render_tabs();
  auto& tab = active_tab();
  if (document_)
    if (auto* surface = dynamic_cast<ElementTerminal*>(
            document_->GetElementById("terminal-surface"))) {
      surface->SetProperty("font-size", std::to_string(tab.font_size) + "dp");
      surface->set_snapshot(tab.vt->render_snapshot());
    }
  set_status(tab.started ? "正在运行" : "未启动");
  search();
}

void TerminalController::render_tabs() {
  auto& rows = view_model_.state().terminal_tabs;
  rows.clear();
  for (std::size_t index = 0; index < tabs_.size(); ++index)
    rows.push_back({tabs_[index]->id, tabs_[index]->title,
                    index == active_index_});
  view_model_.dirty();
}

void TerminalController::clear_search() {
  if (document_)
    if (auto* input = dynamic_cast<Rml::ElementFormControl*>(
            document_->GetElementById("terminal-search")))
      input->SetValue("");
  search();
}

void TerminalController::release_focus() {
  pending_keydown_text_.clear();
  platform_.DeactivateKeyboard();
  set_hint("点击终端后直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Ctrl+单击打开 OSC 8 链接 · Esc 释放焦点");
}

bool TerminalController::handle_pointer(Rml::Event& event) {
  if (!document_)
    return false;
  auto* element = event.GetCurrentElement();
  auto* surface = dynamic_cast<ElementTerminal*>(element);
  if (!surface)
    return false;
  const auto absolute = element->GetAbsoluteOffset(Rml::BoxArea::Content);
  if (event.GetType() == "click") {
    element->Focus(); start(); (void)resize();
    platform_.ActivateKeyboard(
        absolute, static_cast<float>(cell_height_pixels(active_tab())));
    if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0) {
      const auto link = surface->hyperlink_at(
          static_cast<float>(event.GetParameter<int>("mouse_x", 0)) - absolute.x,
          static_cast<float>(event.GetParameter<int>("mouse_y", 0)) - absolute.y);
      if (terminal_safe_hyperlink(link))
        (void)SDL_OpenURL(link.c_str());
    }
    return true;
  }
  if (event.GetType() != "mousedown" && event.GetType() != "mousemove" &&
      event.GetType() != "mouseup")
    return false;
  if (event.GetType() == "mousedown") {
    element->Focus(); start(); (void)resize();
    platform_.ActivateKeyboard(
        absolute, static_cast<float>(cell_height_pixels(active_tab())));
    set_status("正在运行 · 已聚焦");
    set_hint("终端已聚焦 · 直接输入 · Ctrl+Shift+C/V 复制/粘贴 · Esc 释放焦点");
  }
  const float x = static_cast<float>(event.GetParameter<int>("mouse_x", 0)) - absolute.x;
  const float y = static_cast<float>(event.GetParameter<int>("mouse_y", 0)) - absolute.y;
  const bool force_selection = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
  auto& tab = active_tab();
  const int cell_width = cell_width_pixels(tab);
  const int cell_height = cell_height_pixels(tab);
  if (tab.vt->mouse_tracking() && !force_selection) {
    TerminalMouseAction action = TerminalMouseAction::motion;
    if (event.GetType() == "mousedown") { action = TerminalMouseAction::press; mouse_down_ = true; }
    else if (event.GetType() == "mouseup") { action = TerminalMouseAction::release; mouse_down_ = false; }
    const auto bytes = tab.vt->encode_mouse(action,
        event.GetType() == "mousemove" ? TerminalMouseButton::none : TerminalMouseButton::left,
        x, y, static_cast<int>(element->GetClientWidth()),
        static_cast<int>(element->GetClientHeight()), cell_width,
        cell_height, modifiers(SDL_GetModState()), mouse_down_);
    std::string error;
    if (!bytes.empty() && !tab.session->write(bytes, error))
      tab.vt->append("\r\n" + error + "\r\n");
  } else if (event.GetType() == "mousedown") surface->begin_selection(x, y);
  else if (event.GetType() == "mousemove") surface->update_selection(x, y);
  else surface->end_selection();
  return true;
}

bool TerminalController::handle_wheel(const SDL_Event& event) {
  auto& tab = active_tab();
  const int cell_width = cell_width_pixels(tab);
  const int cell_height = cell_height_pixels(tab);
  if (tab.vt->mouse_tracking()) {
    const auto button = event.wheel.y > 0 ? TerminalMouseButton::wheel_up
                                         : TerminalMouseButton::wheel_down;
    const auto bytes = tab.vt->encode_mouse(TerminalMouseAction::press, button,
        event.wheel.mouse_x, event.wheel.mouse_y,
        tab.columns * cell_width, tab.rows * cell_height,
        cell_width, cell_height, modifiers(SDL_GetModState()));
    std::string error;
    if (!bytes.empty() && !tab.session->write(bytes, error))
      tab.vt->append("\r\n" + error + "\r\n");
  } else {
    tab.vt->scroll_viewport(event.wheel.y > 0 ? -3 : 3);
    if (document_)
      if (auto* surface = dynamic_cast<ElementTerminal*>(
              document_->GetElementById("terminal-surface")))
        surface->set_snapshot(tab.vt->render_snapshot());
  }
  return true;
}

bool TerminalController::handle_text(const SDL_Event& event) {
  set_status("正在运行 · 已接收输入");
  const std::string incoming(event.text.text);
  if (!pending_keydown_text_.empty() && pending_keydown_text_.starts_with(incoming))
    pending_keydown_text_.erase(0, incoming.size());
  const auto bytes = vt().encode_key(TerminalKey::unidentified, incoming,
                                     modifiers(SDL_GetModState()));
  std::string error;
  if (!bytes.empty() && !session().write(bytes, error))
    vt().append("\r\n" + error + "\r\n");
  return true;
}

bool TerminalController::handle_key(const SDL_Event& event) {
  const auto mods = event.key.mod;
  const bool control = (mods & SDL_KMOD_CTRL) != 0;
  const bool shift = (mods & SDL_KMOD_SHIFT) != 0;
  if (control && shift && event.key.key == SDLK_C) {
    if (document_)
      if (auto* surface = dynamic_cast<ElementTerminal*>(
              document_->GetElementById("terminal-surface")))
        if (const auto selected = surface->selected_text(); !selected.empty())
          platform_.SetClipboardText(selected);
    return true;
  }
  if (control && shift && event.key.key == SDLK_V) {
    paste(false);
    return true;
  }
  const auto printable_text = printable(event.key.key, mods);
  if (!printable_text.empty() && !control &&
      !(mods & (SDL_KMOD_ALT | SDL_KMOD_GUI))) {
    pending_keydown_text_ += printable_text;
    return true;
  }
  const auto bytes = vt().encode_key(key_code(event.key.key),
      key_text(event.key.key), modifiers(mods), event.key.repeat);
  std::string error;
  if (!bytes.empty() && !session().write(bytes, error))
    vt().append("\r\n" + error + "\r\n");
  return true;
}

bool TerminalController::update() {
  // The panel can become visible or change width after event dispatch. Keep
  // the Ghostty grid synchronized with the settled RmlUi content box even
  // when the shell has not emitted output yet.
  bool changed = resize();
  if (!pending_keydown_text_.empty()) {
    const auto pending = std::move(pending_keydown_text_);
    pending_keydown_text_.clear();
    const auto bytes = vt().encode_key(TerminalKey::unidentified, pending, 0);
    std::string error;
    if (!bytes.empty() && !session().write(bytes, error))
      vt().append("\r\n" + error + "\r\n");
    changed = true;
  }
  std::size_t budget = 256u * 1024u;
  for (std::size_t index = 0; index < tabs_.size() && budget > 0; ++index) {
    auto& tab = *tabs_[index];
    if (!tab.started)
      continue;
    const auto output = tab.session->take_output(
        std::min<std::size_t>(64u * 1024u, budget));
    if (output.empty())
      continue;
    changed = true;
    budget -= output.size();
    tab.vt->append(output);
    if (index == active_index_ && document_)
      if (auto* surface = dynamic_cast<ElementTerminal*>(
              document_->GetElementById("terminal-surface"))) {
        set_status("正在运行 · 已更新");
        surface->set_snapshot(tab.vt->render_snapshot());
        search();
      }
  }
  return changed;
}

bool TerminalController::running() const noexcept {
  return !tabs_.empty() && active_index_ < tabs_.size() &&
         tabs_[active_index_]->started;
}

} // namespace tokmon::desk
