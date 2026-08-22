#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon::builtin {

struct PtyOptions {
  std::vector<std::string> argv;
  std::filesystem::path cwd;
  std::map<std::string, std::string, std::less<>> environment;
  std::uint16_t columns{80};
  std::uint16_t rows{24};
  std::size_t max_output_bytes{256u * 1024u};
  std::size_t max_memory_bytes{0};
  std::size_t max_processes{0};
  std::chrono::milliseconds max_cpu_time{0};
  std::chrono::milliseconds idle_timeout{300'000};
};

struct PtySnapshot {
  std::string output;
  bool truncated{false};
  bool running{false};
  int exit_code{-1};
  std::string sandbox_strength;
};

class PtySession final {
 public:
  ~PtySession();
  PtySession(const PtySession&) = delete;
  PtySession& operator=(const PtySession&) = delete;

  [[nodiscard]] static Result<std::shared_ptr<PtySession>> open(PtyOptions options);
  [[nodiscard]] Result<void> write(std::string_view input);
  [[nodiscard]] Result<void> resize(std::uint16_t columns, std::uint16_t rows);
  [[nodiscard]] PtySnapshot take_output();
  [[nodiscard]] Result<PtySnapshot> close(std::chrono::milliseconds grace);

 private:
  struct Impl;
  explicit PtySession(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon::builtin
