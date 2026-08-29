#include "review/git_service.hpp"

#include "lenses/common/process_runner.hpp"
#include "editor/document_store.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(TOKMON_DESK_HAS_LIBGIT2)
#include <git2.h>
#endif

namespace {

#if defined(TOKMON_DESK_HAS_LIBGIT2)
std::string git_error_text(const char* fallback) {
  const auto* value = git_error_last();
  return value && value->message ? value->message : fallback;
}

char index_code(unsigned status) {
  if (status & GIT_STATUS_INDEX_NEW) return 'A';
  if (status & GIT_STATUS_INDEX_MODIFIED) return 'M';
  if (status & GIT_STATUS_INDEX_DELETED) return 'D';
  if (status & GIT_STATUS_INDEX_RENAMED) return 'R';
  if (status & GIT_STATUS_INDEX_TYPECHANGE) return 'T';
  if (status & GIT_STATUS_WT_NEW) return '?';
  return ' ';
}

char worktree_code(unsigned status) {
  if (status & GIT_STATUS_WT_NEW) return '?';
  if (status & GIT_STATUS_WT_MODIFIED) return 'M';
  if (status & GIT_STATUS_WT_DELETED) return 'D';
  if (status & GIT_STATUS_WT_RENAMED) return 'R';
  if (status & GIT_STATUS_WT_TYPECHANGE) return 'T';
  return ' ';
}

struct DiffBuildContext {
  tokmon::desk::GitFileDiff model;
};

int diff_file_callback(const git_diff_delta* delta, float, void* payload) {
  auto& model = static_cast<DiffBuildContext*>(payload)->model;
  const auto status = delta->status;
  model.binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
  model.created = status == GIT_DELTA_ADDED || status == GIT_DELTA_UNTRACKED;
  model.deleted = status == GIT_DELTA_DELETED;
  model.renamed = status == GIT_DELTA_RENAMED;
  return 0;
}

int diff_hunk_callback(const git_diff_delta*, const git_diff_hunk* hunk,
                       void* payload) {
  auto& model = static_cast<DiffBuildContext*>(payload)->model;
  tokmon::desk::GitDiffHunk result;
  result.index = model.hunks.size();
  result.old_start = hunk->old_start;
  result.old_lines = hunk->old_lines;
  result.new_start = hunk->new_start;
  result.new_lines = hunk->new_lines;
  result.header.assign(hunk->header, hunk->header_len);
  model.hunks.push_back(std::move(result));
  return 0;
}

int diff_line_callback(const git_diff_delta*, const git_diff_hunk*,
                       const git_diff_line* line, void* payload) {
  auto& model = static_cast<DiffBuildContext*>(payload)->model;
  if (model.hunks.empty())
    return 0;
  model.hunks.back().lines.push_back(
      {line->origin, line->old_lineno, line->new_lineno,
       std::string(line->content, line->content_len)});
  return 0;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool write_atomic(const std::filesystem::path& path, const std::string& text,
                  std::string& error) {
  auto temporary = path;
  temporary += ".tokmon-desk-git.tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
      error = "cannot write temporary worktree file";
      return false;
    }
  }
  std::error_code ec;
#if defined(_WIN32)
  const auto target = path.wstring();
  const auto source = temporary.wstring();
  if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr,
                    REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) &&
      !MoveFileExW(source.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    ec = std::error_code(static_cast<int>(GetLastError()),
                         std::system_category());
#else
  std::filesystem::rename(temporary, path, ec);
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    error = "cannot atomically update worktree file: " + ec.message();
    return false;
  }
  return true;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    if (end == std::string::npos) {
      lines.push_back(text.substr(begin));
      begin = text.size();
    } else {
      lines.push_back(text.substr(begin, end - begin + 1));
      begin = end + 1;
    }
  }
  return lines;
}

