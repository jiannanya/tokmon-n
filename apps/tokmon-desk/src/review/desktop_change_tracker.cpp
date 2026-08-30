#include "review/desktop_change_tracker.hpp"

#include "tokmon/hash.hpp"
#include "tokmon/ids.hpp"

#include <algorithm>
#include <fstream>
#include <set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tokmon::desk {
namespace {

constexpr std::uintmax_t kMaximumPreimageBytes = 16u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumSnapshotStoreBytes = 512u * 1024u * 1024u;

bool untracked(const GitFileStatus& status) {
  return status.index_status == '?' && status.worktree_status == '?';
}

std::uintmax_t store_size(const std::filesystem::path& root) {
  std::uintmax_t result = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error))
      result += iterator->file_size(error);
    error.clear();
    if (result > kMaximumSnapshotStoreBytes)
      break;
  }
  return result;
}

bool atomic_write(const std::filesystem::path& path, const std::string_view text,
                  std::string& error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "cannot create change snapshot directory: " +
            filesystem_error.message();
    return false;
  }
  auto temporary = path;
  temporary += ".tmp-" + tokmon::make_id("change");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
      error = "cannot write change snapshot";
      std::filesystem::remove(temporary, filesystem_error);
      return false;
    }
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "cannot atomically replace change snapshot";
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
#else
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    error = "cannot atomically replace change snapshot: " +
            filesystem_error.message();
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
#endif
  return true;
}

} // namespace

DesktopChangeTracker::DesktopChangeTracker(std::filesystem::path workspace,
                                           std::filesystem::path snapshot_root)
    : workspace_(std::move(workspace)), snapshot_root_(std::move(snapshot_root)),
      git_(workspace_) {}

void DesktopChangeTracker::set_workspace(std::filesystem::path workspace) {
  workspace_ = std::move(workspace);
  git_.set_workspace(workspace_);
  active_ = false;
  baseline_.clear();
}

