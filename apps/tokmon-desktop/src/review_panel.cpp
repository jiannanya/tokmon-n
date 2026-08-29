#include "review_panel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h defines min/max macros that break std::min/std::max in this file.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "platform_utils.hpp"

namespace tokmon::desktop {
namespace {

constexpr int kContextLines = 3;
constexpr std::size_t kMaxDiffRows = 4'000;
constexpr std::size_t kMaxFileBytes = 512 * 1'024;
constexpr int kMaxTreeDepth = 6;
constexpr std::size_t kMaxTreeEntries = 3'000;

#ifdef _WIN32
std::wstring to_wide(std::string_view value) {
  if (value.empty())
    return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0)
    return {};
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

struct ProcessOutput {
  bool launched{false};
  int exit_code{0};
  std::string text;
};

// Runs `command_line` with a hidden window and captures stdout. Standard
// CreateProcess pipe plumbing: the parent keeps the read end, the child
// inherits the write end as both stdout and stderr.
ProcessOutput run_process(const std::filesystem::path &working_dir,
                          const std::string &command_line) {
  ProcessOutput result;
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!CreatePipe(&read_handle, &write_handle, &attributes, 0))
    return result;
  // The parent's read end must not be inherited or the pipe never closes.
  SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  startup.wShowWindow = SW_HIDE;
  startup.hStdOutput = write_handle;
  startup.hStdError = write_handle;
  startup.hStdInput = nullptr;

  PROCESS_INFORMATION process{};
  auto wide_command = to_wide(command_line);
  auto wide_directory = to_wide(path_to_utf8(working_dir));
  const bool launched =
      CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr,
                     wide_directory.empty() ? nullptr : wide_directory.c_str(),
                     &startup, &process);
  CloseHandle(write_handle);
  if (!launched) {
    CloseHandle(read_handle);
    return result;
  }
  result.launched = true;

  std::string buffer;
  std::array<char, 8192> chunk{};
  DWORD read = 0;
  while (ReadFile(read_handle, chunk.data(), static_cast<DWORD>(chunk.size()),
                  &read, nullptr) &&
         read > 0)
    buffer.append(chunk.data(), static_cast<std::size_t>(read));

  // The pipe closed, so the child has finished writing; a bounded wait keeps
  // the UI thread from blocking on a wedged git process.
  if (WaitForSingleObject(process.hProcess, 5'000) == WAIT_TIMEOUT)
    TerminateProcess(process.hProcess, 1);
  DWORD code = 0;
  GetExitCodeProcess(process.hProcess, &code);
  result.exit_code = static_cast<int>(code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(read_handle);
  result.text = std::move(buffer);
  return result;
}
#else
struct ProcessOutput {
  bool launched{false};
  int exit_code{0};
  std::string text;
};

ProcessOutput run_process(const std::filesystem::path &working_dir,
                          const std::string &command_line) {
  ProcessOutput result;
  const auto full = "cd " + quote_argument(path_to_utf8(working_dir)) +
                    " && " + command_line + " 2>/dev/null";
  FILE *pipe = popen(full.c_str(), "r");
  if (pipe == nullptr)
    return result;
  result.launched = true;
  std::string buffer;
  std::array<char, 8192> chunk{};
  std::size_t count = 0;
  while ((count = std::fread(chunk.data(), 1, chunk.size(), pipe)) > 0)
    buffer.append(chunk.data(), count);
  result.exit_code = pclose(pipe);
  result.text = std::move(buffer);
  return result;
}
#endif

std::string quote_argument(std::string_view value) {
  std::string out = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\')
      out.push_back('\\');
    out.push_back(character);
  }
  out.push_back('"');
  return out;
}

std::vector<std::string> split_lines(const std::string &text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto newline = text.find('\n', start);
    if (newline == std::string::npos) {
      if (start < text.size())
        lines.push_back(text.substr(start));
      break;
    }
    auto line = text.substr(start, newline - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    start = newline + 1;
  }
  return lines;
}

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    --end;
  return std::string(value.substr(begin, end - begin));
}

// `git status --porcelain` quotes paths containing spaces or non-ASCII bytes.
std::string unquote_status_path(std::string_view value) {
  if (value.size() < 2 || value.front() != '"')
    return std::string(value);
  std::string out;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '"')
      break;
    if (character == '\\' && index + 1 < value.size()) {
      ++index;
      const char escaped = value[index];
      switch (escaped) {
      case 'n':
        out.push_back('\n');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'r':
        out.push_back('\r');
        break;
      default:
        out.push_back(escaped);
        break;
      }
      continue;
    }
    out.push_back(character);
  }
  return out;
}

