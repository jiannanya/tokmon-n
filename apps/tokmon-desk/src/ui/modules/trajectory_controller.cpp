#include "ui/modules/trajectory_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace tokmon::desk {
namespace {

// Keep the event ledger plus its three-lane overview below the strict 2,500
// node budget even when the inspector and timeline interaction chrome are
// mounted. Earlier pages remain addressable through the explicit history
// control, matching the bounded-window behavior of deepseek-harness.
constexpr std::size_t trajectory_window_rows = 96;

enum class EventKind { system, user, context, message, model, tool, other, error };

struct EventEntry {
  const tokmon::Photon* photon{};
  EventKind kind{EventKind::other};
  std::string tone;
  std::string role;
  std::string detail;
  std::string duration{"-"};
  std::string tokens{"-"};
  int turn{0};
  int request{0};
  bool failed{false};
};

std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string cbor_string(const tokmon::cbor::Value& payload,
                        const std::string_view key) {
  const auto* value = tokmon::cbor::find(payload, key);
  return value && std::holds_alternative<std::string>(value->data)
      ? std::string(value->as_string()) : std::string{};
}

std::int64_t cbor_integer(const tokmon::cbor::Value& payload,
                          const std::string_view key,
                          const std::int64_t fallback = 0) {
  const auto* value = tokmon::cbor::find(payload, key);
  return value && std::holds_alternative<std::int64_t>(value->data)
      ? value->as_integer() : fallback;
}

std::string act_kind(const tokmon::Photon& photon) {
  return cbor_string(photon.payload, "kind");
}

EventKind classify(const tokmon::Photon& photon) {
  const auto kind = lower(photon.kind);
  if (kind.find("failed") != std::string::npos ||
      kind.find("error") != std::string::npos ||
      kind.find("rejected") != std::string::npos)
    return EventKind::error;
  if (kind == "system.prompt") return EventKind::system;
  if (kind == "user.input" || kind == "user.message") return EventKind::user;
  if (kind.find("context") != std::string::npos) return EventKind::context;
  if (kind == "assistant.message" || kind == "model.completed")
    return EventKind::message;
  const auto action = kind.starts_with("act.") ? lower(act_kind(photon)) : "";
  if (kind == "model.tool-call" || kind == "tool.result" ||
      kind.starts_with("fs.") || kind.starts_with("process.") ||
      (kind.starts_with("act.") && action != "model.call"))
    return EventKind::tool;
  if (kind.starts_with("model.") || action == "model.call")
    return EventKind::model;
  return EventKind::other;
}

std::string tone(EventKind kind) {
  switch (kind) {
    case EventKind::system: return "SYSTEM";
    case EventKind::user: return "USER";
    case EventKind::context: return "CONTEXT";
    case EventKind::message: return "ASSISTANT";
    case EventKind::model: return "MODEL";
    case EventKind::tool: return "TOOL";
    case EventKind::error: return "ERROR";
    case EventKind::other: return "LENS";
  }
  return "LENS";
}

std::string role(EventKind kind) {
  switch (kind) {
    case EventKind::system:
    case EventKind::context: return "System";
    case EventKind::user: return "User";
    case EventKind::message: return "Assistant";
    case EventKind::model: return "Model";
    case EventKind::tool: return "Tool";
    case EventKind::error: return "Error";
    case EventKind::other: return "Lens";
  }
  return "Lens";
}

std::string css_kind(EventKind kind) {
  switch (kind) {
    case EventKind::system: return "system";
    case EventKind::user: return "user";
    case EventKind::context: return "context";
    case EventKind::message: return "message";
    case EventKind::model: return "model";
    case EventKind::tool: return "tool";
    case EventKind::error: return "error";
    case EventKind::other: return "other";
  }
  return "other";
}

std::string grouped(std::int64_t value) {
  auto source = std::to_string(value);
  std::string result;
  result.reserve(source.size() + source.size() / 3);
  for (std::size_t index = 0; index < source.size(); ++index) {
    if (index > 0 && (source.size() - index) % 3 == 0)
      result.push_back(',');
    result.push_back(source[index]);
  }
  return result;
}

std::string elapsed_label(std::int64_t milliseconds) {
  if (milliseconds < 1000) return std::to_string(milliseconds) + "ms";
  const auto seconds = milliseconds / 1000;
  const auto remainder = milliseconds % 1000;
  if (seconds < 60) {
    std::ostringstream output;
    output << seconds << '.' << std::setw(3) << std::setfill('0')
           << remainder << 's';
    return output.str();
  }
  return std::to_string(seconds / 60) + "m " +
      std::to_string(seconds % 60) + "s";
}

std::string time_label(std::int64_t elapsed_ms) {
  elapsed_ms = std::max<std::int64_t>(0, elapsed_ms);
  const auto minutes = elapsed_ms / 60000;
  const auto seconds = (elapsed_ms / 1000) % 60;
  const auto millis = elapsed_ms % 1000;
  std::ostringstream output;
  output << std::setw(2) << std::setfill('0') << minutes << ':'
         << std::setw(2) << seconds << '.' << std::setw(3) << millis;
  return output.str();
}

std::string percent(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3)
         << std::clamp(value, 0.0, 100.0) << '%';
  return output.str();
}

