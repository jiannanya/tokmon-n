#include "tokmon/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "tokmon/c_abi_loader.hpp"
#include "tokmon/builtin_lens.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"
#include "tokmon/manifest_io.hpp"
#include "tokmon/worker_lens_proxy.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tokmon {
namespace {

std::string short_id(const LensId& id) {
  constexpr std::string_view prefix = "org.tokmon.lens.";
  return id.starts_with(prefix) ? id.substr(prefix.size()) : id;
}

Result<std::filesystem::path> current_executable() {
#if defined(_WIN32)
  std::wstring buffer(1024, L'\0');
  for (;;) {
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0)
      return tl::unexpected(make_error(ErrorCode::io_error,
                                       "cannot locate current executable"));
    if (length < buffer.size() - 1u) {
      buffer.resize(length);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2u);
  }
#else
  std::vector<char> buffer(1024);
  for (;;) {
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0)
      return tl::unexpected(make_error(ErrorCode::io_error,
                                       "cannot locate current executable"));
    if (static_cast<std::size_t>(length) < buffer.size())
      return std::filesystem::path(std::string(buffer.data(),
                                               static_cast<std::size_t>(length)));
    buffer.resize(buffer.size() * 2u);
  }
#endif
}

Result<std::string> artifact_hash(const DesiredLens& desired) {
  if (desired.artifact.starts_with("builtin:"))
    return sha256_hex(desired.artifact + ":0.1.0");

  const std::filesystem::path artifact(desired.artifact);
  std::error_code error;
  if (!std::filesystem::exists(artifact, error) || error)
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "Lens artifact does not exist: " + artifact.string()));

  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_directory(artifact, error)) {
    for (std::filesystem::recursive_directory_iterator iterator(
             artifact, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error)) {
      if (error)
        return tl::unexpected(make_error(ErrorCode::io_error,
            "cannot enumerate Lens artifact: " + error.message()));
      if (iterator->is_regular_file(error) && !error) files.push_back(iterator->path());
    }
  } else {
    files.push_back(artifact);
    const auto manifest = artifact.parent_path() / "lens.yaml";
    if (std::filesystem::is_regular_file(manifest, error) && !error)
      files.push_back(manifest);
  }
  std::ranges::sort(files, [&](const auto& left, const auto& right) {
    return left.lexically_relative(artifact.parent_path()).generic_string() <
           right.lexically_relative(artifact.parent_path()).generic_string();
  });
  std::vector<std::uint8_t> material;
  for (const auto& file : files) {
    const auto relative = file.lexically_relative(
        std::filesystem::is_directory(artifact) ? artifact : artifact.parent_path())
                              .generic_string();
    material.insert(material.end(), relative.begin(), relative.end());
    material.push_back(0);
    std::ifstream input(file, std::ios::binary);
    if (!input)
      return tl::unexpected(make_error(ErrorCode::io_error,
                                       "cannot read Lens artifact file: " + file.string()));
    material.insert(material.end(), std::istreambuf_iterator<char>(input), {});
    material.push_back(0);
  }
  if (files.empty())
    return tl::unexpected(make_error(ErrorCode::integrity_error,
                                     "Lens artifact contains no files"));
  return sha256_hex(material);
}

Result<std::filesystem::path> first_existing(
    const std::vector<std::filesystem::path>& candidates,
    const std::string_view description) {
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error) && !error)
      return std::filesystem::absolute(candidate);
    error.clear();
  }
  return tl::unexpected(make_error(ErrorCode::not_found,
      std::string(description) + " was not found in the immutable Tokmon layout"));
}

Result<std::filesystem::path> worker_supervisor() {
  auto executable = current_executable();
  if (!executable) return tl::unexpected(executable.error());
#if defined(_WIN32)
  constexpr auto name = "tokmon-lens-worker.exe";
#else
  constexpr auto name = "tokmon-lens-worker";
#endif
  return first_existing({executable->parent_path() / name,
                         executable->parent_path().parent_path() / "bin" / name,
#if defined(TOKMON_BUILD_BIN_DIR)
                         std::filesystem::path(TOKMON_BUILD_BIN_DIR) / name,
#endif
                        }, "tokmon-lens-worker");
}

