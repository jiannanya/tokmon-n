#pragma once

#include "editor/document_store.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace tokmon::desk {

struct DocumentRecoveryEntry {
  std::filesystem::path path;
  std::filesystem::path workspace;
  std::uint64_t disk_hash{0};
  std::uint64_t version{0};
  std::string text;
};

// Desktop-private crash recovery. Recovery files are content-addressed by the
// canonical document path and never touch the workspace or its .tokmon data.
class DocumentRecoveryStore final {
 public:
  explicit DocumentRecoveryStore(std::filesystem::path root = {});

  [[nodiscard]] bool save(const DocumentSnapshot& snapshot,
                          const std::filesystem::path& workspace,
                          std::string& error) const;
  [[nodiscard]] std::optional<DocumentRecoveryEntry> load(
      const std::filesystem::path& path,
      const std::filesystem::path& workspace,
      std::string& error) const;
  [[nodiscard]] bool clear(const std::filesystem::path& path,
                           std::string& error) const;

 private:
  [[nodiscard]] std::filesystem::path recovery_path(
      const std::filesystem::path& path) const;
  std::filesystem::path root_;
};

} // namespace tokmon::desk
