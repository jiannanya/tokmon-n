#include "tokmon/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "tokmon/c_abi_loader.hpp"
#include "tokmon/builtin_lens.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"
#include "tokmon/manifest_io.hpp"
#include "tokmon/worker_lens_proxy.hpp"
#include "tokmon/yaml.hpp"
#include "lenses/common/secret_store.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tokmon {
namespace {

bool inside(const std::filesystem::path& root, const std::filesystem::path& path);

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
      if (iterator->is_regular_file(error) && !error &&
          iterator->path().filename() != "lens-lock.yaml" &&
          iterator->path().extension() != ".sig")
        files.push_back(iterator->path());
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

Result<std::string> file_hash(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "artifact evidence is missing: " + path.string()));
  std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
  return sha256_hex(bytes);
}

bool secure_equal(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size()) return false;
  unsigned difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index)
    difference |= static_cast<unsigned>(static_cast<unsigned char>(left[index]) ^
                                        static_cast<unsigned char>(right[index]));
  return difference == 0;
}

Result<void> verify_artifact_evidence(const std::filesystem::path& root,
                                      const LensManifest& manifest,
                                      const std::string_view computed_artifact_hash,
                                      const RuntimeConfig& config) {
  const auto lock_path = root / "lens-lock.yaml";
  if (!std::filesystem::is_regular_file(lock_path)) {
    if (config.require_signatures)
      return tl::unexpected(make_error(ErrorCode::integrity_error,
          "signed Lens artifact requires lens-lock.yaml"));
    for (const auto& relative : {manifest.schema_bundle, manifest.sbom}) {
      if (relative.empty()) continue;
      const auto evidence = (root / relative).lexically_normal();
      if (!inside(root.lexically_normal(), evidence) || !std::filesystem::is_regular_file(evidence))
        return tl::unexpected(make_error(ErrorCode::integrity_error,
            "declared Lens evidence is absent or escapes its artifact"));
    }
    return {};
  }
  auto lock = yaml::load(lock_path);
  if (!lock) return tl::unexpected(lock.error());
  if (!lock->is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "lens-lock.yaml must be a map"));
  const std::set<std::string> allowed{"api", "artifact_hash", "runtime_hash",
      "schema_bundle_hash", "sbom_hash", "dependencies", "signature"};
  for (const auto& [key, value] : *lock->as_map()) {
    (void)value;
    if (!allowed.contains(key))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "unknown lens-lock.yaml field: " + key));
  }
  const auto* api = cbor::find(*lock, "api");
  const auto* artifact_hash = cbor::find(*lock, "artifact_hash");
  if (!api || api->as_string() != "tokmon.lens-lock/v1" || !artifact_hash)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "lens-lock.yaml requires api tokmon.lens-lock/v1 and artifact_hash"));
  if (!secure_equal(artifact_hash->as_string(), computed_artifact_hash))
    return tl::unexpected(make_error(ErrorCode::integrity_error,
                                     "Lens artifact hash does not match its lock"));

  std::string runtime_hash;
  if (!manifest.runtime_entry.empty()) {
    auto hashed = file_hash((root / manifest.runtime_entry).lexically_normal());
    if (!hashed) return tl::unexpected(hashed.error());
    runtime_hash = *hashed;
    const auto* locked_runtime = cbor::find(*lock, "runtime_hash");
    if (!locked_runtime || !secure_equal(locked_runtime->as_string(), runtime_hash))
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Lens runtime hash does not match its lock"));
  }
  const auto evidence_hash = [&](const std::string& relative,
                                 const char* field) -> Result<std::string> {
    if (relative.empty()) return std::string{};
    const auto path = (root / relative).lexically_normal();
    if (!inside(root.lexically_normal(), path))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "Lens evidence escapes its artifact"));
    auto hashed = file_hash(path);
    if (!hashed) return tl::unexpected(hashed.error());
    const auto* locked = cbor::find(*lock, field);
    if (!locked || !secure_equal(locked->as_string(), *hashed))
      return tl::unexpected(make_error(ErrorCode::integrity_error,
          std::string(field) + " does not match its lock"));
    return *hashed;
  };
  auto schema_hash = evidence_hash(manifest.schema_bundle, "schema_bundle_hash");
  if (!schema_hash) return tl::unexpected(schema_hash.error());
  auto sbom_hash = evidence_hash(manifest.sbom, "sbom_hash");
  if (!sbom_hash) return tl::unexpected(sbom_hash.error());

  if (const auto* dependencies = cbor::find(*lock, "dependencies")) {
    if (!dependencies->as_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "lens-lock dependencies must be a map"));
    for (const auto& [id, hash] : *dependencies->as_map()) {
      if (id.empty() || hash.as_string().size() != 64u)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            "locked dependency ids and hashes must be exact"));
    }
  }

  if (const auto* signature = cbor::find(*lock, "signature")) {
    const auto* algorithm = cbor::find(*signature, "algorithm");
    const auto* signer_value = cbor::find(*signature, "signer");
    const auto* signature_value = cbor::find(*signature, "value");
    if (!signature->is_map() || !algorithm || !signer_value || !signature_value ||
        algorithm->as_string() != "hmac-sha256")
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "Lens signature must declare hmac-sha256, signer and value"));
    const auto signer = std::string(signer_value->as_string());
    const auto trusted = config.trusted_signers.find(signer);
    if (trusted == config.trusted_signers.end())
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "Lens artifact signer is not trusted"));
    auto key = builtin::keyring_read(trusted->second);
    if (!key) return tl::unexpected(key.error());
    const auto material = std::string(computed_artifact_hash) + "\n" + runtime_hash +
        "\n" + *schema_hash + "\n" + *sbom_hash;
    const auto expected = hmac_sha256_hex(*key, material);
    std::fill(key->begin(), key->end(), '\0');
    if (!secure_equal(signature_value->as_string(), expected))
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Lens artifact signature is invalid"));
  } else if (config.require_signatures) {
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Lens artifact signature is required"));
  }
  return {};
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
#if defined(TOKMON_MONOLITHIC_EXECUTABLE)
  return *executable;
