#include "lenses/cove/cove_lens.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "lenses/common/process_runner.hpp"
#include "tokmon/hash.hpp"

namespace tokmon::builtin {
namespace {

Result<std::filesystem::path> safe_target(const cbor::Value& parameters,
                                          const std::string_view path_key = "path") {
  const auto* root_field = cbor::find(parameters, "workspace_root");
  const auto* path_field = cbor::find(parameters, path_key);
  if (!root_field || !path_field || root_field->as_string().empty() ||
      path_field->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "workspace_root and path are required"));
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(root_field->as_string()), error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot canonicalize workspace root"));
  const auto relative = std::filesystem::path(path_field->as_string());
  if (relative.is_absolute())
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "workspace path must be relative"));
  const auto target = std::filesystem::weakly_canonical(root / relative, error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot canonicalize target path"));
  auto root_part = root.begin(); auto target_part = target.begin();
  for (; root_part != root.end() && target_part != target.end(); ++root_part, ++target_part)
    if (*root_part != *target_part)
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "workspace path escapes the allowed root"));
  if (root_part != root.end())
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "workspace path escapes the allowed root"));
  return target;
}

Result<std::string> read_all(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot read workspace file"));
  return std::string((std::istreambuf_iterator<char>(input)), {});
}

Result<std::string> persist_artifact(const std::filesystem::path& root,
                                     const std::string_view content) {
  const auto digest = sha256_hex(content);
  const auto directory = root / ".tokmon" / "artifacts";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot create artifact directory"));
  const auto path = directory / digest;
  if (!std::filesystem::exists(path)) {
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot persist content artifact"));
  }
  return path.generic_string();
}

struct EntityState {
  std::string type;
  std::uintmax_t size{0};
  std::filesystem::file_time_type modified{};
  std::string hash;
};

std::int64_t modified_millis(const std::filesystem::file_time_type value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      value.time_since_epoch()).count();
}

Result<std::map<std::string, EntityState, std::less<>>> scan_tree(
    const std::filesystem::path& root, const std::size_t max_entries) {
  std::map<std::string, EntityState, std::less<>> result;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root,
           std::filesystem::directory_options::skip_permission_denied, error), end;
       iterator != end; iterator.increment(error)) {
    if (error) { error.clear(); continue; }
    if (iterator->is_directory() && iterator->path().filename() == ".git") {
      iterator.disable_recursion_pending(); continue;
    }
    if (result.size() >= max_entries)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "workspace scan entry limit exceeded"));
    const auto relative = iterator->path().lexically_relative(root).generic_string();
    if (iterator->is_symlink(error)) {
      const auto target = std::filesystem::read_symlink(iterator->path(), error);
      if (!error) result.emplace(relative, EntityState{"symlink", 0,
          iterator->last_write_time(error), sha256_hex(target.generic_string())});
      error.clear();
    } else if (iterator->is_directory(error)) {
      result.emplace(relative, EntityState{"directory", 0,
          iterator->last_write_time(error), sha256_hex("directory:" + relative)});
    } else if (iterator->is_regular_file(error)) {
      auto content = read_all(iterator->path());
      if (!content) continue;
      result.emplace(relative, EntityState{"file", iterator->file_size(error),
          iterator->last_write_time(error), sha256_hex(*content)});
    }
  }
  return result;
}

