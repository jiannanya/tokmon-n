#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon::builtin {

struct ProcessOutput {
  int exit_code{-1};
  std::string stdout_text;
  std::string stderr_text;
  bool stdout_truncated{false};
  bool stderr_truncated{false};
  bool timed_out{false};
  bool cancelled{false};
  std::string sandbox_strength;
};

[[nodiscard]] Result<ProcessOutput> run_process(
    const std::vector<std::string>& argv, const std::filesystem::path& cwd,
    std::chrono::milliseconds timeout, std::size_t max_output_bytes,
    std::stop_token stop = {});

}  // namespace tokmon::builtin