std::string folder_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return ".";
  return path.substr(0, slash);
}

std::string name_of(const std::string &path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void ReviewPanel::set_workspace(std::filesystem::path workspace) {
  // Git's porcelain output is rooted at the repository, even when the process
  // starts in a nested build/output directory. Pathspecs, on the other hand,
  // are interpreted relative to the process working directory. Keeping the
  // nested directory here therefore produces a valid changed-file list whose
  // subsequent `git diff -- <repo-relative path>` is always empty. Resolve the
  // repository root once and use it consistently for status, diffs, the tree,
  // and Explorer reveal operations.
  std::error_code error;
  auto normalized = std::filesystem::absolute(workspace, error);
  if (error)
    normalized = std::move(workspace);
  else
    normalized = normalized.lexically_normal();

  const auto root = run_process(normalized, "git rev-parse --show-toplevel");
  if (root.launched && root.exit_code == 0) {
    const auto root_text = trim(root.text);
    if (!root_text.empty()) {
      const auto candidate = path_from_utf8(root_text);
      if (std::filesystem::is_directory(candidate, error) && !error)
        normalized = candidate.lexically_normal();
    }
  }
  workspace_ = std::move(normalized);
  diffs_.clear();
  files_.clear();
  branches_.clear();
  branch_.clear();
  available_ = false;
  reset_tree();
}

void ReviewPanel::refresh() {
  diffs_.clear();
  files_.clear();
  branches_.clear();
  branch_.clear();
  last_error_.clear();
  available_ = false;
  reload_branches();
  reload_status();
}

void ReviewPanel::reload_branches() {
  if (workspace_.empty())
    return;
  const auto inside =
      run_process(workspace_, "git rev-parse --is-inside-work-tree");
  available_ = inside.launched && inside.exit_code == 0 &&
               trim(inside.text) == "true";
  if (!available_)
    return;
  const auto head = run_process(workspace_, "git rev-parse --abbrev-ref HEAD");
  if (head.launched && head.exit_code == 0)
    branch_ = trim(head.text);
  const auto list = run_process(workspace_, "git branch --format=%(refname:short)");
  if (list.launched && list.exit_code == 0) {
    for (auto &line : split_lines(list.text)) {
      const auto name = trim(line);
      if (!name.empty())
        branches_.push_back(name);
    }
  }
  if (branches_.empty() && !branch_.empty())
    branches_.push_back(branch_);
}

void ReviewPanel::reload_status() {
  if (!available_)
    return;
  const auto status = run_process(
      workspace_, "git status --porcelain=v1 --untracked-files=all");
  if (!status.launched || status.exit_code != 0) {
    last_error_ = trim(status.text);
    return;
  }
  const auto numstat = run_process(workspace_, "git diff HEAD --numstat");
  std::map<std::string, std::pair<int, int>> stats;
  if (numstat.launched && numstat.exit_code == 0) {
    for (const auto &line : split_lines(numstat.text)) {
      const auto first_tab = line.find('\t');
      if (first_tab == std::string::npos)
        continue;
      const auto second_tab = line.find('\t', first_tab + 1);
      if (second_tab == std::string::npos)
        continue;
      int added = 0;
      int removed = 0;
      try {
        added = std::stoi(line.substr(0, first_tab));
      } catch (...) {
        added = 0;
      }
      try {
        removed = std::stoi(line.substr(first_tab + 1, second_tab - first_tab - 1));
      } catch (...) {
        removed = 0;
      }
      stats[unquote_status_path(std::string_view(line).substr(second_tab + 1))] =
          {added, removed};
    }
  }

  for (const auto &line : split_lines(status.text)) {
    if (line.size() < 4)
      continue;
    const auto index_status = line[0];
    const auto worktree_status = line[1];
    auto path = unquote_status_path(std::string_view(line).substr(3));
    // Renames report "old -> new"; review the destination.
    if (const auto arrow = path.find(" -> "); arrow != std::string::npos)
      path = path.substr(arrow + 4);
    if (path.empty())
      continue;
    ReviewFile file;
    file.id = slint::SharedString(path);
    file.path = slint::SharedString(path);
    file.display_folder = slint::SharedString(folder_of(path));
    file.display_name = slint::SharedString(name_of(path));
    file.show_folder = true;
    const bool untracked = index_status == '?' && worktree_status == '?';
    const bool deleted = index_status == 'D' || worktree_status == 'D';
    const bool renamed = index_status == 'R' || worktree_status == 'R';
    file.status = slint::SharedString(
        untracked || index_status == 'A' || worktree_status == 'A'
            ? "added"
            : deleted ? "deleted" : renamed ? "renamed" : "modified");
    if (auto found = stats.find(path); found != stats.end()) {
      file.additions = found->second.first;
      file.deletions = found->second.second;
    } else if (untracked) {
      std::error_code error;
      const auto absolute = workspace_ / path;
      if (std::filesystem::is_regular_file(absolute, error)) {
        std::ifstream stream(absolute, std::ios::binary);
        std::string content;
        int lines = 0;
        while (std::getline(stream, content))
          ++lines;
        file.additions = lines;
      }
    }
    files_.push_back(std::move(file));
    if (files_.size() >= 500)
      break;
  }
}

int ReviewPanel::total_added() const {
  int total = 0;
  for (const auto &file : files_)
    total += file.additions;
  return total;
}

int ReviewPanel::total_removed() const {
  int total = 0;
  for (const auto &file : files_)
    total += file.deletions;
  return total;
}

void ReviewPanel::parse_file_diff(const std::string &file_id) {
  auto &diff = diffs_[file_id];
  if (diff.parsed)
    return;
  diff.parsed = true;
  diff.hunks.clear();

  const auto append_untracked = [&]() {
    std::error_code error;
    const auto absolute = workspace_ / file_id;
    if (!std::filesystem::is_regular_file(absolute, error))
      return;
    std::ifstream stream(absolute, std::ios::binary);
    if (!stream)
      return;
    Hunk hunk;
    hunk.id = "0";
    hunk.header = "新文件";
    std::string line;
    int number = 1;
    while (std::getline(stream, line) && hunk.rows.size() < kMaxDiffRows) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      DiffRow row;
      row.kind = '+';
      row.new_num = number++;
      row.text = std::move(line);
      hunk.rows.push_back(std::move(row));
    }
    if (!hunk.rows.empty())
      diff.hunks.push_back(std::move(hunk));
  };

  const auto command =
      "git diff --no-color --no-ext-diff HEAD -- " + quote_argument(file_id);
  const auto output = run_process(workspace_, command);
  if (!output.launched || output.exit_code != 0 || output.text.empty()) {
    // Untracked files have no diff against HEAD; show their content as adds.
    append_untracked();
    return;
  }

  Hunk *current = nullptr;
  for (const auto &raw : split_lines(output.text)) {
    if (raw.starts_with("@@")) {
      // @@ -old_start,old_count +new_start,new_count @@ optional heading
      int old_start = 0;
      int new_start = 0;
      if (std::sscanf(raw.c_str(), "@@ -%d,%*d +%d,%*d", &old_start, &new_start) < 2)
        if (std::sscanf(raw.c_str(), "@@ -%d +%d", &old_start, &new_start) < 2) {
          old_start = 0;
          new_start = 0;
        }
      Hunk hunk;
      hunk.id = std::to_string(diff.hunks.size());
      hunk.header = "@@ -" + std::to_string(old_start) + " +" +
                    std::to_string(new_start) + " @@";
      diff.hunks.push_back(std::move(hunk));
      current = &diff.hunks.back();
      continue;
    }
    if (raw.empty() || raw[0] == 'd' || raw[0] == 'i' || raw[0] == '\\')
      continue;
    if (raw.starts_with("---") || raw.starts_with("+++"))
      continue;
    if (current == nullptr)
      continue;
    DiffRow row;
    const char marker = raw[0];
    if (marker == '-') {
      row.kind = '-';
      row.text = raw.substr(1);
    } else if (marker == '+') {
      row.kind = '+';
      row.text = raw.substr(1);
    } else {
      row.kind = ' ';
      row.text = raw.substr(1);
    }
    if (!current->rows.empty()) {
      const auto &previous = current->rows.back();
      row.old_num = previous.old_num + (previous.kind == '+' ? 0 : 1);
      row.new_num = previous.new_num + (previous.kind == '-' ? 0 : 1);
    } else {
      // First row: recover starts from the hunk header.
      int old_start = 0;
      int new_start = 0;
      if (std::sscanf(current->header.c_str(), "@@ -%d +%d", &old_start,
                      &new_start) < 2) {
        old_start = 0;
        new_start = 0;
      }
      row.old_num = old_start;
      row.new_num = new_start;
    }
    current->rows.push_back(std::move(row));
    if (current->rows.size() >= kMaxDiffRows)
      break;
  }

  // Drop empty hunks produced by header-only sections.
  std::erase_if(diff.hunks, [](const Hunk &hunk) { return hunk.rows.empty(); });
  if (diff.hunks.empty())
    append_untracked();
}