#else
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
#endif
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
  auto locked_hash = artifact_hash(desired);
  if (!locked_hash) return tl::unexpected(locked_hash.error());
  if (auto verified = verify_artifact_evidence(root, *manifest, *locked_hash, config);
      !verified) return tl::unexpected(verified.error());

  if (desired.runtime == RuntimeKind::native_worker) {
    if (manifest->runtime_entry.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "native worker manifest runtime.entry is required"));
    auto entry = (root / manifest->runtime_entry).lexically_normal();
    if (!inside(root.lexically_normal(), entry))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "native worker entry escapes its artifact"));
    if (!std::filesystem::is_regular_file(entry, error) || error)
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "native worker entry was not found: " + entry.string()));
    auto supervisor = worker_supervisor();
    if (!supervisor) return tl::unexpected(supervisor.error());
    auto proxy = WorkerLensProxy::launch(WorkerLensOptions{
        .manifest = std::move(*manifest), .supervisor = std::move(*supervisor),
        .entry = std::move(entry)});
    if (!proxy) return tl::unexpected(proxy.error());
    return std::static_pointer_cast<ILens>(*proxy);
  }

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
    const auto& embedded = (*loaded)->manifest();
    if (embedded.id != manifest->id || embedded.version != manifest->version ||
        embedded.runtime != manifest->runtime ||
        embedded.view_channels != manifest->view_channels ||
        embedded.light_permissions != manifest->light_permissions ||
        embedded.refracts.size() != manifest->refracts.size())
      return tl::unexpected(make_error(ErrorCode::integrity_error,
          "C ABI Lens embedded contract does not match lens.yaml"));
    for (std::size_t index = 0; index < embedded.refracts.size(); ++index)
      if (embedded.refracts[index].kind != manifest->refracts[index].kind ||
          embedded.refracts[index].schema != manifest->refracts[index].schema)
        return tl::unexpected(make_error(ErrorCode::integrity_error,
            "C ABI Lens Act contract does not match lens.yaml"));
    return std::static_pointer_cast<ILens>(*loaded);
  }

  return tl::unexpected(make_error(ErrorCode::unsupported,
      "external Lens runtime is not implemented: " +
      std::string(to_string(desired.runtime))));
}