Result<std::map<std::string, std::string, std::less<>>> git_path_states(
    const std::filesystem::path& root, const std::chrono::milliseconds timeout,
    const std::stop_token stop) {
  std::map<std::string, std::string, std::less<>> states;
  std::error_code error;
  if (!std::filesystem::is_directory(root / ".git", error) || error) return states;
  auto status = run_process(ProcessRequest{
      .argv = {"git", "status", "--porcelain=v1", "-z", "--ignored=matching",
               "--untracked-files=all"},
      .cwd = root, .timeout = timeout, .max_output_bytes = 4u * 1024u * 1024u,
      .stop = stop});
  if (!status) return tl::unexpected(status.error());
  if (status->exit_code != 0)
    return tl::unexpected(make_error(ErrorCode::io_error,
        "git status failed while building workspace tree: " + status->stderr_text));
  std::size_t cursor = 0;
  while (cursor < status->stdout_text.size()) {
    const auto end = status->stdout_text.find('\0', cursor);
    const auto record = std::string_view(status->stdout_text).substr(cursor,
        (end == std::string::npos ? status->stdout_text.size() : end) - cursor);
    if (record.size() >= 4u && record[2] == ' ') {
      const auto code = std::string(record.substr(0, 2));
      states[std::string(record.substr(3))] = code == "??" ? "untracked" :
          code == "!!" ? "ignored" : code;
    }
    if (end == std::string::npos) break;
    cursor = end + 1u;
  }
  return states;
}

bool valid_git_name(const std::string_view value) {
  return !value.empty() && value != "." && value != ".." &&
      std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '-' || character == '_' ||
               character == '.' || character == '/';
      }) && !value.starts_with('/') && !value.ends_with('/') &&
      value.find("..") == std::string_view::npos;
}

Result<ProcessOutput> run_git(const cbor::Value& parameters,
                              std::vector<std::string> arguments,
                              const Act& act, RefractionBeam& beam) {
  const auto* root_field = cbor::find(parameters, "workspace_root");
  if (!root_field || root_field->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "git action requires workspace_root"));
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(root_field->as_string()), error);
  if (error || !std::filesystem::is_directory(root / ".git"))
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "workspace_root is not a Git worktree"));
  arguments.insert(arguments.begin(), "git");
  return run_process(arguments, root, act.timeout, 512u * 1024u, beam.stop_token());
}

}  // namespace

CoveLens::CoveLens() : LensBase(make_manifest("cove", "Cove / Workspace 实景物镜",
    {"workspace.tree", "workspace.diff", "workspace.git", "ui.artifact"},
    {{"workspace.*", "*"}, {"fs.*", "*"}, {"git.*", "*"}, {"artifact.*", "*"}},
    {{"fs.read", "tokmon.fs.read.v1"}, {"fs.create", "tokmon.fs.create.v1"},
     {"fs.write", "tokmon.fs.write.v1"},
     {"fs.move", "tokmon.fs.move.v1"}, {"fs.delete", "tokmon.fs.delete.v1"},
     {"workspace.scan", "tokmon.workspace.scan.v1"},
     {"workspace.watch", "tokmon.workspace.watch.v1"},
     {"git.status", "tokmon.git.status.v1"},
     {"git.stage", "tokmon.git.stage.v1"}, {"git.commit", "tokmon.git.commit.v1"},
     {"git.branch", "tokmon.git.branch.v1"}, {"git.merge", "tokmon.git.merge.v1"},
     {"git.rebase", "tokmon.git.rebase.v1"},
     {"artifact.create", "tokmon.artifact.create.v1"},
     {"artifact.preview", "tokmon.artifact.preview.v1"},
     {"artifact.export", "tokmon.artifact.export.v1"}},
    {"photon.emit", "io.workspace", "artifact.write", "log.write"})) {}

Result<void> CoveLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* changed = photons.latest("fs.changed");
  const auto* git = photons.latest("git.status-observed");
  if (changed) {
    if (auto result = surface.add("workspace.diff", changed->id, changed->payload, 30);
        !result) return result;
  }
  if (auto result = identify(surface, "workspace.tree", cbor::object({
      {"last_change", changed ? changed->payload : cbor::Value(nullptr)},
      {"git_available", git != nullptr}, {"canonical_paths", true}})); !result)
    return result;
  if (auto result = surface.add("workspace.git", "status",
      git ? git->payload : cbor::object({{"available", false}}), 10); !result)
    return result;
  const auto* artifact = photons.latest("artifact.created");
  return surface.add("ui.artifact", "latest",
      artifact ? artifact->payload : cbor::object({{"available", false}}), 10);
}

