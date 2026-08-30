#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace tokmon::desk {

struct WorkspaceEntry {
  std::filesystem::path absolute_path;
  std::string relative_path;
  std::string name;
  std::size_t depth{0};
  bool directory{false};
  bool expanded{false};
};

struct WorkspaceSearchResult {
  std::string relative_path;
  std::size_t line{0};
  std::size_t column{0};
  std::string preview;
};

enum class WorkspaceChangeKind { created, modified, removed };
enum class WorkspaceChangeOrigin { external, self };

struct WorkspaceChange {
  WorkspaceChangeKind kind{WorkspaceChangeKind::modified};
  std::filesystem::path path;
  WorkspaceChangeOrigin origin{WorkspaceChangeOrigin::external};
};

class WorkspaceWatcher final {
public:
  explicit WorkspaceWatcher(std::filesystem::path root = {});
  ~WorkspaceWatcher();
  WorkspaceWatcher(const WorkspaceWatcher&) = delete;
  WorkspaceWatcher& operator=(const WorkspaceWatcher&) = delete;

  void reset(std::filesystem::path root);
  void acknowledge_self_write(const std::filesystem::path& path);
  [[nodiscard]] std::vector<WorkspaceChange> take_changes();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class WorkspaceService final {
public:
  explicit WorkspaceService(std::filesystem::path root = {});
  [[nodiscard]] bool set_root(std::filesystem::path root, std::string& error);
  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] std::vector<WorkspaceEntry> enumerate(
      std::size_t max_entries = 4000, std::size_t max_depth = 12) const;
  [[nodiscard]] std::future<std::vector<WorkspaceEntry>> enumerate_async(
      std::size_t max_entries = 4000, std::size_t max_depth = 12) const;
  [[nodiscard]] std::vector<WorkspaceEntry> children(
      const std::filesystem::path& relative_directory,
      std::size_t max_entries = 1000) const;
  [[nodiscard]] std::vector<WorkspaceSearchResult> search(
      std::string query, std::size_t max_results = 200,
      const std::atomic_bool* cancelled = nullptr) const;
  [[nodiscard]] std::future<std::vector<WorkspaceSearchResult>> search_async(
      std::string query, std::size_t max_results = 200) const;
  [[nodiscard]] bool contains(const std::filesystem::path& path) const;
  [[nodiscard]] std::string read_text(const std::filesystem::path& path,
                                      std::size_t max_bytes,
                                      std::string& error) const;
  [[nodiscard]] bool create_file(const std::filesystem::path& relative,
                                 std::string_view initial_text,
                                 std::string& error) const;
  [[nodiscard]] bool create_directory(const std::filesystem::path& relative,
                                      std::string& error) const;
  [[nodiscard]] bool rename_entry(const std::filesystem::path& relative,
                                  const std::filesystem::path& new_name,
                                  std::string& error) const;
  [[nodiscard]] bool remove_entry(const std::filesystem::path& relative,
                                  bool recursive, std::string& error) const;

private:
  std::filesystem::path root_;
  [[nodiscard]] bool resolve_mutation_path(
      const std::filesystem::path& relative, bool may_not_exist,
      std::filesystem::path& absolute, std::string& error) const;
};

} // namespace tokmon::desk