bool apply_hunk_text(std::string& text, const tokmon::desk::GitDiffHunk& hunk,
                     const bool reverse, std::string& error) {
  std::vector<std::string> before;
  std::vector<std::string> after;
  for (const auto& line : hunk.lines) {
    if (line.origin == ' ' || line.origin == '-')
      before.push_back(line.content);
    if (line.origin == ' ' || line.origin == '+')
      after.push_back(line.content);
  }
  const auto& expected = reverse ? after : before;
  const auto& replacement = reverse ? before : after;
  auto lines = split_lines(text);
  const int line_number = reverse ? hunk.new_start : hunk.old_start;
  const auto offset = static_cast<std::size_t>(std::max(line_number - 1, 0));
  if (offset > lines.size() || offset + expected.size() > lines.size() ||
      !std::equal(expected.begin(), expected.end(), lines.begin() + offset)) {
    error = "hunk context no longer matches current content";
    return false;
  }
  lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(offset),
              lines.begin() + static_cast<std::ptrdiff_t>(offset + expected.size()));
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(offset),
               replacement.begin(), replacement.end());
  text.clear();
  for (const auto& line : lines)
    text += line;
  return true;
}

bool load_index_text(git_repository* repository, const std::string& path,
                     std::string& text, unsigned& mode, std::string& error) {
  git_index* index = nullptr;
  if (git_repository_index(&index, repository) != 0) {
    error = git_error_text("could not open Git index");
    return false;
  }
  const auto* entry = git_index_get_bypath(index, path.c_str(), 0);
  if (!entry) {
    text.clear();
    mode = GIT_FILEMODE_BLOB;
    git_index_free(index);
    return true;
  }
  mode = entry->mode;
  git_blob* blob = nullptr;
  const bool ok = git_blob_lookup(&blob, repository, &entry->id) == 0;
  if (ok)
    text.assign(static_cast<const char*>(git_blob_rawcontent(blob)),
                git_blob_rawsize(blob));
  else
    error = git_error_text("could not read index blob");
  git_blob_free(blob);
  git_index_free(index);
  return ok;
}

bool store_index_text(git_repository* repository, const std::string& path,
                      const std::string& text, unsigned mode,
                      std::string& error) {
  git_oid id{};
  git_index* index = nullptr;
  if (git_blob_create_from_buffer(&id, repository, text.data(), text.size()) != 0 ||
      git_repository_index(&index, repository) != 0) {
    error = git_error_text("could not prepare index blob");
    git_index_free(index);
    return false;
  }
  git_index_entry entry{};
  entry.mode = mode ? mode : GIT_FILEMODE_BLOB;
  entry.id = id;
  entry.path = path.c_str();
  const bool ok = git_index_add(index, &entry) == 0 && git_index_write(index) == 0;
  if (!ok)
    error = git_error_text("could not update Git index");
  git_index_free(index);
  return ok;
}
#endif

} // namespace

