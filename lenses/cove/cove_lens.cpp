#include "lenses/cove/cove_lens.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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
    {{"fs.read", "tokmon.fs.read.v1"}, {"fs.write", "tokmon.fs.write.v1"},
     {"fs.move", "tokmon.fs.move.v1"}, {"fs.delete", "tokmon.fs.delete.v1"},
     {"git.stage", "tokmon.git.stage.v1"}, {"git.commit", "tokmon.git.commit.v1"},
     {"git.branch", "tokmon.git.branch.v1"}, {"git.merge", "tokmon.git.merge.v1"},
     {"artifact.create", "tokmon.artifact.create.v1"}},
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
    if (act.kind == "fs.write") {
      const auto* content = cbor::find(act.parameters, "content");
      if (!content || !std::holds_alternative<std::string>(content->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "fs.write string content is required"));
      if (const auto* precondition = cbor::find(act.parameters, "precondition_sha256")) {
        std::string current_hash;
        if (std::filesystem::exists(*target)) {
          auto current = read_all(*target);
          if (!current) return tl::unexpected(current.error());
          current_hash = sha256_hex(*current);
        }
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
      return emit(beam, "fs.changed", "tokmon.fs.changed.v1", cbor::object({
          {"path", target->generic_string()}, {"operation", "write"},
          {"sha256", sha256_hex(*read_back)},
          {"bytes", static_cast<std::int64_t>(read_back->size())}}));
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
    const auto removed = directory ? std::filesystem::remove_all(*target, error)
                                   : (std::filesystem::remove(*target, error) ? 1u : 0u);
    if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "cannot delete workspace path: " + error.message()));
    return emit(beam, "fs.changed", "tokmon.fs.changed.v1", cbor::object({
        {"path", target->generic_string()}, {"operation", "delete"},
        {"removed_entries", static_cast<std::int64_t>(removed)}}));
  }
  if (act.kind == "artifact.create") {
    const auto* root = cbor::find(act.parameters, "workspace_root");
    const auto* content = cbor::find(act.parameters, "content");
    if (!root || !content || !std::holds_alternative<std::string>(content->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "artifact.create requires workspace_root and string content"));
    const auto value = std::string(content->as_string());
    const auto digest = sha256_hex(value);
    const auto directory = std::filesystem::path(root->as_string()) / ".tokmon" / "artifacts";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "cannot create artifact directory"));
    const auto path = directory / digest;
    if (!std::filesystem::exists(path)) {
      std::ofstream output(path, std::ios::binary);
      output.write(value.data(), static_cast<std::streamsize>(value.size()));
      if (!output) return tl::unexpected(make_error(ErrorCode::io_error,
                                                     "cannot write artifact"));
    }
    return emit(beam, "artifact.created", "tokmon.artifact.created.v1", cbor::object({
        {"sha256", digest}, {"path", path.generic_string()},
        {"bytes", static_cast<std::int64_t>(value.size())}}));
  }

  std::vector<std::string> git_arguments;
  const auto* git_root = cbor::find(act.parameters, "workspace_root");
  if (!git_root || git_root->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Git action requires workspace_root"));
  if (act.kind == "git.stage") {
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
        : std::vector<std::string>{"merge", "--no-edit", std::string(name->as_string())};
  }
  auto result = run_git(act.parameters, std::move(git_arguments), act, beam);
  if (!result) return tl::unexpected(result.error());
  if (result->timed_out) return tl::unexpected(make_error(ErrorCode::timeout,
                                                          "Git action timed out"));
  if (result->cancelled) return tl::unexpected(make_error(ErrorCode::cancelled,
                                                          "Git action cancelled"));
  return emit(beam, result->exit_code == 0 ? "git.completed" : "git.failed",
      "tokmon.git.result.v1", cbor::object({
          {"operation", act.kind}, {"exit_code", result->exit_code},
          {"stdout", result->stdout_text}, {"stderr", result->stderr_text},
          {"output_truncated", result->stdout_truncated || result->stderr_truncated}}));
}

}  // namespace tokmon::builtin