Result<void> order_light_path(std::vector<MountedLens>& lenses) {
  const auto count = lenses.size();
  std::unordered_map<LensId, std::size_t> index;
  for (std::size_t position = 0; position < count; ++position) {
    if (!lenses[position].lens)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "cannot order a null Lens"));
    index[lenses[position].lens->manifest().id] = position;
  }
  std::vector<std::set<std::size_t>> outgoing(count);
  std::vector<std::size_t> indegree(count, 0);
  const auto edge = [&](const std::size_t from, const std::size_t to) {
    if (from != to && outgoing[from].insert(to).second) ++indegree[to];
  };
  for (std::size_t position = 0; position < count; ++position) {
    const auto& manifest = lenses[position].lens->manifest();
    for (const auto& dependency : manifest.dependencies)
      if (const auto found = index.find(dependency.id); found != index.end())
        edge(found->second, position);
    for (const auto& before : manifest.optical_before)
      if (const auto found = index.find(before); found != index.end()) edge(position, found->second);
    for (const auto& after : manifest.optical_after)
      if (const auto found = index.find(after); found != index.end()) edge(found->second, position);
  }
  std::vector<std::size_t> ready;
  for (std::size_t position = 0; position < count; ++position)
    if (indegree[position] == 0) ready.push_back(position);
  std::vector<MountedLens> ordered;
  ordered.reserve(count);
  while (!ready.empty()) {
    const auto chosen = ready.front();
    ready.erase(ready.begin());
    ordered.push_back(std::move(lenses[chosen]));
    for (const auto target : outgoing[chosen]) {
      if (--indegree[target] == 0) {
        const auto insertion = std::lower_bound(ready.begin(), ready.end(), target);
        ready.insert(insertion, target);
      }
    }
  }
  if (ordered.size() != count)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "Lens dependency/optical-order graph contains a cycle"));
  lenses = std::move(ordered);
  return {};
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