namespace tokmon::desk {

GitService::GitService(std::filesystem::path workspace)
    : workspace_(std::move(workspace)) {}

void GitService::set_workspace(std::filesystem::path workspace) {
  workspace_ = std::move(workspace);
}

bool GitService::resolve_path(const std::string& relative,
                              std::filesystem::path& absolute,
                              std::string& error) const {
  if (relative.empty() || std::filesystem::path(relative).is_absolute()) {
    error = "Git path must be workspace-relative";
    return false;
  }
  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(workspace_, ec);
  if (ec) {
    error = "invalid workspace path";
    return false;
  }
  absolute = std::filesystem::weakly_canonical(root / relative, ec);
  if (ec)
    absolute = (root / relative).lexically_normal();
  const auto rel = std::filesystem::relative(absolute, root, ec);
  if (ec || rel.is_absolute() || (!rel.empty() && *rel.begin() == "..")) {
    error = "Git path escapes workspace";
    return false;
  }
  return true;
}

bool GitService::run(const std::vector<std::string>& argv, std::string& output,
                     std::string& error) const {
  auto result = tokmon::builtin::run_process(argv, workspace_, std::chrono::seconds(30),
                                              8u * 1024u * 1024u);
  if (!result) {
    error = result.error().describe();
    return false;
  }
  output = std::move(result->stdout_text);
  if (result->exit_code != 0) {
    error = result->stderr_text.empty() ? "git command failed" : result->stderr_text;
    return false;
  }
  return true;
}

GitSnapshot GitService::status() const {
  GitSnapshot snapshot;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  if (git_repository_open_ext(&repository, workspace_.string().c_str(),
                              GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0) {
    snapshot.error = git_error_text("not a Git repository");
    git_libgit2_shutdown();
    return snapshot;
  }
  snapshot.repository = true;
  git_reference* head = nullptr;
  if (git_repository_head(&head, repository) == 0) {
    if (const auto* branch = git_reference_shorthand(head)) snapshot.branch = branch;
    git_reference_free(head);
  }
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                  GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                  GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
  git_status_list* list = nullptr;
  if (git_status_list_new(&list, repository, &options) != 0) {
    snapshot.error = git_error_text("could not read Git status");
  } else {
    const auto count = git_status_list_entrycount(list);
    snapshot.files.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto* entry = git_status_byindex(list, index);
      const char* path = nullptr;
      if (entry->index_to_workdir) path = entry->index_to_workdir->new_file.path;
      if (!path && entry->head_to_index) path = entry->head_to_index->new_file.path;
      if (path) snapshot.files.push_back(
          {path, index_code(entry->status), worktree_code(entry->status)});
    }
    git_status_list_free(list);
  }
  git_repository_free(repository);
  git_libgit2_shutdown();
  return snapshot;
#else
  std::string output;
  if (!run({"git", "status", "--porcelain=v1", "-z", "--branch"}, output,
           snapshot.error))
    return snapshot;
  snapshot.repository = true;
  std::size_t offset = 0;
  bool first = true;
  while (offset < output.size()) {
    const auto end = output.find('\0', offset);
    const auto item = output.substr(offset, end == std::string::npos ? std::string::npos
                                                                     : end - offset);
    offset = end == std::string::npos ? output.size() : end + 1;
    if (first && item.starts_with("## ")) {
      first = false;
      auto branch = item.substr(3);
      if (const auto separator = branch.find("..."); separator != std::string::npos)
        branch.resize(separator);
      snapshot.branch = std::move(branch);
      continue;
    }
    first = false;
    if (item.size() >= 4)
      snapshot.files.push_back({item.substr(3), item[0], item[1]});
  }
  return snapshot;
#endif
}

std::vector<std::string> GitService::branches(std::string& error) const {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  std::vector<std::string> result;
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_branch_iterator* iterator = nullptr;
  if (git_repository_open_ext(&repository, workspace_.string().c_str(),
                              GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0 ||
      git_branch_iterator_new(&iterator, repository,
                              GIT_BRANCH_LOCAL) != 0) {
    error = git_error_text("could not enumerate Git branches");
  } else {
    git_reference* reference = nullptr;
    git_branch_t type{};
    while (git_branch_next(&reference, &type, iterator) == 0) {
      const char* name = nullptr;
      if (git_branch_name(&name, reference) == 0 && name)
        result.emplace_back(name);
      git_reference_free(reference);
      reference = nullptr;
    }
    std::ranges::sort(result);
  }
  git_branch_iterator_free(iterator);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return result;
#else
  std::string output;
  if (!run({"git", "for-each-ref", "--format=%(refname:short)",
            "refs/heads"}, output, error))
    return {};
  std::vector<std::string> result;
  for (std::size_t begin = 0; begin < output.size();) {
    const auto end = output.find('\n', begin);
    auto value = output.substr(begin, end == std::string::npos
                                          ? std::string::npos
                                          : end - begin);
    if (!value.empty() && value.back() == '\r')
      value.pop_back();
    if (!value.empty())
      result.push_back(std::move(value));
    begin = end == std::string::npos ? output.size() : end + 1;
  }
  return result;
#endif
}

bool GitService::checkout_branch(const std::string& branch,
                                 std::string& error) const {
  if (branch.empty() || branch.size() > 256 || branch.starts_with('-') ||
      branch.find('\0') != std::string::npos) {
    error = "invalid Git branch";
    return false;
  }
  const auto snapshot = status();
  if (!snapshot.repository) {
    error = snapshot.error.empty() ? "not a Git repository" : snapshot.error;
    return false;
  }
  if (!snapshot.files.empty()) {
    error = "请先提交、暂存处理或放弃当前修改，再切换分支";
    return false;
  }
  if (snapshot.branch == branch)
    return true;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_reference* reference = nullptr;
  git_object* target = nullptr;
  bool success =
      git_repository_open_ext(&repository, workspace_.string().c_str(),
                              GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
      git_branch_lookup(&reference, repository, branch.c_str(),
                        GIT_BRANCH_LOCAL) == 0 &&
      git_reference_peel(&target, reference, GIT_OBJECT_COMMIT) == 0;
  if (success) {
    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    options.checkout_strategy = GIT_CHECKOUT_SAFE;
    success = git_checkout_tree(repository, target, &options) == 0 &&
              git_repository_set_head(repository,
                                      git_reference_name(reference)) == 0;
  }
  if (!success)
    error = git_error_text("could not switch Git branch safely");
  git_object_free(target);
  git_reference_free(reference);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  std::string output;
  return run({"git", "switch", "--", branch}, output, error);
#endif
}

std::string GitService::diff(const std::string& path, bool staged,
                             std::string& error) const {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  git_tree* tree = nullptr;
  git_diff* diff = nullptr;
  git_buf buffer = GIT_BUF_INIT;
  std::string result;
  if (git_repository_open_ext(&repository, workspace_.string().c_str(),
                              GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0 ||
      git_repository_index(&index, repository) != 0) {
    error = git_error_text("could not open Git repository/index");
  } else {
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    char* selected = const_cast<char*>(path.c_str());
    options.pathspec = {&selected, 1};
    int code = 0;
    if (staged) {
      git_object* tree_object = nullptr;
      code = git_revparse_single(&tree_object, repository, "HEAD^{tree}");
      tree = reinterpret_cast<git_tree*>(tree_object);
      if (code == 0) code = git_diff_tree_to_index(&diff, repository, tree, index, &options);
    } else {
      code = git_diff_index_to_workdir(&diff, repository, index, &options);
    }
    if (code != 0 || git_diff_to_buf(&buffer, diff, GIT_DIFF_FORMAT_PATCH) != 0)
      error = git_error_text("could not create diff");
    else
      result.assign(buffer.ptr ? buffer.ptr : "", buffer.size);
  }
  git_buf_dispose(&buffer);
  git_diff_free(diff);
  git_tree_free(tree);
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return result;
#else
  std::vector<std::string> argv{"git", "diff", "--no-ext-diff", "--unified=3"};
  if (staged)
    argv.push_back("--cached");
  argv.push_back("--");
  argv.push_back(path);
  std::string output;
  (void)run(argv, output, error);
  return output;
#endif
}

std::optional<GitFileDiff> GitService::diff_model(
    const std::string& path, const bool staged, std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_path(path, absolute, error))
    return std::nullopt;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  git_tree* tree = nullptr;
  git_diff* diff_value = nullptr;
  DiffBuildContext context;
  context.model.path = path;
  context.model.staged = staged;
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 git_repository_index(&index, repository) == 0;
  if (success) {
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    char* selected = const_cast<char*>(path.c_str());
    options.pathspec = {&selected, 1};
    options.flags |= GIT_DIFF_INCLUDE_UNTRACKED |
                     GIT_DIFF_RECURSE_UNTRACKED_DIRS |
                     GIT_DIFF_SHOW_UNTRACKED_CONTENT;
    int code = 0;
    if (staged) {
      git_object* tree_object = nullptr;
      code = git_revparse_single(&tree_object, repository, "HEAD^{tree}");
      tree = reinterpret_cast<git_tree*>(tree_object);
      if (code == 0)
        code = git_diff_tree_to_index(&diff_value, repository, tree, index, &options);
    } else {
      code = git_diff_index_to_workdir(&diff_value, repository, index, &options);
    }
    success = code == 0 &&
              git_diff_find_similar(diff_value, nullptr) == 0 &&
              git_diff_foreach(diff_value, diff_file_callback, nullptr,
                               diff_hunk_callback, diff_line_callback,
                               &context) == 0;
    git_buf patch = GIT_BUF_INIT;
    if (success && git_diff_to_buf(&patch, diff_value, GIT_DIFF_FORMAT_PATCH) == 0)
      context.model.patch.assign(patch.ptr ? patch.ptr : "", patch.size);
    else if (success)
      success = false;
    git_buf_dispose(&patch);
  }
  if (!success)
    error = git_error_text("could not create structured diff");
  git_diff_free(diff_value);
  git_tree_free(tree);
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success ? std::optional<GitFileDiff>(std::move(context.model))
                 : std::nullopt;
#else
  GitFileDiff result;
  result.path = path;
  result.staged = staged;
  result.patch = diff(path, staged, error);
  return error.empty() ? std::optional<GitFileDiff>(std::move(result))
                       : std::nullopt;
#endif
}

bool GitService::stage_file(const std::string& path, std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_path(path, absolute, error))
    return false;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 git_repository_index(&index, repository) == 0;
  std::error_code ec;
  if (success && std::filesystem::exists(absolute, ec))
    success = git_index_add_bypath(index, path.c_str()) == 0;
  else if (success)
    success = git_index_remove_bypath(index, path.c_str()) == 0;
  success = success && git_index_write(index) == 0;
  if (!success)
    error = git_error_text("could not stage file");
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  std::string output;
  return run({"git", "add", "--", path}, output, error);
#endif
}

bool GitService::unstage_file(const std::string& path, std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_path(path, absolute, error))
    return false;
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_object* head = nullptr;
  char* selected = const_cast<char*>(path.c_str());
  git_strarray paths{&selected, 1};
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0;
  if (success && git_revparse_single(&head, repository, "HEAD") == GIT_ENOTFOUND)
    head = nullptr;
  if (success)
    success = git_reset_default(repository, head, &paths) == 0;
  if (!success)
    error = git_error_text("could not unstage file");
  git_object_free(head);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  std::string output;
  return run({"git", "reset", "--mixed", "HEAD", "--", path}, output, error);
#endif
}

bool GitService::stage_hunk(const std::string& path,
                            const std::size_t hunk_index,
                            std::string& error) const {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  const auto model = diff_model(path, false, error);
  if (!model || hunk_index >= model->hunks.size()) {
    if (error.empty()) error = "unknown worktree hunk";
    return false;
  }
  git_libgit2_init();
  git_repository* repository = nullptr;
  std::string text;
  unsigned mode = GIT_FILEMODE_BLOB;
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 load_index_text(repository, path, text, mode, error) &&
                 apply_hunk_text(text, model->hunks[hunk_index], false, error) &&
                 store_index_text(repository, path, text, mode, error);
  if (!success && error.empty())
    error = git_error_text("could not stage hunk");
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  error = "hunk staging requires libgit2";
  return false;
#endif
}

bool GitService::unstage_hunk(const std::string& path,
                              const std::size_t hunk_index,
                              std::string& error) const {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  const auto model = diff_model(path, true, error);
  if (!model || hunk_index >= model->hunks.size()) {
    if (error.empty()) error = "unknown staged hunk";
    return false;
  }
  git_libgit2_init();
  git_repository* repository = nullptr;
  std::string text;
  unsigned mode = GIT_FILEMODE_BLOB;
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 load_index_text(repository, path, text, mode, error) &&
                 apply_hunk_text(text, model->hunks[hunk_index], true, error) &&
                 store_index_text(repository, path, text, mode, error);
  if (!success && error.empty())
    error = git_error_text("could not unstage hunk");
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  error = "hunk unstaging requires libgit2";
  return false;
#endif
}

bool GitService::discard_hunk(const std::string& path,
                              const std::size_t hunk_index,
                              const std::uint64_t expected_hash,
                              std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_path(path, absolute, error))
    return false;
  const auto current = read_file(absolute);
  if (!current || DocumentStore::content_hash(*current) != expected_hash) {
    error = "worktree changed since diff was reviewed";
    return false;
  }
  const auto model = diff_model(path, false, error);
  if (!model || hunk_index >= model->hunks.size()) {
    if (error.empty()) error = "unknown worktree hunk";
    return false;
  }
  auto replacement = *current;
  return apply_hunk_text(replacement, model->hunks[hunk_index], true, error) &&
         write_atomic(absolute, replacement, error);
}

