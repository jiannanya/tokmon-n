#pragma once

#include <filesystem>
#include <string_view>

#include "tokmon/error.hpp"

namespace tokmon {

Result<void> initialize_logging(const std::filesystem::path& log_directory,
                                std::string_view process_name,
                                std::string_view level = "info");
[[nodiscard]] std::string redact(std::string_view message);

}  // namespace tokmon