std::optional<std::string> DesktopChangeTracker::read_workspace_file(
    const std::string& path, std::string& error) const {
  const auto absolute = workspace_ / std::filesystem::path(path);
  std::error_code filesystem_error;
  const auto canonical_parent = std::filesystem::weakly_canonical(
      absolute.parent_path(), filesystem_error);
  const auto canonical_root = std::filesystem::weakly_canonical(
      workspace_, filesystem_error);
  const auto relative = std::filesystem::relative(canonical_parent,
                                                   canonical_root,
                                                   filesystem_error);
  if (filesystem_error || relative.is_absolute() ||
      (!relative.empty() && *relative.begin() == "..")) {
    error = "change path escapes workspace";
    return std::nullopt;
  }
  if (!std::filesystem::exists(absolute, filesystem_error))
    return std::nullopt;
  if (!std::filesystem::is_regular_file(absolute, filesystem_error)) {
    error = "change path is not a regular file";
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(absolute, filesystem_error);
  if (filesystem_error || size > kMaximumPreimageBytes) {
    error = "change file exceeds the reversible snapshot limit";
    return std::nullopt;
  }
  std::ifstream input(absolute, std::ios::binary);
  if (!input) {
    error = "cannot read change file";
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string DesktopChangeTracker::store_blob(const std::string_view content,
                                             std::string& error) const {
  const auto digest = tokmon::sha256_hex(content);
  const auto path = snapshot_root_ / digest.substr(0, 2) / (digest + ".blob");
  std::error_code filesystem_error;
  if (std::filesystem::exists(path, filesystem_error))
    return digest;
  if (store_size(snapshot_root_) + content.size() > kMaximumSnapshotStoreBytes) {
    error = "change snapshot store quota exceeded";
    return {};
  }
  return atomic_write(path, content, error) ? digest : std::string{};
}

std::optional<std::string> DesktopChangeTracker::load_blob(
    const std::string_view digest, std::string& error) const {
  if (digest.size() != 64) {
    error = "invalid preimage digest";
    return std::nullopt;
  }
  const auto path = snapshot_root_ / std::string(digest.substr(0, 2)) /
                    (std::string(digest) + ".blob");
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "preimage snapshot is missing";
    return std::nullopt;
  }
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  if (tokmon::sha256_hex(content) != digest) {
    error = "preimage snapshot failed integrity validation";
    return std::nullopt;
  }
  return content;
}

bool DesktopChangeTracker::begin(std::string run_id, std::string& error) {
  error.clear();
  if (active_) {
    error = "a Desktop change baseline is already active";
    return false;
  }
  const auto snapshot = git_.status();
  if (!snapshot.repository) {
    error = "Desktop change attribution requires a Git workspace";
    return false;
  }
  baseline_head_ = git_.head_revision(error);
  if (baseline_head_.empty())
    return false;
  baseline_.clear();
  for (const auto& status : snapshot.files) {
    std::string read_error;
    auto content = read_workspace_file(status.path, read_error);
    BaselineEntry entry;
    entry.existed = content.has_value();
    if (content) {
      entry.sha256 = tokmon::sha256_hex(*content);
      entry.blob = store_blob(*content, error);
      if (entry.blob.empty()) {
        baseline_.clear();
        return false;
      }
    } else if (!read_error.empty() && !untracked(status)) {
      error = std::move(read_error);
      baseline_.clear();
      return false;
    }
    baseline_.insert_or_assign(status.path, std::move(entry));
  }
  // A clean tracked file is absent from Git status, but the Agent may modify
  // it during the run. Snapshot its exact worktree bytes now (including BOM,
  // CRLF, and clean/smudge-filter output) so rejection restores what the user
  // actually had, not merely the canonical blob stored in HEAD.
  const auto tracked = git_.tracked_files(error);
  if (!error.empty()) {
    baseline_.clear();
    return false;
  }
  for (const auto& path : tracked) {
    if (baseline_.contains(path))
      continue;
    std::string read_error;
    auto content = read_workspace_file(path, read_error);
    if (!content) {
      if (!read_error.empty()) {
        error = std::move(read_error);
        baseline_.clear();
        return false;
      }
      continue;
    }
    BaselineEntry entry{.existed = true,
                        .sha256 = tokmon::sha256_hex(*content)};
    entry.blob = store_blob(*content, error);
    if (entry.blob.empty()) {
      baseline_.clear();
      return false;
    }
    baseline_.insert_or_assign(path, std::move(entry));
  }
  run_id_ = std::move(run_id);
  active_ = true;
  return true;
}

std::optional<DesktopChangeSet> DesktopChangeTracker::finish(
    std::string& error) {
  error.clear();
  if (!active_) {
    error = "no Desktop change baseline is active";
    return std::nullopt;
  }
  active_ = false;
  const auto current_head = git_.head_revision(error);
  if (current_head != baseline_head_) {
    error = "Git HEAD changed during the Agent run; precise attribution was cancelled";
    baseline_.clear();
    return std::nullopt;
  }
  const auto current = git_.status();
  if (!current.repository) {
    error = "Git workspace became unavailable";
    baseline_.clear();
    return std::nullopt;
  }
  std::set<std::string> paths;
  for (const auto& [path, entry] : baseline_) {
    (void)entry;
    paths.insert(path);
  }
  for (const auto& status : current.files)
    paths.insert(status.path);

  DesktopChangeSet result;
  result.id = tokmon::make_id("desk-changeset");
  result.run_id = run_id_;
  result.baseline_head = baseline_head_;
  for (const auto& path : paths) {
    bool existed_before = false;
    std::string before;
    std::string preimage_blob;
    if (const auto found = baseline_.find(path); found != baseline_.end()) {
      existed_before = found->second.existed;
      preimage_blob = found->second.blob;
      if (existed_before) {
        auto loaded = load_blob(preimage_blob, error);
        if (!loaded) {
          baseline_.clear();
          return std::nullopt;
        }
        before = std::move(*loaded);
      }
    } else {
      auto head = git_.head_file(path, error);
      if (head) {
        existed_before = true;
        before = std::move(*head);
        preimage_blob = store_blob(before, error);
        if (preimage_blob.empty()) {
          baseline_.clear();
          return std::nullopt;
        }
      } else {
        error.clear();
      }
    }
    std::string read_error;
    auto after = read_workspace_file(path, read_error);
    if (!read_error.empty() && !after) {
      error = std::move(read_error);
      baseline_.clear();
      return std::nullopt;
    }
    const auto before_hash = existed_before ? tokmon::sha256_hex(before) : "absent";
    const auto after_hash = after ? tokmon::sha256_hex(*after) : "absent";
    if (before_hash == after_hash)
      continue;
    result.changes.push_back({
        .path = path, .existed_before = existed_before,
        .exists_after = after.has_value(),
        .reversible = !existed_before || !preimage_blob.empty(),
        .before_sha256 = before_hash, .after_sha256 = after_hash,
        .preimage_blob = std::move(preimage_blob)});
  }
  baseline_.clear();
  return result;
}

bool DesktopChangeTracker::accept(DesktopChangeSet& changes,
                                  std::string& error) const {
  error.clear();
  if (changes.rejected) {
    error = "rejected ChangeSet cannot be accepted";
    return false;
  }
  changes.accepted = true;
  return true;
}

bool DesktopChangeTracker::write_workspace_file(const std::string& path,
                                                const std::string_view content,
                                                std::string& error) const {
  return atomic_write(workspace_ / std::filesystem::path(path), content, error);
}

bool DesktopChangeTracker::reject(DesktopChangeSet& changes,
                                  std::string& error) const {
  error.clear();
  if (changes.accepted) {
    error = "accepted ChangeSet cannot be rejected";
    return false;
  }
  for (const auto& change : changes.changes) {
    std::string read_error;
    const auto current = read_workspace_file(change.path, read_error);
    if (!read_error.empty() && !current) {
      error = std::move(read_error);
      return false;
    }
    const auto current_hash = current ? tokmon::sha256_hex(*current) : "absent";
    if (current_hash != change.after_sha256) {
      error = "workspace changed after review for " + change.path;
      return false;
    }
  }
  for (const auto& change : changes.changes) {
    if (!change.existed_before) {
      std::error_code filesystem_error;
      std::filesystem::remove(workspace_ / std::filesystem::path(change.path),
                              filesystem_error);
      if (filesystem_error) {
        error = "cannot remove Agent-created file " + change.path + ": " +
                filesystem_error.message();
        return false;
      }
      continue;
    }
    auto before = load_blob(change.preimage_blob, error);
    if (!before || !write_workspace_file(change.path, *before, error))
      return false;
  }
  changes.rejected = true;
  return true;
}

} // namespace tokmon::desk
