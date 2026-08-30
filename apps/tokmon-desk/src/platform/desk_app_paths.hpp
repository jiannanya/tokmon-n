#pragma once

#include <filesystem>
#include <chrono>
#include <cstdint>
#include <string>

namespace tokmon::desk {

struct DeskRetentionPolicy {
  std::uintmax_t cache_bytes{512u * 1024u * 1024u};
  std::uintmax_t log_bytes{128u * 1024u * 1024u};
  std::uintmax_t recovery_bytes{256u * 1024u * 1024u};
  std::chrono::hours log_age{24 * 30};
  std::chrono::hours recovery_age{24 * 14};
};

struct DeskRetentionReport {
  std::uintmax_t removed_bytes{0};
  std::size_t removed_files{0};
};

struct DeskAppPaths {
  std::filesystem::path config;
  std::filesystem::path data;
  std::filesystem::path state;
  std::filesystem::path cache;
  std::filesystem::path logs;
  std::filesystem::path runtime;
  std::filesystem::path recovery;
  std::filesystem::path change_snapshots;

  [[nodiscard]] static DeskAppPaths resolve(
      const std::filesystem::path& override_root = {});
  [[nodiscard]] bool ensure(std::string& error) const;
  [[nodiscard]] bool enforce_retention(const DeskRetentionPolicy& policy,
                                       DeskRetentionReport& report,
                                       std::string& error) const;
  [[nodiscard]] bool isolated_from(const std::filesystem::path& workspace) const;
};

} // namespace tokmon::desk
