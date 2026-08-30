#pragma once

#include "tokmon/photon.hpp"
#include "ui/desk_view_model.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tokmon::desk {

// Owns every piece of trajectory presentation state. The shell controller
// only forwards UI intents and supplies the current immutable Photon window.
class TrajectoryController {
public:
  explicit TrajectoryController(DeskViewModel& view_model);

  void render(const std::vector<tokmon::Photon>& photons,
              std::string_view active_ray, std::uint64_t cursor);
  void set_search(std::string query);
  void cycle_filter();
  void toggle_actual_duration();
  void toggle_all_turns();
  void toggle_all_calls();
  void toggle_turn(int turn);
  void select(std::uint64_t sequence);
  void close_details();
  void select_detail_tab(std::string tab);
  void load_earlier();
  void follow_latest();
  void begin_timeline_gesture(double fraction, bool pan);
  void update_timeline_gesture(double fraction);
  void end_timeline_gesture(double fraction);
  void zoom_timeline(double anchor_fraction, double wheel_delta);
  void clear_timeline_focus();
  [[nodiscard]] bool timeline_gesture_active() const {
    return timeline_gesture_ != TimelineGesture::none;
  }

  [[nodiscard]] bool export_json(
      const std::filesystem::path& directory,
      const std::vector<tokmon::Photon>& photons,
      std::string_view active_ray, std::uint64_t cursor,
      std::filesystem::path& output_path, std::string& error) const;

private:
  enum class Filter { all, user, context, assistant, tool, error };

  [[nodiscard]] bool turn_collapsed(int turn) const;
  [[nodiscard]] double domain_point(double fraction) const;
  void reset_timeline_navigation();

  enum class TimelineGesture { none, range, pan };

  DeskViewModel& view_model_;
  std::string search_;
  Filter filter_{Filter::all};
  std::optional<std::uint64_t> selected_sequence_;
  std::set<int> collapsed_turns_;
  std::set<int> expanded_turns_;
  std::size_t window_end_{0};
  std::uint64_t last_sequence_{0};
  bool actual_duration_{true};
  bool all_turns_collapsed_{false};
  bool all_calls_collapsed_{false};
  bool follow_tail_{true};
  double viewport_start_{0.0};
  double viewport_end_{1.0};
  std::optional<std::pair<double, double>> timeline_range_;
  TimelineGesture timeline_gesture_{TimelineGesture::none};
  double gesture_anchor_fraction_{0.0};
  double gesture_anchor_time_{0.0};
  double gesture_viewport_start_{0.0};
};

} // namespace tokmon::desk