bool GitService::discard_file(const std::string& path,
                              const std::uint64_t expected_hash,
                              const bool allow_delete_untracked,
                              std::string& error) const {
  std::filesystem::path absolute;
  if (!resolve_path(path, absolute, error))
    return false;
  const auto current = read_file(absolute);
  if (!current || DocumentStore::content_hash(*current) != expected_hash) {
    error = "worktree changed since diff was reviewed";
    return false;
  }
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                         GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 git_repository_index(&index, repository) == 0;
  const auto* entry = success ? git_index_get_bypath(index, path.c_str(), 0) : nullptr;
  if (success && !entry) {
    if (!allow_delete_untracked) {
      error = "discarding an untracked file requires explicit confirmation";
      success = false;
    } else {
      std::error_code ec;
      success = std::filesystem::remove(absolute, ec);
      if (!success) error = "cannot remove untracked file: " + ec.message();
    }
  } else if (success) {
    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    char* selected = const_cast<char*>(path.c_str());
    options.paths = {&selected, 1};
    options.checkout_strategy = GIT_CHECKOUT_FORCE;
    success = git_checkout_index(repository, index, &options) == 0;
    if (!success) error = git_error_text("could not restore file from index");
  }
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  error = "guarded discard requires libgit2";
  return false;
#endif
}