void ReviewPanel::project_file_diff(const std::string &file_id) {
  auto &diff = diffs_[file_id];
  std::vector<DiffLine> out;
  for (const auto &hunk : diff.hunks) {
    const bool expanded = diff.expanded.contains(hunk.id);
    DiffLine header;
    header.kind = slint::SharedString("banner");
    header.banner = slint::SharedString(expanded
                                            ? hunk.header
                                            : hunk.header + " · 已折叠未修改上下文");
    header.hunk = slint::SharedString(hunk.id);
    header.text = slint::SharedString(expanded ? "点击折叠" : "点击展开上下文");
    out.push_back(std::move(header));
    if (expanded) {
      for (const auto &row : hunk.rows) {
        DiffLine line;
        line.kind = slint::SharedString(row.kind == '+' ? "add"
                                        : row.kind == '-' ? "delete" : "context");
        line.old_num = slint::SharedString(row.kind == '+' ? "" : std::to_string(row.old_num));
        line.new_num = slint::SharedString(row.kind == '-' ? "" : std::to_string(row.new_num));
        line.text = slint::SharedString(row.text);
        out.push_back(std::move(line));
      }
      continue;
    }

    // Collapsed: keep changed rows plus a small context window around them.
    std::vector<bool> keep(hunk.rows.size(), false);
    for (std::size_t index = 0; index < hunk.rows.size(); ++index) {
      if (hunk.rows[index].kind == ' ')
        continue;
      const auto begin = index > static_cast<std::size_t>(kContextLines)
                             ? index - static_cast<std::size_t>(kContextLines)
                             : 0u;
      const auto end = std::min(hunk.rows.size(),
                                index + static_cast<std::size_t>(kContextLines) + 1);
      for (auto cursor = begin; cursor < end; ++cursor)
        keep[cursor] = true;
    }
    std::size_t skipped = 0;
    for (std::size_t index = 0; index < hunk.rows.size(); ++index) {
      if (keep[index]) {
        if (skipped > 0) {
          DiffLine banner;
          banner.kind = slint::SharedString("banner");
          banner.banner = slint::SharedString("折叠 " + std::to_string(skipped) + " 行未修改内容");
          banner.hunk = slint::SharedString(hunk.id);
          banner.text = slint::SharedString("点击展开上下文");
          out.push_back(std::move(banner));
          skipped = 0;
        }
        const auto &row = hunk.rows[index];
        DiffLine line;
        line.kind = slint::SharedString(row.kind == '+' ? "add"
                                        : row.kind == '-' ? "delete" : "context");
        line.old_num = slint::SharedString(row.kind == '+' ? "" : std::to_string(row.old_num));
        line.new_num = slint::SharedString(row.kind == '-' ? "" : std::to_string(row.new_num));
        line.text = slint::SharedString(row.text);
        out.push_back(std::move(line));
        continue;
      }
      ++skipped;
    }
    if (skipped > 0) {
      DiffLine banner;
      banner.kind = slint::SharedString("banner");
      banner.banner = slint::SharedString("折叠 " + std::to_string(skipped) + " 行未修改内容");
      banner.hunk = slint::SharedString(hunk.id);
      banner.text = slint::SharedString("点击展开上下文");
      out.push_back(std::move(banner));
    }
  }
  diff.projection = std::move(out);
}

