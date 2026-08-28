#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tokmon.h"

namespace tokmon::desktop {

// Everything the right side panel needs: Git review data (branch, changed
// files, per-file diffs) and the workspace directory tree used by the
// "open file" tab. Both are read-only views over the current workspace; the
// mutating Git verbs are the only operations that touch the repository.
class ReviewPanel {
public:
  void set_workspace(std::filesystem::path workspace);
  const std::filesystem::path &workspace() const noexcept { return workspace_; }

  // Drops every cached diff and re-reads the branch plus changed files.
  void refresh();

  bool available() const noexcept { return available_; }
  std::string branch() const { return branch_; }
  std::vector<std::string> branches() const { return branches_; }

  std::vector<ReviewFile> review_files() const { return files_; }
  int total_added() const;
  int total_removed() const;

  // Diff rows for one changed file, honouring the expanded-hunk set. The
  // result is cached until the workspace refreshes or a hunk toggles.
  const std::vector<DiffLine> &diff_lines(const std::string &file_id);
  void toggle_hunk(const std::string &file_id, const std::string &hunk_id);

  bool stage_all();
  bool discard_unstaged();
  bool commit(const std::string &message);
  bool push();
  bool checkout(const std::string &branch);
  std::string last_error() const { return last_error_; }

  // Workspace directory tree for the "open file" tab.
  void reset_tree();
  std::vector<TreeItem> tree_items() const { return tree_; }
  // Tree rows are addressed by path, never by the filtered UI index, so a
  // rename or a filter change cannot make a click expand the wrong folder.
  void toggle_tree_folder(const std::string &path);
  bool read_tree_file(const std::string &path, std::string *name,
                      std::string *content) const;

private:
  struct DiffRow {
    char kind{' '}; // ' ', '+', '-'
    int old_num{0};
    int new_num{0};
    std::string text;
  };

  struct Hunk {
    std::string id;
    std::string header;
    std::vector<DiffRow> rows;
  };

  struct FileDiff {
    std::vector<Hunk> hunks;
    std::set<std::string> expanded;
    bool parsed{false};
    std::vector<DiffLine> projection;
  };

  struct TreeEntry {
    std::string name;
    std::string path;
    bool folder{false};
    int depth{0};
    bool expanded{false};
    bool loaded{false};
  };

  void reload_status();
  void reload_branches();
  void parse_file_diff(const std::string &file_id);
  void project_file_diff(const std::string &file_id);
  void rebuild_tree();

  std::filesystem::path workspace_;
  bool available_{false};
  std::string branch_;
  std::vector<std::string> branches_;
  std::vector<ReviewFile> files_;
  std::map<std::string, FileDiff> diffs_;
  std::string last_error_;

  std::vector<TreeEntry> tree_entries_;
  std::vector<TreeItem> tree_;
};

// Opens the system file manager with `path` selected. Returns false when the
// platform call fails.
bool reveal_path_in_file_manager(const std::filesystem::path &path);

} // namespace tokmon::desktop