std::string bounded(std::string value, const std::size_t maximum) {
  if (value.size() <= maximum) return value;
  value.resize(maximum);
  value += "…";
  return value;
}

std::string result_text(const tokmon::Photon& photon) {
  for (const auto key : {"result", "output", "detail", "text", "message"}) {
    auto value = cbor_string(photon.payload, key);
    if (!value.empty()) return value;
  }
  return tokmon::cbor::diagnostic(photon.payload);
}

std::string escape_json(const std::string_view value) {
  std::string result;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result += static_cast<char>(character); break;
    }
  }
  return result;
}

} // namespace

TrajectoryController::TrajectoryController(DeskViewModel& view_model)
    : view_model_(view_model) {}

bool TrajectoryController::turn_collapsed(const int turn) const {
  if (all_turns_collapsed_)
    return !expanded_turns_.contains(turn);
  return collapsed_turns_.contains(turn);
}

void TrajectoryController::render(const std::vector<tokmon::Photon>& photons,
                                  const std::string_view active_ray,
                                  const std::uint64_t cursor) {
  auto& view = view_model_.state();
  view.trajectory.clear();
  view.trajectory_segments.clear();
  view.trajectory_empty = photons.empty();
  view.trajectory_count = std::to_string(photons.size());
  view.trajectory_ray = active_ray.empty() ? "未绑定" : std::string(active_ray);
  view.trajectory_cursor = std::to_string(cursor);
  view.trajectory_actual_duration = actual_duration_;
  view.trajectory_turns_collapsed = all_turns_collapsed_;
  view.trajectory_calls_collapsed = all_calls_collapsed_;
  view.trajectory_follow_tail = follow_tail_;
  const auto viewport_span = std::max(0.0001, viewport_end_ - viewport_start_);
  view.trajectory_zoomed = viewport_span < 0.999;
  view.trajectory_zoom = [&] {
    std::ostringstream output;
    output << std::fixed << std::setprecision(viewport_span < 0.2 ? 1 : 0)
           << 1.0 / viewport_span << "×";
    return output.str();
  }();
  view.trajectory_timeline_dragging =
      timeline_gesture_ != TimelineGesture::none;
  view.trajectory_selection_visible = timeline_range_.has_value();
  if (timeline_range_) {
    const auto projected_start = std::clamp(
        (timeline_range_->first - viewport_start_) / viewport_span, 0.0, 1.0);
    const auto projected_end = std::clamp(
        (timeline_range_->second - viewport_start_) / viewport_span, 0.0, 1.0);
    view.trajectory_selection_left = percent(projected_start * 100.0);
    view.trajectory_selection_width = percent(
        std::max(0.0, projected_end - projected_start) * 100.0);
  } else {
    view.trajectory_selection_left = "0%";
    view.trajectory_selection_width = "0%";
  }
  view.trajectory_detail_tab = view.trajectory_detail_tab.empty()
      ? "summary" : view.trajectory_detail_tab;

  if (photons.empty()) {
    view.trajectory_duration = "0ms";
    view.trajectory_turns = "0";
    view.trajectory_calls = "0";
    view.trajectory_tokens = "0";
    view.trajectory_match_count = "0";
    view.trajectory_visible_range = "显示 0 条，共 0 条";
    view.trajectory_has_notice = false;
    view.trajectory_filtered_empty = false;
    view.trajectory_detail_visible = false;
    view_model_.dirty();
    return;
  }

  std::vector<EventEntry> all;
  all.reserve(photons.size());
  int current_turn = 0;
  int requests = 0;
  int calls = 0;
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
  for (const auto& photon : photons) {
    const auto classified = classify(photon);
    if (classified == EventKind::user) ++current_turn;
    if (classified == EventKind::message || classified == EventKind::model)
      ++requests;
    if (classified == EventKind::tool) ++calls;
    if (photon.kind == "model.usage") {
      input_tokens += cbor_integer(photon.payload, "input_tokens");
      output_tokens += cbor_integer(photon.payload, "output_tokens");
    }
    EventEntry entry;
    entry.photon = &photon;
    entry.kind = classified;
    entry.tone = tone(classified);
    entry.role = role(classified);
    entry.detail = bounded(tokmon::cbor::diagnostic(photon.payload), 360);
    const auto duration = cbor_integer(photon.payload, "duration_ms", -1);
    if (duration >= 0) entry.duration = elapsed_label(duration);
    if (photon.kind == "model.usage")
      entry.tokens = grouped(cbor_integer(photon.payload, "input_tokens") +
                             cbor_integer(photon.payload, "output_tokens"));
    entry.turn = std::max(1, current_turn);
    entry.request = requests;
    entry.failed = classified == EventKind::error;
    all.push_back(std::move(entry));
  }

  const auto first_time = photons.front().committed_at_ms;
  const auto last_time = photons.back().committed_at_ms;
  const auto duration_ms = std::max<std::int64_t>(0, last_time - first_time);
  view.trajectory_duration = elapsed_label(duration_ms);
  view.trajectory_turns = std::to_string(std::max(1, current_turn));
  view.trajectory_calls = std::to_string(calls);
  view.trajectory_tokens = grouped(input_tokens + output_tokens);

  std::vector<const EventEntry*> filtered;
  filtered.reserve(all.size());
  const auto query = lower(search_);
  for (const auto& entry : all) {
    bool matches_filter = false;
    switch (filter_) {
      case Filter::all: matches_filter = true; break;
      case Filter::user: matches_filter = entry.kind == EventKind::user; break;
      case Filter::context:
        matches_filter = entry.kind == EventKind::context ||
                         entry.kind == EventKind::system;
        break;
      case Filter::assistant:
        matches_filter = entry.kind == EventKind::message ||
                         entry.kind == EventKind::model;
        break;
      case Filter::tool: matches_filter = entry.kind == EventKind::tool; break;
      case Filter::error: matches_filter = entry.failed; break;
    }
    if (!matches_filter) continue;
    auto haystack = lower(entry.photon->kind + " " + entry.photon->schema +
                          " " + entry.detail);
    if (query.empty() || haystack.find(query) != std::string::npos)
      filtered.push_back(&entry);
  }

  view.trajectory_match_count = std::to_string(filtered.size());
  view.trajectory_filtered_empty = filtered.empty();
  if (last_sequence_ != photons.back().sequence) {
    last_sequence_ = photons.back().sequence;
    if (follow_tail_) window_end_ = filtered.size();
  }
  if (follow_tail_ || window_end_ == 0) window_end_ = filtered.size();
  window_end_ = std::min(window_end_, filtered.size());
  const auto begin = window_end_ > trajectory_window_rows
      ? window_end_ - trajectory_window_rows : 0;
  view.trajectory_has_notice = begin > 0;
  view.trajectory_window_notice = begin > 0
      ? "载入更早轨迹（前面还有 " + std::to_string(begin) + " 条）" : "";
  view.trajectory_visible_range = filtered.empty()
      ? "显示 0 条，共 0 条"
      : "显示 " + std::to_string(begin + 1) + "-" +
            std::to_string(window_end_) + " 条，共 " +
            std::to_string(filtered.size()) + " 条";

  const auto timeline_count = window_end_ - begin;
  const auto full_span = std::max<std::int64_t>(1, duration_ms);
  const auto geometry = [&](const std::size_t index) {
    const auto& photon = *filtered[index]->photon;
    const auto local_index = index - begin;
    double left = actual_duration_ && duration_ms > 0
        ? static_cast<double>(photon.committed_at_ms - first_time) /
              static_cast<double>(full_span)
        : static_cast<double>(local_index) /
              static_cast<double>(std::max<std::size_t>(1, timeline_count));
    std::int64_t recorded_duration =
        cbor_integer(photon.payload, "duration_ms", -1);
    if (recorded_duration < 0 && index + 1 < window_end_)
      recorded_duration = std::max<std::int64_t>(
          0, filtered[index + 1]->photon->committed_at_ms -
                 photon.committed_at_ms);
    double width = actual_duration_ && duration_ms > 0
        ? std::max(0.0035,
                   static_cast<double>(std::max<std::int64_t>(
                       1, recorded_duration)) / static_cast<double>(full_span))
        : std::max(0.0035, 0.88 /
              static_cast<double>(std::max<std::size_t>(1, timeline_count)));
    left = std::clamp(left, 0.0, 1.0);
    width = std::min(width, 1.0 - left);
    return std::pair{left, width};
  };

  int previous_turn = -1;
  for (std::size_t index = begin; index < window_end_; ++index) {
    const auto& entry = *filtered[index];
    const auto& photon = *entry.photon;
    const bool turn_start = entry.turn != previous_turn;
    previous_turn = entry.turn;
    const bool collapsed = turn_collapsed(entry.turn);
    const bool selected = selected_sequence_ == photon.sequence;
    const bool hidden_by_turn = collapsed && !turn_start;
    const bool hidden_by_call = all_calls_collapsed_ &&
        entry.kind == EventKind::tool && !selected;
    const auto [timeline_left, timeline_width] = geometry(index);
    const bool outside_range = timeline_range_ &&
        (timeline_left + timeline_width < timeline_range_->first ||
         timeline_left > timeline_range_->second);
    TrajectoryRowView row;
    row.sequence = std::to_string(photon.sequence);
    row.kind = photon.kind;
    row.metadata = photon.schema;
    row.detail = entry.detail;
    row.classes = css_kind(entry.kind) +
        (selected ? " selected" : "") +
        (entry.failed ? " failed" : "") +
        (outside_range ? " outside-range" : "");
    row.time = time_label(photon.committed_at_ms - first_time);
    row.tone = entry.tone;
    row.role = entry.role;
    row.duration = entry.duration;
    row.tokens = entry.tokens;
    row.turn_label = "Turn " + std::to_string(entry.turn);
    row.request_label = entry.request > 0
        ? "Request #" + std::to_string(entry.request) : "";
    row.wrapper_classes = hidden_by_turn || hidden_by_call ? "hidden" : "";
    row.turn_start = turn_start;
    row.row_visible = !collapsed;
    row.selected = selected;
    row.failed = entry.failed;
    view.trajectory.push_back(std::move(row));
  }

  for (std::size_t index = begin; index < window_end_; ++index) {
    const auto& entry = *filtered[index];
    const auto& photon = *entry.photon;
    const auto [full_left, full_width] = geometry(index);
    const auto clipped_left = std::max(full_left, viewport_start_);
    const auto clipped_right = std::min(full_left + full_width, viewport_end_);
    const bool outside_viewport = clipped_right <= clipped_left;
    const double left = (clipped_left - viewport_start_) / viewport_span * 100.0;
    const double width = outside_viewport ? 0.0
        : (clipped_right - clipped_left) / viewport_span * 100.0;
    int lane = 1;
    if (entry.kind == EventKind::user || entry.kind == EventKind::context ||
        entry.kind == EventKind::system) lane = 0;
    else if (entry.kind == EventKind::tool || entry.kind == EventKind::error)
      lane = 2;
    view.trajectory_segments.push_back({
        std::to_string(photon.sequence),
        "trajectory-span " + css_kind(entry.kind) +
            (selected_sequence_ == photon.sequence ? " selected" : "") +
            (outside_viewport ? " hidden" : ""),
        percent(left), percent(width),
        std::to_string(7 + lane * 14) + "dp"});
  }

  view.trajectory_detail_visible = false;
  view.trajectory_detail_failed = false;
  if (selected_sequence_) {
    const auto selected = std::ranges::find_if(all, [&](const EventEntry& item) {
      return item.photon->sequence == *selected_sequence_;
    });
    if (selected != all.end()) {
      const auto& photon = *selected->photon;
      view.trajectory_detail_visible = true;
      view.trajectory_detail_failed = selected->failed;
      view.trajectory_detail_title = selected->tone + " · #" +
          std::to_string(photon.sequence);
      view.trajectory_detail_location = "Turn " +
          std::to_string(selected->turn) + " · " + photon.kind;
      view.trajectory_detail_status = selected->failed ? "FAILED" : "COMPLETED";
      view.trajectory_detail_summary =
          "Role  " + selected->role + "\nRay  " + std::string(photon.ray) +
          "\nParent  " + (photon.parent ? std::string(*photon.parent) : "-") +
          "\nCause  " + (photon.caused_by_act.empty() ? "-" : photon.caused_by_act);
      view.trajectory_detail_payload = tokmon::cbor::diagnostic(photon.payload);
      view.trajectory_detail_result = result_text(photon);
      view.trajectory_detail_schema = photon.schema.empty() ? "-" : photon.schema;
      view.trajectory_detail_timing =
          "Offset  " + time_label(photon.committed_at_ms - first_time) +
          "\nCommitted  " + std::to_string(photon.committed_at_ms) +
          "\nDuration  " + selected->duration +
          "\nTokens  " + selected->tokens;
    }
  }
  view_model_.dirty();
}

