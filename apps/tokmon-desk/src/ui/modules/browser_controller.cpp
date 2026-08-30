#include "ui/modules/browser_controller.hpp"

#include "ui/desk_view_model.hpp"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <chrono>

namespace tokmon::desk {
namespace {

Rml::ElementFormControl* control(Rml::ElementDocument* document,
                                 const char* id) {
  return document ? dynamic_cast<Rml::ElementFormControl*>(
                        document->GetElementById(id)) : nullptr;
}

} // namespace

BrowserController::BrowserController(std::filesystem::path data_root,
                                     DeskViewModel& view_model)
    : manager_(std::move(data_root)), view_model_(view_model) {}

BrowserController::~BrowserController() {
  if (future_.valid())
    future_.wait();
  std::string ignored;
  (void)manager_.close(session_, ignored);
}

void BrowserController::attach(Rml::ElementDocument& document) {
  document_ = &document;
  auto& state = view_model_.state();
  if (const auto executable = discovered_executable(); !executable.empty())
    state.settings.browser_executable = executable.generic_string();
  view_model_.dirty();
}

std::filesystem::path BrowserController::discovered_executable() const {
  const auto candidates = manager_.discover();
  return candidates.empty() ? std::filesystem::path{} : candidates.front().executable;
}

void BrowserController::launch() {
  if (future_.valid() || takeover_)
    return;
  const auto candidates = manager_.discover();
  auto& view = view_model_.state();
  if (candidates.empty()) {
    view.browser_title = "未找到 Chrome / Chromium";
    view.browser_detail = "可在设置中指定浏览器路径";
    view.browser_running = false;
    view_model_.dirty();
    return;
  }
  const auto executable = candidates.front().executable;
  const std::string url = view.browser_url.empty() ? "about:blank"
                                                    : view.browser_url;
  view.browser_title = "正在准备 Agent Browser…";
  view.browser_detail = "首次使用会安装并校验固定版本 Runtime";
  view.browser_running = false;
  view_model_.dirty();
  future_ = std::async(std::launch::async, [this, executable, url] {
    std::string error;
    if (!manager_.install_runtime(error)) {
      BrowserSessionState state;
      state.error = std::move(error);
      return state;
    }
    return manager_.open(executable, session_, url, true);
  });
}

void BrowserController::refresh() {
  if (!future_.valid())
    future_ = std::async(std::launch::async,
                         [this] { return manager_.refresh(session_); });
}

void BrowserController::back() {
  if (!future_.valid() && !takeover_)
    future_ = std::async(std::launch::async,
                         [this] { return manager_.back(session_); });
}

void BrowserController::forward() {
  if (!future_.valid() && !takeover_)
    future_ = std::async(std::launch::async,
                         [this] { return manager_.forward(session_); });
}

void BrowserController::reload() {
  if (!future_.valid() && !takeover_)
    future_ = std::async(std::launch::async,
                         [this] { return manager_.reload(session_); });
}

void BrowserController::toggle_takeover() {
  takeover_ = !takeover_;
  auto& view = view_model_.state();
  view.browser_takeover = takeover_;
  view.browser_permission = takeover_
      ? "用户已接管外部浏览器 · Agent 网页操作已暂停"
      : "独立 Tokmon Profile · 仅显式操作网页";
  view.browser_takeover_label = takeover_ ? "恢复 Agent" : "用户接管";
  view_model_.dirty();
}

void BrowserController::stop() {
  if (future_.valid())
    return;
  future_ = std::async(std::launch::async, [this] {
    BrowserSessionState state;
    state.session = session_;
    std::string error;
    if (!manager_.close(session_, error))
      state.error = std::move(error);
    return state;
  });
}

void BrowserController::click() {
  auto* selector = control(document_, "browser-selector");
  if (future_.valid() || takeover_ || !selector || selector->GetValue().empty())
    return;
  const std::string target = selector->GetValue();
  future_ = std::async(std::launch::async, [this, target] {
    BrowserSessionState state;
    state.session = session_;
    std::string error;
    if (!manager_.click(session_, target, error)) {
      state.error = std::move(error);
      return state;
    }
    return manager_.refresh(session_);
  });
}

void BrowserController::fill() {
  auto* selector = control(document_, "browser-selector");
  auto* value = control(document_, "browser-value");
  if (future_.valid() || takeover_ || !selector ||
      selector->GetValue().empty() || !value)
    return;
  const std::string target = selector->GetValue();
  const std::string replacement = value->GetValue();
  future_ = std::async(std::launch::async,
      [this, target, replacement] {
        BrowserSessionState state;
        state.session = session_;
        std::string error;
        if (!manager_.fill(session_, target, replacement, error)) {
          state.error = std::move(error);
          return state;
        }
        return manager_.refresh(session_);
      });
}

void BrowserController::present(const BrowserSessionState& state) {
  auto& view = view_model_.state();
  if (!state.error.empty()) {
    view.browser_title = "Agent Browser 操作失败";
    view.browser_detail = state.error;
    view.browser_running = false;
    view.browser_preview_visible = false;
  } else if (!state.running) {
    view.browser_title = "Agent Browser 已停止";
    view.browser_detail = "会话进程与私有 Profile 已安全关闭";
    view.browser_running = false;
    view.browser_preview_visible = false;
  } else {
    view.browser_title = state.title;
    view.browser_detail = state.url;
    view.browser_url = state.url;
    view.browser_preview = state.preview_image.generic_string();
    view.browser_snapshot = state.accessibility_snapshot;
    view.browser_running = true;
    view.browser_preview_visible = !state.preview_image.empty();
  }
  view_model_.dirty();
}

bool BrowserController::update() {
  if (!future_.valid() ||
      future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return false;
  present(future_.get());
  return true;
}

} // namespace tokmon::desk