const std::vector<DiffLine> &ReviewPanel::diff_lines(const std::string &file_id) {
  parse_file_diff(file_id);
  auto &diff = diffs_[file_id];
  if (diff.projection.empty())
    project_file_diff(file_id);
  return diff.projection;
}

void ReviewPanel::toggle_hunk(const std::string &file_id,
                              const std::string &hunk_id) {
  parse_file_diff(file_id);
  auto &diff = diffs_[file_id];
  if (diff.expanded.contains(hunk_id))
    diff.expanded.erase(hunk_id);
  else
    diff.expanded.insert(hunk_id);
  project_file_diff(file_id);
}

bool ReviewPanel::stage_all() {
  const auto output = run_process(workspace_, "git add -A");
  if (!output.launched || output.exit_code != 0) {
    last_error_ = trim(output.text);
    return false;
  }
  return true;
}

bool ReviewPanel::discard_unstaged() {
  const auto restored = run_process(workspace_, "git restore --worktree .");
  if (restored.launched && restored.exit_code == 0)
    return true;
  const auto checkout = run_process(workspace_, "git checkout -- .");
  if (!checkout.launched || checkout.exit_code != 0) {
    last_error_ = trim(checkout.text);
    return false;
  }
  return true;
}

bool ReviewPanel::commit(const std::string &message) {
  if (message.empty()) {
    last_error_ = "提交信息不能为空";
    return false;
  }
  const auto output =
      run_process(workspace_, "git commit -m " + quote_argument(message));
  if (!output.launched || output.exit_code != 0) {
    last_error_ = trim(output.text);
    return false;
  }
  return true;
}