void TrajectoryController::set_search(std::string query) {
  search_ = std::move(query);
  follow_tail_ = true;
  window_end_ = 0;
  timeline_range_.reset();
}

void TrajectoryController::cycle_filter() {
  filter_ = static_cast<Filter>((static_cast<int>(filter_) + 1) % 6);
  static constexpr std::string_view labels[] = {
      "全部", "USER", "CONTEXT", "ASSISTANT", "TOOL", "ERROR"};
  view_model_.state().trajectory_filter = labels[static_cast<int>(filter_)];
  follow_tail_ = true;
  window_end_ = 0;
  timeline_range_.reset();
}

void TrajectoryController::toggle_actual_duration() {
  actual_duration_ = !actual_duration_;
  reset_timeline_navigation();
}

void TrajectoryController::toggle_all_turns() {
  all_turns_collapsed_ = !all_turns_collapsed_;
  collapsed_turns_.clear();
  expanded_turns_.clear();
}

void TrajectoryController::toggle_all_calls() {
  all_calls_collapsed_ = !all_calls_collapsed_;
}

void TrajectoryController::toggle_turn(const int turn) {
  if (all_turns_collapsed_) {
    if (!expanded_turns_.erase(turn)) expanded_turns_.insert(turn);
  } else {
    if (!collapsed_turns_.erase(turn)) collapsed_turns_.insert(turn);
  }
}

