#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
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
  bool cooperative_stop_attempted{false};
  bool forced_tree_termination{false};
  std::string timeout_reason;
  std::string sandbox_strength;
};

struct ProcessRequest {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds first_output_timeout{0};
  std::chrono::milliseconds idle_output_timeout{0};
  std::size_t max_output_bytes{256u * 1024u};
  std::string stdin_text;
  std::map<std::string, std::string, std::less<>> environment;
  bool inherit_environment{true};
  std::size_t max_memory_bytes{0};
  std::chrono::milliseconds max_cpu_time{0};
  std::size_t max_processes{0};
  std::function<void(std::string_view)> on_stdout;
  std::function<void(std::string_view)> on_stderr;
  std::stop_token stop;

  ~ProcessRequest() {
    std::fill(stdin_text.begin(), stdin_text.end(), '\0');
    for (auto& [_, value] : environment) std::fill(value.begin(), value.end(), '\0');
  }
};

[[nodiscard]] Result<ProcessOutput> run_process(ProcessRequest request);

[[nodiscard]] Result<ProcessOutput> run_process(
    const std::vector<std::string>& argv, const std::filesystem::path& cwd,
    std::chrono::milliseconds timeout, std::size_t max_output_bytes,
    std::stop_token stop = {});

}  // namespace tokmon::builtin