Result<RefractionResult> CoveLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "workspace.scan" || act.kind == "workspace.watch") {
    const auto* root_value = cbor::find(act.parameters, "workspace_root");
    if (!root_value || root_value->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "workspace operation requires workspace_root"));
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::path(root_value->as_string()), error);
    if (error || !std::filesystem::is_directory(root))
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "workspace_root is not a directory"));
    const auto max_entries = static_cast<std::size_t>(std::clamp<std::int64_t>(
        cbor::find(act.parameters, "max_entries")
            ? cbor::find(act.parameters, "max_entries")->as_integer(100'000) : 100'000,
        1, 1'000'000));
    auto snapshot = scan_tree(root, max_entries);
    if (!snapshot) return tl::unexpected(snapshot.error());
    auto git_states = git_path_states(root, act.timeout, beam.stop_token());
    if (!git_states) return tl::unexpected(git_states.error());
    cbor::Value::Array entities;
    for (const auto& [path, state] : *snapshot)
      entities.push_back(cbor::object({{"path", path},
          {"canonical_path", (root / path).lexically_normal().generic_string()},
          {"type", state.type}, {"size", static_cast<std::int64_t>(state.size)},
          {"mtime_ms", modified_millis(state.modified)}, {"sha256", state.hash},
          {"git_status", git_states->contains(path) ? (*git_states)[path] : "clean"},
          {"ignored", git_states->contains(path) && (*git_states)[path] == "ignored"}}));
    if (act.kind == "workspace.scan") {
      const auto snapshot_hash = sha256_hex(cbor::encode(cbor::Value(entities)));
      return emit(beam, "workspace.scanned", "tokmon.workspace.scan-result.v1",
          cbor::object({{"root", root.generic_string()}, {"entities", std::move(entities)},
            {"count", static_cast<std::int64_t>(snapshot->size())},
            {"snapshot_hash", snapshot_hash}}));
    }

    std::vector<PhotonId> emitted;
    auto started = beam.emit("watcher.started", "tokmon.workspace.watcher.v1",
        cbor::object({{"root", root.generic_string()},
                      {"baseline_count", static_cast<std::int64_t>(snapshot->size())}}));
    if (!started) return tl::unexpected(started.error());
    emitted.push_back(started->id);
    const auto duration = std::chrono::milliseconds(std::clamp<std::int64_t>(
        cbor::find(act.parameters, "duration_ms")
            ? cbor::find(act.parameters, "duration_ms")->as_integer(1000) : 1000,
        50, std::min<std::int64_t>(act.timeout.count(), 60'000)));
    const auto debounce = std::chrono::milliseconds(std::clamp<std::int64_t>(
        cbor::find(act.parameters, "debounce_ms")
            ? cbor::find(act.parameters, "debounce_ms")->as_integer(100) : 100, 10, 2000));
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!beam.stop_requested() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(debounce);
      auto next = scan_tree(root, max_entries);
      if (!next) return tl::unexpected(next.error());
      cbor::Value::Array changes;
      for (const auto& [path, state] : *next) {
        const auto found = snapshot->find(path);
        if (found == snapshot->end() || found->second.hash != state.hash)
          changes.push_back(cbor::object({{"path", path},
              {"operation", found == snapshot->end() ? "create" : "modify"},
              {"type", state.type}, {"mtime_ms", modified_millis(state.modified)},
              {"sha256", state.hash}, {"size", static_cast<std::int64_t>(state.size)}}));
      }
      for (const auto& [path, _] : *snapshot)
        if (!next->contains(path))
          changes.push_back(cbor::object({{"path", path}, {"operation", "delete"}}));
      if (changes.size() > 1024u) {
        auto photon = beam.emit("watcher.overflowed", "tokmon.workspace.watcher.v1",
            cbor::object({{"change_count", static_cast<std::int64_t>(changes.size())},
                          {"rescan", true}}));
        if (!photon) return tl::unexpected(photon.error());
        emitted.push_back(photon->id);
      } else if (!changes.empty()) {
        auto photon = beam.emit("workspace.changes-observed", "tokmon.workspace.changes.v1",
            cbor::object({{"root", root.generic_string()},
                          {"changes", std::move(changes)}, {"reobserved", true}}));
        if (!photon) return tl::unexpected(photon.error());
        emitted.push_back(photon->id);
      }
      snapshot = std::move(next);
    }
    auto stopped = beam.emit("watcher.stopped", "tokmon.workspace.watcher.v1",
        cbor::object({{"cancelled", beam.stop_requested()},
                      {"final_count", static_cast<std::int64_t>(snapshot->size())}}));
    if (!stopped) return tl::unexpected(stopped.error());
    emitted.push_back(stopped->id);
    return RefractionResult{.status = beam.stop_requested() ? RefractionStatus::rejected
                                                            : RefractionStatus::completed,
                             .emitted = std::move(emitted), .detail = "workspace watch ended"};
  }
  if (act.kind.starts_with("fs.")) {
    auto target = safe_target(act.parameters);
    if (!target) return tl::unexpected(target.error());
    if (act.kind == "fs.read") {
      auto full_content = read_all(*target);
      if (!full_content) return tl::unexpected(full_content.error());
      const auto digest = sha256_hex(*full_content);
      const bool truncated = full_content->size() > 65'536;
      if (truncated) full_content->resize(65'536);
      return emit(beam, "fs.read-completed", "tokmon.fs.result.v1", cbor::object({
          {"path", target->generic_string()}, {"content", *full_content},
          {"sha256", digest}, {"truncated", truncated}}));
    }
    if (act.kind == "fs.write" || act.kind == "fs.create") {
      const auto* content = cbor::find(act.parameters, "content");
      if (!content || !std::holds_alternative<std::string>(content->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "fs.write string content is required"));
      if (act.kind == "fs.create" && std::filesystem::exists(*target))
        return tl::unexpected(make_error(ErrorCode::invalid_state,
                                         "fs.create target already exists"));
      std::string preimage;
      std::string current_hash;
      if (std::filesystem::exists(*target)) {
        auto current = read_all(*target);
        if (!current) return tl::unexpected(current.error());
        preimage = std::move(*current);
        current_hash = sha256_hex(preimage);
      }
      if (const auto* precondition = cbor::find(act.parameters, "precondition_sha256")) {
        if (current_hash != precondition->as_string())
          return tl::unexpected(make_error(ErrorCode::invalid_state,
                                           "workspace precondition hash does not match"));
      }
      std::error_code error;
      std::filesystem::create_directories(target->parent_path(), error);
      if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot create target directory"));
      std::ofstream output(*target, std::ios::binary | std::ios::trunc);
      const auto value = std::string(content->as_string());
      output.write(value.data(), static_cast<std::streamsize>(value.size())); output.close();
      if (!output) return tl::unexpected(make_error(ErrorCode::io_error,
                                                    "cannot write workspace file"));
      auto read_back = read_all(*target);
      if (!read_back) return tl::unexpected(read_back.error());
      const auto root = std::filesystem::weakly_canonical(
          std::filesystem::path(cbor::find(act.parameters, "workspace_root")->as_string()), error);
      auto preimage_ref = persist_artifact(root, preimage);
      auto postimage_ref = persist_artifact(root, *read_back);
      if (!preimage_ref) return tl::unexpected(preimage_ref.error());
      if (!postimage_ref) return tl::unexpected(postimage_ref.error());
      const bool diff_truncated = preimage.size() + read_back->size() > 131'072u;
      return emit(beam, "fs.changed", "tokmon.fs.changed.v1", cbor::object({
          {"path", target->generic_string()},
          {"operation", act.kind == "fs.create" ? "create" : "write"},
          {"preimage_sha256", current_hash}, {"preimage_ref", *preimage_ref},
          {"postimage_sha256", sha256_hex(*read_back)}, {"postimage_ref", *postimage_ref},
          {"diff", diff_truncated ? cbor::Value(nullptr) : cbor::object({
              {"before", preimage}, {"after", *read_back}})},
          {"diff_truncated", diff_truncated},
          {"bytes", static_cast<std::int64_t>(read_back->size())},
          {"write_verified", true}}));
    }
    if (act.kind == "fs.move") {
      auto destination = safe_target(act.parameters, "destination");
      if (!destination) return tl::unexpected(destination.error());
      if (std::filesystem::exists(*destination))
        return tl::unexpected(make_error(ErrorCode::invalid_state,
                                         "fs.move destination already exists"));
      std::error_code error;
      std::filesystem::create_directories(destination->parent_path(), error);
      if (!error) std::filesystem::rename(*target, *destination, error);
      if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot move workspace path: " + error.message()));
      return emit(beam, "fs.changed", "tokmon.fs.changed.v1", cbor::object({
          {"path", target->generic_string()}, {"destination", destination->generic_string()},
          {"operation", "move"}}));
    }
    if (!std::filesystem::exists(*target))
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "fs.delete target does not exist"));
    std::error_code error;
    const bool directory = std::filesystem::is_directory(*target, error);
    if (directory && (!cbor::find(act.parameters, "recursive") ||
                      !cbor::find(act.parameters, "recursive")->as_bool()))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "directory deletion requires recursive=true"));
    std::string preimage_hash;
    std::string preimage_ref;
    if (!directory) {
      auto content = read_all(*target);
      if (!content) return tl::unexpected(content.error());
      preimage_hash = sha256_hex(*content);
      const auto root = std::filesystem::weakly_canonical(
          std::filesystem::path(cbor::find(act.parameters, "workspace_root")->as_string()), error);
      auto stored = persist_artifact(root, *content);
      if (!stored) return tl::unexpected(stored.error());
      preimage_ref = std::move(*stored);
    }
    const auto removed = directory ? std::filesystem::remove_all(*target, error)
                                   : (std::filesystem::remove(*target, error) ? 1u : 0u);
    if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "cannot delete workspace path: " + error.message()));
    return emit(beam, "fs.changed", "tokmon.fs.changed.v1", cbor::object({
        {"path", target->generic_string()}, {"operation", "delete"},
        {"preimage_sha256", preimage_hash}, {"preimage_ref", preimage_ref},
        {"removed_entries", static_cast<std::int64_t>(removed)}, {"verified_absent", true}}));
  }
  if (act.kind.starts_with("artifact.")) {
    const auto* root = cbor::find(act.parameters, "workspace_root");
    if (!root || root->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "artifact operation requires workspace_root"));
    const auto directory = std::filesystem::path(root->as_string()) / ".tokmon" / "artifacts";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "cannot create artifact directory"));
    if (act.kind == "artifact.create") {
      const auto* content = cbor::find(act.parameters, "content");
      if (!content || !std::holds_alternative<std::string>(content->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "artifact.create requires string content"));
      const auto value = std::string(content->as_string());
      const auto digest = sha256_hex(value);
      auto path = persist_artifact(std::filesystem::path(root->as_string()), value);
      if (!path) return tl::unexpected(path.error());
      return emit(beam, "artifact.created", "tokmon.artifact.created.v1", cbor::object({
          {"sha256", digest}, {"path", *path},
          {"bytes", static_cast<std::int64_t>(value.size())},
          {"provenance_act", act.id}, {"immutable", true}}));
    }
    const auto digest = cbor::find(act.parameters, "sha256");
    if (!digest || digest->as_string().size() != 64u)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "artifact preview/export requires sha256"));
    const auto path = directory / std::string(digest->as_string());
    auto content = read_all(path);
    if (!content || sha256_hex(*content) != digest->as_string())
      return tl::unexpected(content ? make_error(ErrorCode::integrity_error,
          "artifact content hash mismatch") : content.error());
    if (act.kind == "artifact.preview") {
      const auto max_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
          cbor::find(act.parameters, "max_bytes")
              ? cbor::find(act.parameters, "max_bytes")->as_integer(65'536) : 65'536,
          1, 1'048'576));
      const bool truncated = content->size() > max_bytes;
      if (truncated) content->resize(max_bytes);
      return emit(beam, "artifact.previewed", "tokmon.artifact.preview.v1",
          cbor::object({{"sha256", std::string(digest->as_string())}, {"content", *content},
                        {"truncated", truncated}}));
    }
    auto destination = safe_target(act.parameters, "destination");
    if (!destination) return tl::unexpected(destination.error());
    if (std::filesystem::exists(*destination))
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "artifact export destination exists"));
    std::filesystem::create_directories(destination->parent_path(), error);
    std::filesystem::copy_file(path, *destination, std::filesystem::copy_options::none, error);
    if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "artifact export failed: " + error.message()));
    return emit(beam, "artifact.exported", "tokmon.artifact.export.v1", cbor::object({
        {"sha256", std::string(digest->as_string())},
        {"destination", destination->generic_string()}, {"verified", true}}));
  }

  std::vector<std::string> git_arguments;
  const auto* git_root = cbor::find(act.parameters, "workspace_root");
  if (!git_root || git_root->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Git action requires workspace_root"));
  if (act.kind == "git.status") {
    git_arguments = {"status", "--porcelain=v2", "--branch", "--untracked-files=all"};
  } else if (act.kind == "git.stage") {
    git_arguments = {"add", "--"};
    const auto* paths = cbor::find(act.parameters, "paths");
    if (!paths || !paths->as_array() || paths->as_array()->empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "git.stage requires paths"));
    for (const auto& path : *paths->as_array()) {
      if (!std::holds_alternative<std::string>(path.data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "git.stage path must be a string"));
      auto checked = safe_target(cbor::object({
          {"workspace_root", *git_root},
          {"path", path}}));
      if (!checked) return tl::unexpected(checked.error());
      git_arguments.emplace_back(path.as_string());
    }
  } else if (act.kind == "git.commit") {
    const auto* message = cbor::find(act.parameters, "message");
    if (!message || message->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "git.commit requires message"));
    git_arguments = {"commit", "-m", std::string(message->as_string())};
  } else {
    const auto* name = cbor::find(act.parameters, "name");
    if (!name || !valid_git_name(name->as_string()))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Git branch name is invalid"));
    git_arguments = act.kind == "git.branch"
        ? std::vector<std::string>{"switch", "-c", std::string(name->as_string())}
        : act.kind == "git.rebase"
            ? std::vector<std::string>{"rebase", std::string(name->as_string())}
            : std::vector<std::string>{"merge", "--no-edit", std::string(name->as_string())};
  }
  auto result = run_git(act.parameters, std::move(git_arguments), act, beam);
  if (!result) return tl::unexpected(result.error());
  if (result->timed_out) return tl::unexpected(make_error(ErrorCode::timeout,
                                                          "Git action timed out"));
  if (result->cancelled) return tl::unexpected(make_error(ErrorCode::cancelled,
                                                          "Git action cancelled"));
  const auto event = act.kind == "git.status" ? "git.status-observed" :
      result->exit_code == 0 ? "git.completed" : "git.failed";
  return emit(beam, event,
      "tokmon.git.result.v1", cbor::object({
          {"operation", act.kind}, {"exit_code", result->exit_code},
          {"stdout", result->stdout_text}, {"stderr", result->stderr_text},
          {"conflict", result->exit_code != 0 &&
              (act.kind == "git.merge" || act.kind == "git.rebase")},
          {"automatic_overwrite", false},
          {"output_truncated", result->stdout_truncated || result->stderr_truncated}}));
}

}  // namespace tokmon::builtin