void TrajectoryController::select(const std::uint64_t sequence) {
  selected_sequence_ = sequence;
}

void TrajectoryController::close_details() {
  selected_sequence_.reset();
  view_model_.state().trajectory_detail_visible = false;
}

void TrajectoryController::select_detail_tab(std::string tab) {
  if (tab == "summary" || tab == "payload" || tab == "result" ||
      tab == "schema" || tab == "timing")
    view_model_.state().trajectory_detail_tab = std::move(tab);
}

void TrajectoryController::load_earlier() {
  if (window_end_ > trajectory_window_rows)
    window_end_ -= trajectory_window_rows;
  follow_tail_ = false;
}

void TrajectoryController::follow_latest() {
  follow_tail_ = true;
  window_end_ = 0;
}

double TrajectoryController::domain_point(const double fraction) const {
  const auto clamped = std::clamp(fraction, 0.0, 1.0);
  return viewport_start_ + clamped * (viewport_end_ - viewport_start_);
}

void TrajectoryController::reset_timeline_navigation() {
  viewport_start_ = 0.0;
  viewport_end_ = 1.0;
  timeline_range_.reset();
  timeline_gesture_ = TimelineGesture::none;
}

void TrajectoryController::begin_timeline_gesture(const double fraction,
                                                  const bool pan) {
  gesture_anchor_fraction_ = std::clamp(fraction, 0.0, 1.0);
  gesture_anchor_time_ = domain_point(gesture_anchor_fraction_);
  gesture_viewport_start_ = viewport_start_;
  timeline_gesture_ = pan ? TimelineGesture::pan : TimelineGesture::range;
  if (!pan)
    timeline_range_ = std::pair{gesture_anchor_time_, gesture_anchor_time_};
}