Result<std::filesystem::path> language_runtime(const RuntimeConfig& config,
                                                const RuntimeKind kind) {
  const auto root = config.paths.runtimes /
      (kind == RuntimeKind::node ? "node" : "cpython");
#if defined(_WIN32)
  const auto executable = kind == RuntimeKind::node ? "node.exe" : "python.exe";
  return first_existing({root / executable, root / "bin" / executable}, executable);
#else
  if (kind == RuntimeKind::node)
    return first_existing({root / "bin" / "node", root / "node"}, "Node.js runtime");
  return first_existing({root / "bin" / "python3", root / "bin" / "python",
                         root / "python3"}, "CPython runtime");
#endif
}

Result<std::filesystem::path> language_adapter(const RuntimeKind kind) {
  auto executable = current_executable();
  if (!executable) return tl::unexpected(executable.error());
  const auto relative = kind == RuntimeKind::node
      ? std::filesystem::path("typescript") / "worker.mjs"
      : std::filesystem::path("python") / "tokmon_lens_sdk" / "worker.py";
  std::vector<std::filesystem::path> candidates{
      executable->parent_path().parent_path() / "share" / "tokmon" / "sdk" / relative};
#if defined(TOKMON_SOURCE_DIR)
  candidates.push_back(std::filesystem::path(TOKMON_SOURCE_DIR) / "sdk" / relative);
#endif
  return first_existing(candidates, "language Lens adapter");
}

bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

Result<std::shared_ptr<ILens>> stage_lens(const DesiredLens& desired,
                                          const RuntimeConfig& config) {
  if (desired.artifact.starts_with("builtin:")) {
    auto lens = make_builtin_lens(short_id(desired.id));
    if (!lens)
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "unknown built-in Lens: " + desired.id));
    return lens;
  }

  const std::filesystem::path artifact(desired.artifact);
  std::error_code error;
  const auto artifact_is_directory = std::filesystem::is_directory(artifact, error);
  if (error)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot inspect Lens artifact: " + error.message()));
  const auto root = artifact_is_directory ? artifact : artifact.parent_path();
  const auto manifest_path = root / "lens.yaml";
  auto manifest = load_lens_manifest(manifest_path);
  if (!manifest) return tl::unexpected(manifest.error());
  if (manifest->id != desired.id)
    return tl::unexpected(make_error(ErrorCode::integrity_error,
        "artifact manifest id does not match desired Lens: " + desired.id));
  if (manifest->runtime != desired.runtime)
    return tl::unexpected(make_error(ErrorCode::integrity_error,
        "artifact runtime does not match desired Lens: " + desired.id));

  if (desired.runtime == RuntimeKind::node ||
      desired.runtime == RuntimeKind::cpython) {
    auto entry = (root / manifest->runtime_entry).lexically_normal();
    if (!inside(root.lexically_normal(), entry))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "language Lens entry escapes its artifact"));
    if (!std::filesystem::is_regular_file(entry, error) || error)
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "language Lens entry was not found: " + entry.string()));
    auto supervisor = worker_supervisor();
    auto runtime = language_runtime(config, desired.runtime);
    auto adapter = language_adapter(desired.runtime);
    if (!supervisor) return tl::unexpected(supervisor.error());
    if (!runtime) return tl::unexpected(runtime.error());
    if (!adapter) return tl::unexpected(adapter.error());
    auto proxy = WorkerLensProxy::launch(WorkerLensOptions{
        .manifest = std::move(*manifest), .supervisor = std::move(*supervisor),
        .runtime_executable = std::move(*runtime), .adapter = std::move(*adapter),
        .entry = std::move(entry)});
    if (!proxy) return tl::unexpected(proxy.error());
    return std::static_pointer_cast<ILens>(*proxy);
  }

  if (desired.runtime == RuntimeKind::in_process) {
    auto entry = artifact;
    if (artifact_is_directory) {
      if (manifest->runtime_entry.empty())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "native Lens directory manifest runtime.entry is required"));
      entry = (root / manifest->runtime_entry).lexically_normal();
    }
    auto loaded = CAbiLens::load(entry);
    if (!loaded) return tl::unexpected(loaded.error());
    return std::static_pointer_cast<ILens>(*loaded);
  }

  return tl::unexpected(make_error(ErrorCode::unsupported,
      "external Lens runtime is not implemented: " +
      std::string(to_string(desired.runtime))));
}

std::string path_hash(const LightPathSnapshot& path) {
  std::string material = std::to_string(path.epoch);
  for (const auto& mounted : path.lenses) {
    material.append("\n").append(mounted.lens->manifest().id)
        .append("@").append(mounted.lens->manifest().version)
        .append("#").append(std::to_string(mounted.generation))
        .append(":").append(mounted.artifact_hash);
  }
  return sha256_hex(material);
}

}  // namespace

