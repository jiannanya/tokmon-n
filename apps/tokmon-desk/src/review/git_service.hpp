#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tokmon::desk {

struct GitFileStatus {
  std::string path;
  char index_status{' '};
  char worktree_status{' '};
};

struct GitSnapshot {
  bool repository{false};
  std::string branch;
  std::vector<GitFileStatus> files;
  std::string error;
};

struct GitDiffLine {
  char origin{' '};
  int old_line{-1};
  int new_line{-1};
  std::string content;
};

struct GitDiffHunk {
  std::size_t index{0};
  int old_start{0};
  int old_lines{0};
  int new_start{0};
  int new_lines{0};
  std::string header;
  std::vector<GitDiffLine> lines;
};

struct GitFileDiff {
  std::string path;
  bool staged{false};
  bool binary{false};
  bool created{false};
  bool deleted{false};
  bool renamed{false};
  std::vector<GitDiffHunk> hunks;
  std::string patch;
};

class GitService final {
 public:
  explicit GitService(std::filesystem::path workspace = {});
  void set_workspace(std::filesystem::path workspace);
  [[nodiscard]] GitSnapshot status() const;
  [[nodiscard]] std::vector<std::string> branches(std::string& error) const;
  [[nodiscard]] bool checkout_branch(const std::string& branch,
                                     std::string& error) const;
  [[nodiscard]] std::string diff(const std::string& path, bool staged,
                                 std::string& error) const;
  [[nodiscard]] std::optional<GitFileDiff> diff_model(
      const std::string& path, bool staged, std::string& error) const;
  [[nodiscard]] bool stage_file(const std::string& path,
                                std::string& error) const;
  [[nodiscard]] bool unstage_file(const std::string& path,
                                  std::string& error) const;
  [[nodiscard]] bool stage_hunk(const std::string& path,
                                std::size_t hunk_index,
                                std::string& error) const;
  [[nodiscard]] bool unstage_hunk(const std::string& path,
                                  std::size_t hunk_index,
                                  std::string& error) const;
  [[nodiscard]] bool discard_hunk(const std::string& path,
                                  std::size_t hunk_index,
                                  std::uint64_t expected_hash,
                                  std::string& error) const;
  [[nodiscard]] bool discard_file(const std::string& path,
                                  std::uint64_t expected_hash,
                                  bool allow_delete_untracked,
                                  std::string& error) const;
  [[nodiscard]] bool stage_all(std::string& error) const;
  [[nodiscard]] bool unstage_all(std::string& error) const;
  [[nodiscard]] bool commit(std::string message, std::string& error) const;
  [[nodiscard]] bool push(std::string& error) const;

 private:
  std::filesystem::path workspace_;
  [[nodiscard]] bool resolve_path(const std::string& relative,
                                  std::filesystem::path& absolute,
                                  std::string& error) const;
  [[nodiscard]] bool run(const std::vector<std::string>& argv, std::string& output,
                         std::string& error) const;
};

} // namespace tokmon::desk