void TrajectoryController::update_timeline_gesture(const double fraction) {
  if (timeline_gesture_ == TimelineGesture::none) return;
  const auto current = std::clamp(fraction, 0.0, 1.0);
  if (timeline_gesture_ == TimelineGesture::range) {
    const auto point = domain_point(current);
    timeline_range_ = std::minmax(gesture_anchor_time_, point);
    return;
  }
  const auto viewport_span = viewport_end_ - viewport_start_;
  if (viewport_span >= 0.999) return;
  const auto maximum_start = 1.0 - viewport_span;
  const auto next_start = std::clamp(
      gesture_viewport_start_ -
          (current - gesture_anchor_fraction_) * viewport_span,
      0.0, maximum_start);
  viewport_start_ = next_start;
  viewport_end_ = next_start + viewport_span;
}

void TrajectoryController::end_timeline_gesture(const double fraction) {
  if (timeline_gesture_ == TimelineGesture::none) return;
  const auto gesture = timeline_gesture_;
  update_timeline_gesture(fraction);
  const auto travel = std::abs(std::clamp(fraction, 0.0, 1.0) -
                               gesture_anchor_fraction_);
  timeline_gesture_ = TimelineGesture::none;
  if (gesture == TimelineGesture::pan) {
    if (travel < 0.004) timeline_range_.reset();
    return;
  }
  if (!timeline_range_) return;
  const auto minimum = std::min(0.02, viewport_end_ - viewport_start_);
  if (timeline_range_->second - timeline_range_->first < minimum) {
    const auto center = timeline_range_->first;
    const auto start = std::clamp(center - minimum * 0.5, 0.0, 1.0 - minimum);
    timeline_range_ = std::pair{start, start + minimum};
  }
}