Result<std::size_t> recover_inflight_acts(PhotonStore& store) {
  auto verified = store.verify();
  if (!verified) return tl::unexpected(verified.error());
  auto history = store.read_all();
  if (!history) return tl::unexpected(history.error());
  std::unordered_map<ActId, Photon> started;
  std::unordered_set<ActId> terminal;
  for (const auto& photon : *history) {
    if (photon.caused_by_act.empty()) continue;
    if (photon.kind == "act.started") started[photon.caused_by_act] = photon;
    if (photon.kind == "act.completed" || photon.kind == "act.failed" ||
        photon.kind == "act.rejected" || photon.kind == "act.outcome-unknown")
      terminal.insert(photon.caused_by_act);
  }
  std::size_t recovered = 0;
  for (const auto& [act_id, start] : started) {
    if (terminal.contains(act_id)) continue;
    auto appended = store.append(PhotonDraft{.ray = start.ray,
        .kind = "act.outcome-unknown", .schema = "tokmon.act.recovery.v1",
        .payload = cbor::object({
            {"act_hash", cbor::find(start.payload, "act_hash") ?
                *cbor::find(start.payload, "act_hash") : cbor::Value("")},
            {"started_sequence", static_cast<std::int64_t>(start.sequence)},
            {"reason", "tokmond restarted before observing a terminal result"},
            {"retry_automatically", false}}),
        .epoch = start.epoch, .caused_by_act = act_id});
    if (!appended) return tl::unexpected(appended.error());
    ++recovered;
  }
  return recovered;
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
  auto recovered = recover_inflight_acts(store_);
  if (!recovered) return tl::unexpected(recovered.error());
  if (*recovered != 0) log_warn("marked {} in-flight Acts outcome-unknown", *recovered);
  engine_ = std::make_unique<RayTracingEngine>(store_, path_, beams_);
  engine_->set_admission([this](const Act& act) {
    auto all_photons = store_.read_all();
    if (!all_photons) return AdmissionDecision::deny;
    for (auto iterator = all_photons->rbegin(); iterator != all_photons->rend(); ++iterator) {
      if (iterator->kind != "child.started") continue;
      const auto* child = cbor::find(iterator->payload, "child_ray");
      if (!child || child->as_string() != act.ray) continue;
      const auto* allowed = cbor::find(iterator->payload, "allowed_acts");
      if (!allowed || !allowed->as_array()) return AdmissionDecision::deny;
      const auto permitted = std::any_of(allowed->as_array()->begin(), allowed->as_array()->end(),
          [&act](const cbor::Value& item) {
            const auto pattern = item.as_string();
            return pattern == "*" || pattern == act.kind ||
                (pattern.ends_with('*') && act.kind.starts_with(pattern.substr(0, pattern.size() - 1u)));
          });
      if (!permitted) return AdmissionDecision::deny;
      break;
    }
    auto trust = TrustLevel::t3;
    for (const auto& mounted : path_.snapshot()->lenses)
      if (mounted.lens->manifest().id == act.target && mounted.generation == act.generation) {
        trust = mounted.lens->manifest().trust; break;
      }
    const auto workspace = workspace_ ? std::filesystem::absolute(*workspace_).generic_string() :
                                        std::filesystem::current_path().generic_string();
    const auto policy = evaluate_policy(config_, act, trust, workspace);
    if (policy == PolicyEffect::deny) return AdmissionDecision::deny;
    if (policy == PolicyEffect::allow) return AdmissionDecision::allow;
    auto history = store_.read_ray(act.ray);
    if (!history) return AdmissionDecision::ask;
    const auto expected_hash = act_binding_hash(act);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (auto iterator = history->rbegin(); iterator != history->rend(); ++iterator) {
      if (iterator->kind != "approval.granted" && iterator->kind != "approval.denied")
        continue;
      const auto* hash = cbor::find(iterator->payload, "act_hash");
      const auto* generation = cbor::find(iterator->payload, "target_generation");
      const auto* epoch = cbor::find(iterator->payload, "epoch");
      const auto* deadline = cbor::find(iterator->payload, "deadline_ms");
      const auto scope = cbor::find(iterator->payload, "scope")
          ? cbor::find(iterator->payload, "scope")->as_string() : std::string_view("one_shot");
      const auto exact = hash && hash->as_string() == expected_hash;
      const auto session = scope == "session" &&
          cbor::find(iterator->payload, "act_kind") &&
          cbor::find(iterator->payload, "act_kind")->as_string() == act.kind &&
          cbor::find(iterator->payload, "target") &&
          cbor::find(iterator->payload, "target")->as_string() == act.target;
      if ((!exact && !session) || !generation ||
          generation->as_integer() != static_cast<std::int64_t>(act.generation) ||
          !epoch || epoch->as_integer() != static_cast<std::int64_t>(act.epoch) ||
          (deadline && deadline->as_integer() > 0 && deadline->as_integer() < now))
        continue;
      if (scope != "session" && iterator->kind == "approval.granted") {
        const auto consumed = std::any_of(history->begin(), history->end(),
            [&](const Photon& photon) {
              if (photon.sequence <= iterator->sequence || photon.kind != "act.admitted")
                return false;
              const auto* admitted_hash = cbor::find(photon.payload, "act_hash");
              return admitted_hash && admitted_hash->as_string() == expected_hash;
            });
        if (consumed) return AdmissionDecision::ask;
      }
      return iterator->kind == "approval.granted" ? AdmissionDecision::allow :
                                                    AdmissionDecision::deny;
    }
    return AdmissionDecision::ask;
  });
  open_ = true;
  if (auto result = reconcile(); !result) { open_ = false; return result; }
  auto started = store_.append(PhotonDraft{.ray = make_id("system-ray"),
      .kind = "system.started", .schema = "tokmon.system.started.v1",
      .payload = cbor::object({{"version", "0.1.0"},
          {"light_path_hash", path_.snapshot()->hash}}), .epoch = path_.snapshot()->epoch});
  if (!started) return tl::unexpected(started.error());
  log_info("Tokmon runtime ready epoch={} lenses={}", path_.snapshot()->epoch,
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
    const auto previous = std::find_if(current->lenses.begin(), current->lenses.end(),
        [&](const MountedLens& mounted) {
          return mounted.lens && mounted.lens->manifest().id == desired.id;
        });
    if (previous != current->lenses.end()) {
      const auto& old_permissions = previous->lens->manifest().light_permissions;
      for (const auto& permission : lens->manifest().light_permissions)
        if (std::find(old_permissions.begin(), old_permissions.end(), permission) ==
            old_permissions.end())
          return tl::unexpected(rejected(desired.id, make_error(ErrorCode::permission_denied,
              "Lens replacement expands authority with permission '" + permission +
              "'; an explicitly approved configuration epoch is required")));
    }
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
  if (auto ordered = order_light_path(candidate->lenses); !ordered)
    return tl::unexpected(rejected("org.tokmon.lens.ignis", ordered.error()));
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
  return submit(std::move(input), cbor::Value::Map{});
}
Result<RayId> TokmonRuntime::submit(std::string input, cbor::Value context) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  return engine_->begin(std::move(input), 0, std::move(context));
}
Result<RayId> TokmonRuntime::submit_to(const RayId& ray, std::string input) {
  return submit_to(ray, std::move(input), cbor::Value::Map{});
}
Result<RayId> TokmonRuntime::submit_to(const RayId& ray, std::string input,
                                       cbor::Value context) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  auto history = store_.read_ray(ray);
  if (!history) return tl::unexpected(history.error());
  if (history->empty())
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "cannot continue an unknown ray"));
  auto appended = engine_->continue_ray(ray, std::move(input), 0, std::move(context));
  if (!appended) return tl::unexpected(appended.error());
  return ray;
}
Result<RefractionResult> TokmonRuntime::refract(Act act) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  return engine_->refract(std::move(act));
}
Result<std::size_t> TokmonRuntime::advance(const RayId& ray) {
  if (!engine_) return tl::unexpected(make_error(ErrorCode::invalid_state, "runtime is not open"));
  auto advanced = engine_->advance(ray, config_.max_beats);
  if (!advanced) return tl::unexpected(advanced.error());
  std::size_t total = *advanced;
  auto parent_history = store_.read_ray(ray);
  if (!parent_history) return tl::unexpected(parent_history.error());
  bool child_fact_added = false;
  for (const auto& started : *parent_history) {
    if (started.kind != "child.started") continue;
    const auto* child_field = cbor::find(started.payload, "child_ray");
    if (!child_field || child_field->as_string().empty()) continue;
    const auto child = std::string(child_field->as_string());
    const auto already_terminal = std::any_of(parent_history->begin(), parent_history->end(),
        [&child](const Photon& photon) {
          if (photon.kind != "child.completed" && photon.kind != "child.failed" &&
              photon.kind != "child.cancelled") return false;
          const auto* candidate = cbor::find(photon.payload, "child_ray");
          return candidate && candidate->as_string() == child;
        });
    if (already_terminal) continue;
    const auto* budget = cbor::find(started.payload, "budget");
    const auto child_beats = static_cast<std::size_t>(std::clamp<std::int64_t>(
        budget ? budget->as_integer(static_cast<std::int64_t>(config_.max_beats)) :
                 static_cast<std::int64_t>(config_.max_beats),
        1, static_cast<std::int64_t>(config_.max_beats)));
    auto child_result = engine_->advance(child, child_beats);
    auto child_history = store_.read_ray(child);
    if (!child_history) return tl::unexpected(child_history.error());
    const Photon* answer = nullptr;
    for (auto iterator = child_history->rbegin(); iterator != child_history->rend(); ++iterator)
      if (iterator->kind == "assistant.message") { answer = &*iterator; break; }
    auto terminal = store_.append(PhotonDraft{.ray = ray,
        .kind = child_result ? "child.completed" : "child.failed",
        .schema = "tokmon.child.supervision.v1",
        .payload = cbor::object({{"child_ray", child}, {"parent_ray", ray},
          {"beats", child_result ? static_cast<std::int64_t>(*child_result) : 0},
          {"summary", answer && cbor::find(answer->payload, "text") ?
              *cbor::find(answer->payload, "text") : cbor::Value("")},
          {"tail_sequence", child_history->empty() ? 0 :
              static_cast<std::int64_t>(child_history->back().sequence)},
          {"error", child_result ? "" : child_result.error().describe()},
          {"history_deleted", false}}), .epoch = path_.snapshot()->epoch,
        .caused_by_act = started.caused_by_act});
    if (!terminal) return tl::unexpected(terminal.error());
    if (child_result) total += *child_result;
    child_fact_added = true;
  }
  if (child_fact_added) {
    auto resumed = engine_->advance(ray, config_.max_beats);
    if (!resumed) return tl::unexpected(resumed.error());
    total += *resumed;
  }
  return total;
}
void TokmonRuntime::cancel(const RayId& ray) noexcept {
  if (engine_) engine_->cancel_ray(ray);
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