bool GitService::stage_all(std::string& error) const {
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  git_strarray paths{nullptr, 0};
  const bool success =
      git_repository_open_ext(&repository, workspace_.string().c_str(),
                              GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
      git_repository_index(&index, repository) == 0 &&
      git_index_add_all(index, &paths, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) == 0 &&
      git_index_update_all(index, &paths, nullptr, nullptr) == 0 &&
      git_index_write(index) == 0;
  if (!success) error = git_error_text("could not stage changes");
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  std::string output;
  return run({"git", "add", "--all"}, output, error);
#endif
}

bool GitService::unstage_all(std::string& error) const {
  std::string output;
  return run({"git", "reset", "--mixed", "HEAD", "--"}, output, error);
}

bool GitService::commit(std::string message, std::string& error) const {
  if (message.empty()) {
    error = "commit message is empty";
    return false;
  }
#if defined(TOKMON_DESK_HAS_LIBGIT2)
  git_libgit2_init();
  git_repository* repository = nullptr;
  git_index* index = nullptr;
  git_tree* tree = nullptr;
  git_commit* parent = nullptr;
  git_signature* signature = nullptr;
  git_oid tree_id{}, commit_id{};
  bool success = git_repository_open_ext(&repository, workspace_.string().c_str(),
                                          GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) == 0 &&
                 git_repository_index(&index, repository) == 0 &&
                 git_index_write_tree(&tree_id, index) == 0 &&
                 git_tree_lookup(&tree, repository, &tree_id) == 0;
  if (success && git_signature_default(&signature, repository) != 0)
    success = git_signature_now(&signature, "Tokmon Desk", "tokmon-desk@localhost") == 0;
  if (success) {
    const git_commit* parents[1]{};
    std::size_t parent_count = 0;
    git_oid parent_id{};
    if (git_reference_name_to_id(&parent_id, repository, "HEAD") == 0 &&
        git_commit_lookup(&parent, repository, &parent_id) == 0) {
      parents[0] = parent;
      parent_count = 1;
    }
    success = git_commit_create(&commit_id, repository, "HEAD", signature, signature,
                                "UTF-8", message.c_str(), tree,
                                parent_count, parents) == 0;
  }
  if (!success) error = git_error_text("could not create commit");
  git_signature_free(signature);
  git_commit_free(parent);
  git_tree_free(tree);
  git_index_free(index);
  git_repository_free(repository);
  git_libgit2_shutdown();
  return success;
#else
  std::string output;
  return run({"git", "commit", "-m", std::move(message)}, output, error);
#endif
}

bool GitService::push(std::string& error) const {
  std::string output;
  return run({"git", "push"}, output, error);
}

} // namespace tokmon::desk
