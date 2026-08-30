#pragma once

#include "review/git_service.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tokmon::desk {

struct DesktopChange {
  std::string path;
  bool existed_before{false};
  bool exists_after{false};
  bool reversible{false};
  std::string before_sha256;
  std::string after_sha256;
  std::string preimage_blob;
};

struct DesktopChangeSet {
  std::string id;
  std::string run_id;
  std::string baseline_head;
  std::vector<DesktopChange> changes;
  bool accepted{false};
  bool rejected{false};
};

class DesktopChangeTracker final {
 public:
  DesktopChangeTracker(std::filesystem::path workspace = {},
                       std::filesystem::path snapshot_root = {});

  void set_workspace(std::filesystem::path workspace);
  [[nodiscard]] bool begin(std::string run_id, std::string& error);
  [[nodiscard]] std::optional<DesktopChangeSet> finish(std::string& error);
  [[nodiscard]] bool accept(DesktopChangeSet& changes, std::string& error) const;
  [[nodiscard]] bool reject(DesktopChangeSet& changes, std::string& error) const;
  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  struct BaselineEntry {
    bool existed{false};
    std::string sha256;
    std::string blob;
  };

  [[nodiscard]] std::optional<std::string> read_workspace_file(
      const std::string& path, std::string& error) const;
  [[nodiscard]] std::string store_blob(std::string_view content,
                                       std::string& error) const;
  [[nodiscard]] std::optional<std::string> load_blob(
      std::string_view digest, std::string& error) const;
  [[nodiscard]] bool write_workspace_file(const std::string& path,
                                          std::string_view content,
                                          std::string& error) const;

  std::filesystem::path workspace_;
  std::filesystem::path snapshot_root_;
  GitService git_;
  bool active_{false};
  std::string run_id_;
  std::string baseline_head_;
  std::unordered_map<std::string, BaselineEntry> baseline_;
};

} // namespace tokmon::desk
