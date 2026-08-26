#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <slint.h>

namespace tokmon::desktop {

std::string display_utf8(std::string_view input);
slint::SharedString display_string(std::string_view input);
void copy_to_clipboard(std::string_view text);
std::string choose_attachment(bool directory);

std::filesystem::path path_from_utf8(std::string_view value);
std::string path_to_utf8(const std::filesystem::path &value);
std::string path_basename_utf8(std::string_view value);
std::optional<std::filesystem::path> normalize_workspace_path(
    std::string_view value,
    const std::optional<std::filesystem::path> &relative_to = std::nullopt);
bool same_workspace(const std::filesystem::path &left,
                    const std::filesystem::path &right);

int default_ui_scale_percent_for_resolution(std::uint32_t width,
                                            std::uint32_t height);
int default_ui_scale_percent_for_primary_display();

void drag_current_process_window();
void update_current_process_window_drag();
void end_current_process_window_drag();
void make_current_process_window_frameless();
void activate_current_process_window();
void set_current_process_window_topmost(bool enabled);

} // namespace tokmon::desktop