TokmonRuntime::TokmonRuntime() = default;
TokmonRuntime::~TokmonRuntime() { stop(); }

Result<void> TokmonRuntime::open(const std::optional<std::filesystem::path>& workspace,
                                 const std::string_view process_name) {
  if (open_)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "TokmonRuntime is already open"));
  workspace_ = workspace;
  auto config = load_config(workspace);
  if (!config) return tl::unexpected(config.error());
  config_ = std::move(*config);
  if (auto result = ensure_directory_layout(config_.paths); !result) return result;
  if (auto result = initialize_logging(config_.paths.logs, process_name, config_.log_level); !result)
    return result;
  if (auto result = store_.open(config_.paths.database); !result) return result;
  engine_ = std::make_unique<RayTracingEngine>(store_, path_, beams_);
  engine_->set_approval([](const Act&) { return false; });
  open_ = true;
  if (auto result = reconcile(); !result) { open_ = false; return result; }
  auto started = store_.append(PhotonDraft{.ray = make_id("system-ray"),
      .kind = "system.started", .schema = "tokmon.system.started.v1",
      .payload = cbor::object({{"version", "0.1.0"},
          {"light_path_hash", path_.snapshot()->hash}}), .epoch = path_.snapshot()->epoch});
  if (!started) return tl::unexpected(started.error());
  spdlog::info("Tokmon runtime ready epoch={} lenses={}", path_.snapshot()->epoch,
               path_.snapshot()->lenses.size());
  return {};
}