void TrajectoryController::zoom_timeline(const double anchor_fraction,
                                         const double wheel_delta) {
  const auto fraction = std::clamp(anchor_fraction, 0.0, 1.0);
  const auto old_span = viewport_end_ - viewport_start_;
  const auto new_span = std::clamp(
      old_span * std::exp(wheel_delta * 0.24), 0.05, 1.0);
  if (new_span > 0.995) {
    viewport_start_ = 0.0;
    viewport_end_ = 1.0;
    return;
  }
  const auto anchor = domain_point(fraction);
  const auto start = std::clamp(anchor - fraction * new_span,
                                0.0, 1.0 - new_span);
  viewport_start_ = start;
  viewport_end_ = start + new_span;
}

void TrajectoryController::clear_timeline_focus() {
  reset_timeline_navigation();
}

bool TrajectoryController::export_json(
    const std::filesystem::path& directory,
    const std::vector<tokmon::Photon>& photons,
    const std::string_view active_ray, const std::uint64_t cursor,
    std::filesystem::path& output_path, std::string& error) const {
  if (photons.empty()) {
    error = "当前没有可导出的轨迹";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    error = filesystem_error.message();
    return false;
  }
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  output_path = directory /
      ("tokmon-trace-" + std::to_string(milliseconds) + ".json");
  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "无法创建文件";
    return false;
  }
  output << "{\n  \"ray\": \"" << escape_json(active_ray)
         << "\",\n  \"snowCursor\": " << cursor << ",\n  \"events\": [\n";
  for (std::size_t index = 0; index < photons.size(); ++index) {
    const auto& photon = photons[index];
    output << "    {\"sequence\": " << photon.sequence
           << ", \"id\": \"" << escape_json(photon.id)
           << "\", \"kind\": \"" << escape_json(photon.kind)
           << "\", \"schema\": \"" << escape_json(photon.schema)
           << "\", \"committedAtMs\": " << photon.committed_at_ms
           << ", \"payload\": \""
           << escape_json(tokmon::cbor::diagnostic(photon.payload)) << "\"}"
           << (index + 1 == photons.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  if (!output) {
    error = "写入未完成";
    return false;
  }
  return true;
}

} // namespace tokmon::desk