bool ReviewPanel::checkout(const std::string &branch) {
  if (branch.empty()) {
    last_error_ = "分支名为空";
    return false;
  }
  const auto output =
      run_process(workspace_, "git checkout " + quote_argument(branch));
  if (!output.launched || output.exit_code != 0) {
    last_error_ = trim(output.text);
    return false;
  }
  branch_ = branch;
  return true;
}

bool ReviewPanel::push() {
  if (branch_.empty()) {
    last_error_ = "当前分支未知";
    return false;
  }
  const auto output = run_process(
      workspace_, "git push origin " + quote_argument(branch_));
  if (!output.launched || output.exit_code != 0) {
    last_error_ = trim(output.text);
    return false;
  }
  return true;
}

void ReviewPanel::reset_tree() {
  tree_entries_.clear();
  tree_.clear();
  if (workspace_.empty())
    return;
  std::error_code error;
  if (!std::filesystem::is_directory(workspace_, error))
    return;
  std::vector<TreeEntry> folders;
  std::vector<TreeEntry> files;
  for (std::filesystem::directory_iterator it(workspace_, error), end;
       it != end && !error; it.increment(error)) {
    std::error_code entry_error;
    const auto &entry = *it;
    const auto name = path_to_utf8(entry.path().filename());
    if (name.empty())
      continue;
    TreeEntry item;
    item.name = name;
    item.path = name;
    item.depth = 0;
    if (entry.is_directory(entry_error)) {
      item.folder = true;
      folders.push_back(std::move(item));
    } else if (entry.is_regular_file(entry_error)) {
      files.push_back(std::move(item));
    }
  }
  const auto by_name = [](const TreeEntry &left, const TreeEntry &right) {
    std::string a = left.name;
    std::string b = right.name;
    std::ranges::transform(a, a.begin(), [](unsigned char c) { return std::tolower(c); });
    std::ranges::transform(b, b.begin(), [](unsigned char c) { return std::tolower(c); });
    return a < b;
  };
  std::ranges::sort(folders, by_name);
  std::ranges::sort(files, by_name);
  tree_entries_ = std::move(folders);
  for (auto &file : files)
    tree_entries_.push_back(std::move(file));
  rebuild_tree();
}