Result<void> TokmonRuntime::reconcile() {
  auto refreshed = load_config(workspace_);
  if (!refreshed) return tl::unexpected(refreshed.error());
  config_ = std::move(*refreshed);
  const auto current = path_.snapshot();
  auto candidate = std::make_shared<LightPathSnapshot>();
  candidate->epoch = current->epoch + 1u;
  const auto control_ray = make_id("mount-ray");
  cbor::Value::Array desired_entries;
  for (const auto& desired : config_.light_path) {
    desired_entries.push_back(cbor::object({
        {"id", desired.id}, {"artifact", desired.artifact},
        {"enabled", desired.enabled},
        {"runtime", std::string(to_string(desired.runtime))}}));
  }
  auto observed = store_.append(PhotonDraft{.ray = control_ray,
      .kind = "config.light-path-observed",
      .schema = "tokmon.config.light-path.v1",
      .payload = cbor::object({
          {"candidate_epoch", static_cast<std::int64_t>(candidate->epoch)},
          {"lenses", std::move(desired_entries)}}),
      .epoch = current->epoch});
  if (!observed) return tl::unexpected(observed.error());

  const auto rejected = [&](const LensId& lens_id, const Error& cause) -> Error {
    auto photon = store_.append(PhotonDraft{.ray = control_ray,
        .kind = "lens.candidate-rejected", .schema = "tokmon.lens.candidate.v1",
        .payload = cbor::object({
            {"lens_id", lens_id}, {"candidate_epoch",
             static_cast<std::int64_t>(candidate->epoch)},
            {"error_code", std::string(to_string(cause.code))},
            {"error", cause.describe()}}), .epoch = current->epoch});
    return photon ? cause : photon.error();
  };

  GenerationId generation = candidate->epoch * 1000u;
  for (const auto& desired : config_.light_path) {
    if (!desired.enabled) continue;
    auto staged = stage_lens(desired, config_);
    if (!staged) return tl::unexpected(rejected(desired.id, staged.error()));
    auto lens = std::move(*staged);
    if (lens->manifest().id != desired.id)
      return tl::unexpected(rejected(desired.id, make_error(ErrorCode::integrity_error,
          "artifact manifest id does not match desired Lens: " + desired.id)));
    if (lens->manifest().runtime != desired.runtime)
      return tl::unexpected(rejected(desired.id, make_error(ErrorCode::integrity_error,
          "artifact runtime does not match desired Lens: " + desired.id)));
    SurfaceBuilder dark_surface(lens->manifest().id);
    auto dark_result = lens->view(PhotonWindow{}, dark_surface);
    if (!dark_result)
      return tl::unexpected(rejected(desired.id, make_error(ErrorCode::invalid_state,
          "dark-lane view failed for " + desired.id + ": " +
          dark_result.error().describe())));
    auto hash = artifact_hash(desired);
    if (!hash) return tl::unexpected(rejected(desired.id, hash.error()));
    const auto mounted_generation = generation++;
    auto verified = store_.append(PhotonDraft{.ray = control_ray,
        .kind = "lens.candidate-verified", .schema = "tokmon.lens.candidate.v1",
        .payload = cbor::object({
            {"lens_id", desired.id}, {"artifact_hash", *hash},
            {"runtime", std::string(to_string(desired.runtime))},
            {"generation", static_cast<std::int64_t>(mounted_generation)},
            {"candidate_epoch", static_cast<std::int64_t>(candidate->epoch)},
            {"dark_surface_contributions",
             static_cast<std::int64_t>(dark_surface.contributions().size())}}),
        .epoch = current->epoch});
    if (!verified) return tl::unexpected(verified.error());
    candidate->lenses.push_back(MountedLens{
        std::move(lens), mounted_generation, std::move(*hash)});
  }
  const auto has_calculator = std::ranges::any_of(candidate->lenses,
      [](const MountedLens& mounted) {
        return mounted.lens->manifest().id == "org.tokmon.lens.calculator";
      });
  if (!has_calculator)
    candidate->lenses.push_back(MountedLens{make_builtin_lens("calculator"),
                                            generation++, sha256_hex("builtin:calculator:0.1.0")});
  candidate->hash = path_hash(*candidate);

  // Validate the complete graph before recording the durable epoch decision.
  LightPath dark_path;
  if (auto checked = dark_path.publish(candidate); !checked)
    return tl::unexpected(checked.error());

  auto committed = store_.append(PhotonDraft{.ray = control_ray,
      .kind = "mount.epoch-committed", .schema = "tokmon.mount.epoch.v1",
      .payload = cbor::object({{"epoch", static_cast<std::int64_t>(candidate->epoch)},
          {"path_hash", candidate->hash},
          {"lens_count", static_cast<std::int64_t>(candidate->lenses.size())}}),
      .epoch = candidate->epoch});
  if (!committed) return tl::unexpected(committed.error());
  if (auto result = path_.publish(candidate); !result) return result;

  for (const auto& old : current->lenses) {
    auto started = store_.append(PhotonDraft{.ray = control_ray,
        .kind = "lens.afterglow-started", .schema = "tokmon.lens.afterglow.v1",
        .payload = cbor::object({
            {"lens_id", old.lens->manifest().id},
            {"generation", static_cast<std::int64_t>(old.generation)},
            {"superseded_by_epoch", static_cast<std::int64_t>(candidate->epoch)}}),
        .epoch = candidate->epoch});
    if (!started) return tl::unexpected(started.error());
    beams_.stop_generation(old.lens->manifest().id, old.generation);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (beams_.active(old.lens->manifest().id, old.generation) != 0 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    old.lens->request_stop();
    auto completed = store_.append(PhotonDraft{.ray = control_ray,
        .kind = "lens.afterglow-completed", .schema = "tokmon.lens.afterglow.v1",
        .payload = cbor::object({
            {"lens_id", old.lens->manifest().id},
            {"generation", static_cast<std::int64_t>(old.generation)},
            {"active_beams", static_cast<std::int64_t>(
                 beams_.active(old.lens->manifest().id, old.generation))}}),
        .epoch = candidate->epoch});
    if (!completed) return tl::unexpected(completed.error());
  }
  return {};
}

Result<RayId> TokmonRuntime::submit(std::string input) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  return engine_->begin(std::move(input));
}
Result<std::size_t> TokmonRuntime::advance(const RayId& ray) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  return engine_->advance(ray, config_.max_beats);
}
Result<std::vector<Photon>> TokmonRuntime::history(const RayId& ray) const {
  return store_.read_ray(ray);
}
Result<std::vector<Photon>> TokmonRuntime::history_all(const std::uint64_t after) const {
  return store_.read_all(after);
}
Result<SurfaceSnapshot> TokmonRuntime::surface(const RayId& ray) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  return engine_->view(ray);
}
Result<void> TokmonRuntime::verify() const { return store_.verify(); }
void TokmonRuntime::stop() noexcept {
  if (engine_) engine_->request_stop();
  engine_.reset();
  open_ = false;
}
const RuntimeConfig& TokmonRuntime::config() const noexcept { return config_; }
std::shared_ptr<const LightPathSnapshot> TokmonRuntime::light_path() const noexcept {
  return path_.snapshot();
}
PhotonStore& TokmonRuntime::store() noexcept { return store_; }

}  // namespace tokmon