void ReviewPanel::toggle_tree_folder(const std::string &path) {
  auto entry = std::ranges::find(
      tree_entries_, path, [](const TreeEntry &item) { return item.path; });
  if (entry == tree_entries_.end() || !entry->folder)
    return;
  if (entry->depth >= kMaxTreeDepth)
    return;
  const auto position = static_cast<std::size_t>(entry - tree_entries_.begin());
  if (!entry->loaded) {
    std::error_code error;
    const auto absolute = workspace_ / path;
    std::vector<TreeEntry> folders;
    std::vector<TreeEntry> files;
    for (std::filesystem::directory_iterator it(absolute, error), end;
         it != end && !error; it.increment(error)) {
      std::error_code entry_error;
      const auto &child = *it;
      const auto name = path_to_utf8(child.path().filename());
      if (name.empty())
        continue;
      TreeEntry item;
      item.name = name;
      item.path = entry->path + "/" + name;
      item.depth = entry->depth + 1;
      if (child.is_directory(entry_error)) {
        item.folder = true;
        folders.push_back(std::move(item));
      } else if (child.is_regular_file(entry_error)) {
        files.push_back(std::move(item));
      }
      if (tree_entries_.size() + folders.size() + files.size() >=
          kMaxTreeEntries)
        break;
    }
    const auto by_name = [](const TreeEntry &left, const TreeEntry &right) {
      std::string a = left.name;
      std::string b = right.name;
      std::ranges::transform(a, a.begin(), [](unsigned char c) { return std::tolower(c); });
      std::ranges::transform(b, b.begin(), [](unsigned char c) { return std::tolower(c); });
      return a < b;
    };
    std::ranges::sort(folders, by_name);
    std::ranges::sort(files, by_name);
    std::vector<TreeEntry> children = std::move(folders);
    for (auto &file : files)
      children.push_back(std::move(file));
    tree_entries_.insert(tree_entries_.begin() + static_cast<std::ptrdiff_t>(position) + 1,
                         std::make_move_iterator(children.begin()),
                         std::make_move_iterator(children.end()));
    tree_entries_[position].loaded = true;
    entry = tree_entries_.begin() + static_cast<std::ptrdiff_t>(position);
  }
  entry->expanded = !entry->expanded;
  if (entry->expanded && tree_entries_.size() > kMaxTreeEntries) {
    entry->expanded = false;
    return;
  }
  rebuild_tree();
}

void ReviewPanel::rebuild_tree() {
  tree_.clear();
  // `open_depth[d]` tracks whether the ancestor sitting at depth `d` is open.
  std::vector<bool> open_depth(static_cast<std::size_t>(kMaxTreeDepth) + 2, false);
  for (const auto &entry : tree_entries_) {
    const auto depth = static_cast<std::size_t>(entry.depth);
    if (depth >= open_depth.size())
      continue;
    if (depth > 0 && !open_depth[depth - 1])
      continue;
    TreeItem item;
    item.name = slint::SharedString(entry.name);
    item.path = slint::SharedString(entry.path);
    item.kind = slint::SharedString(entry.folder ? "folder" : "file");
    item.depth = entry.depth;
    item.expanded = entry.expanded;
    tree_.push_back(std::move(item));
    if (depth < open_depth.size())
      open_depth[depth] = entry.expanded;
  }
}

bool ReviewPanel::read_tree_file(const std::string &path, std::string *name,
                                std::string *content) const {
  const auto entry = std::ranges::find(
      tree_entries_, path, [](const TreeEntry &item) { return item.path; });
  if (entry == tree_entries_.end() || entry->folder)
    return false;
  std::error_code error;
  const auto absolute = workspace_ / path;
  if (!std::filesystem::is_regular_file(absolute, error))
    return false;
  const auto size = std::filesystem::file_size(absolute, error);
  if (error || size > kMaxFileBytes)
    return false;
  std::ifstream stream(absolute, std::ios::binary);
  if (!stream)
    return false;
  std::string text((std::istreambuf_iterator<char>(stream)),
                   std::istreambuf_iterator<char>());
  if (text.find('\0') != std::string::npos)
    return false; // Binary content has no useful read-only preview.
  if (name != nullptr)
    *name = entry->name;
  if (content != nullptr)
    *content = std::move(text);
  return true;
}

bool reveal_path_in_file_manager(const std::filesystem::path &path) {
  if (path.empty())
    return false;
  std::error_code error;
#ifdef _WIN32
  auto target = std::filesystem::absolute(path, error);
  if (error)
    target = path;
  target = target.lexically_normal().make_preferred();

  // Deleted review entries no longer exist. In that case open their nearest
  // existing parent instead of reporting a misleading locate failure.
  const bool exists = std::filesystem::exists(target, error) && !error;
  if (!exists) {
    auto parent = target.parent_path();
    while (!parent.empty() &&
           !std::filesystem::exists(parent, error))
      parent = parent.parent_path();
    if (parent.empty())
      return false;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
  }

  if (std::filesystem::is_directory(target, error) && !error) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
  }

  // Quote the native path: explorer otherwise splits paths containing spaces
  // and silently selects an unrelated location.
  const auto wide_argument = L"/select,\"" + target.native() + L"\"";
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
      nullptr, L"open", L"explorer.exe", wide_argument.c_str(), nullptr,
      SW_SHOWNORMAL));
  return result > 32;
#else
  if (!std::filesystem::exists(path, error))
    return false;
  return false;
#endif
}

} // namespace tokmon::desktop
