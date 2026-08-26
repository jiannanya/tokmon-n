#include "tokmon/c_abi_loader.hpp"

#include "tokmon_lens_api.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <span>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tokmon {
namespace {

void release(TokmonOwnedBytesV1& bytes) {
  if (bytes.release && bytes.data) bytes.release(bytes.data, bytes.size, bytes.user);
  bytes = {};
}

TokmonOwnedBytesV1 owned_bytes(std::vector<std::uint8_t> bytes) {
  TokmonOwnedBytesV1 result{};
  if (bytes.empty()) return result;
  result.data = static_cast<std::uint8_t*>(std::malloc(bytes.size()));
  if (!result.data) return result;
  std::memcpy(result.data, bytes.data(), bytes.size());
  result.size = bytes.size();
  result.release = [](std::uint8_t* data, std::size_t, void*) { std::free(data); };
  return result;
}

cbor::Value error_frame(const Error& error) {
  return cbor::object({{"code", std::string(to_string(error.code))},
                       {"message", error.message}, {"retryable", error.retryable}});
}

int32_t optical_get(void* user, const TokmonBytesV1 request,
                    TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* error) {
  if (!user || !output || !error) return -1;
  auto value = cbor::decode(std::span(request.data, request.size));
  if (!value) { *error = owned_bytes(cbor::encode(error_frame(value.error()))); return 1; }
  const auto* channel = cbor::find(*value, "channel");
  const auto* key = cbor::find(*value, "key");
  if (!channel || !key) return -1;
  auto result = static_cast<const OpticalContext*>(user)->get(channel->as_string(), key->as_string());
  if (!result) { *error = owned_bytes(cbor::encode(error_frame(result.error()))); return 1; }
  *output = owned_bytes(cbor::encode(*result));
  return 0;
}

int32_t optical_get_all(void* user, const TokmonBytesV1 request,
                        TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* error) {
  if (!user || !output || !error) return -1;
  auto value = cbor::decode(std::span(request.data, request.size));
  if (!value) { *error = owned_bytes(cbor::encode(error_frame(value.error()))); return 1; }
  const auto* channel = cbor::find(*value, "channel");
  if (!channel) return -1;
  auto result = static_cast<const OpticalContext*>(user)->get_all(channel->as_string());
  if (!result) { *error = owned_bytes(cbor::encode(error_frame(result.error()))); return 1; }
  cbor::Value::Array items;
  for (auto& item : *result) items.push_back(std::move(item));
  *output = owned_bytes(cbor::encode(cbor::Value(std::move(items))));
  return 0;
}

int32_t optical_query_callback(void* user, const TokmonBytesV1 request,
                               TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* error) {
  if (!user || !output || !error) return -1;
  auto value = cbor::decode(std::span(request.data, request.size));
  if (!value) { *error = owned_bytes(cbor::encode(error_frame(value.error()))); return 1; }
  OpticalQueryRequest query;
  if (const auto* part = cbor::find(*value, "capability")) query.capability = part->as_string();
  if (const auto* part = cbor::find(*value, "parameters")) query.parameters = *part;
  if (const auto* part = cbor::find(*value, "request_schema")) query.request_schema = part->as_string();
  if (const auto* part = cbor::find(*value, "response_schema")) query.response_schema = part->as_string();
  if (const auto* part = cbor::find(*value, "timeout_ms")) query.timeout = std::chrono::milliseconds(part->as_integer());
  if (const auto* part = cbor::find(*value, "max_response_bytes")) query.max_response_bytes = static_cast<std::size_t>(part->as_integer());
  auto result = static_cast<const OpticalContext*>(user)->query(std::move(query));
  if (!result) { *error = owned_bytes(cbor::encode(error_frame(result.error()))); return 1; }
  *output = owned_bytes(cbor::encode(*result));
  return 0;
}

Error decode_error(const TokmonOwnedBytesV1& bytes, const Error fallback) {
  if (!bytes.data || bytes.size == 0) return fallback;
  auto value = cbor::decode(std::span(bytes.data, bytes.size));
  if (!value) return fallback;
  const auto* message = cbor::find(*value, "message");
  auto error = fallback;
  if (const auto* code = cbor::find(*value, "code"))
    error.code = error_code_from_string(code->as_string(), fallback.code);
  if (message) error.message = std::string(message->as_string(error.message));
  if (const auto* retryable = cbor::find(*value, "retryable"))
    error.retryable = retryable->as_bool(error.retryable);
  return error;
}

RuntimeKind runtime_from(std::string_view value) {
  if (value == "native_worker") return RuntimeKind::native_worker;
  if (value == "node") return RuntimeKind::node;
  if (value == "cpython") return RuntimeKind::cpython;
  if (value == "wasm") return RuntimeKind::wasm;
  if (value == "desktop") return RuntimeKind::desktop;
  return RuntimeKind::in_process;
}

Result<LensManifest> parse_manifest(const TokmonBytesV1 bytes) {
  auto value = cbor::decode(std::span(bytes.data, bytes.size));
  if (!value) return tl::unexpected(value.error());
  LensManifest manifest;
  if (const auto* field = cbor::find(*value, "id")) manifest.id = field->as_string();
  if (const auto* field = cbor::find(*value, "display_name")) manifest.display_name = field->as_string();
  if (const auto* field = cbor::find(*value, "version")) manifest.version = field->as_string();
  if (const auto* field = cbor::find(*value, "abi_major")) manifest.abi_major = static_cast<std::uint32_t>(field->as_integer());
  if (const auto* field = cbor::find(*value, "abi_minor")) manifest.abi_minor = static_cast<std::uint32_t>(field->as_integer());
  if (const auto* field = cbor::find(*value, "runtime")) manifest.runtime = runtime_from(field->as_string());
  if (const auto* field = cbor::find(*value, "runtime_version"))
    manifest.runtime_version = field->as_string();
  if (const auto* field = cbor::find(*value, "runtime_entry"))
    manifest.runtime_entry = field->as_string();
  if (const auto* field = cbor::find(*value, "trust")) manifest.trust = static_cast<TrustLevel>(field->as_integer());
  if (const auto* field = cbor::find(*value, "stateless")) manifest.stateless = field->as_bool(true);
  if (const auto* field = cbor::find(*value, "view_channels"); field && field->as_array())
    for (const auto& item : *field->as_array()) manifest.view_channels.emplace_back(item.as_string());
  if (const auto* field = cbor::find(*value, "provides_queries"); field && field->as_array())
    for (const auto& item : *field->as_array()) {
      OpticalQueryCapability capability;
      if (const auto* part = cbor::find(item, "capability")) capability.capability = part->as_string();
      if (const auto* part = cbor::find(item, "request_schema")) capability.request_schema = part->as_string();
      if (const auto* part = cbor::find(item, "response_schema")) capability.response_schema = part->as_string();
      if (const auto* part = cbor::find(item, "deterministic")) capability.deterministic = part->as_bool(true);
      if (const auto* part = cbor::find(item, "priority")) capability.priority = static_cast<std::int32_t>(part->as_integer());
      if (const auto* part = cbor::find(item, "default_timeout_ms")) capability.default_timeout = std::chrono::milliseconds(part->as_integer(10));
      if (const auto* part = cbor::find(item, "max_timeout_ms")) capability.max_timeout = std::chrono::milliseconds(part->as_integer(100));
      if (const auto* part = cbor::find(item, "max_request_bytes")) capability.max_request_bytes = static_cast<std::size_t>(part->as_integer(256 * 1024));
      if (const auto* part = cbor::find(item, "max_response_bytes")) capability.max_response_bytes = static_cast<std::size_t>(part->as_integer(1024 * 1024));
      if (const auto* part = cbor::find(item, "max_concurrent_queries")) capability.max_concurrent_queries = static_cast<std::size_t>(part->as_integer(4));
      if (const auto* part = cbor::find(item, "max_queries_per_beat")) capability.max_queries_per_beat = static_cast<std::size_t>(part->as_integer(1024));
      if (const auto* part = cbor::find(item, "cache"); part && part->as_string() == "none") capability.cache = OpticalQueryCache::none;
      manifest.provides_queries.push_back(std::move(capability));
    }
  if (const auto* field = cbor::find(*value, "consumes_queries"); field && field->as_array())
    for (const auto& item : *field->as_array()) {
      OpticalQueryConsumption consumption;
      if (const auto* part = cbor::find(item, "capability")) consumption.capability = part->as_string();
      if (const auto* part = cbor::find(item, "cardinality")) {
        if (part->as_string() == "single") consumption.cardinality = OpticalQueryCardinality::single;
        else if (part->as_string() == "many") consumption.cardinality = OpticalQueryCardinality::many;
      }
      if (const auto* part = cbor::find(item, "required")) consumption.required = part->as_bool();
      if (const auto* part = cbor::find(item, "merge")) {
        if (part->as_string() == "all") consumption.merge = OpticalQueryMerge::all;
        else if (part->as_string() == "priority_then_path") consumption.merge = OpticalQueryMerge::priority_then_path;
      }
      manifest.consumes_queries.push_back(std::move(consumption));
    }
  if (const auto* field = cbor::find(*value, "permissions"); field && field->as_array())
    for (const auto& item : *field->as_array()) manifest.light_permissions.emplace_back(item.as_string());
  if (const auto* field = cbor::find(*value, "dependencies"); field && field->as_array())
    for (const auto& item : *field->as_array()) {
      const auto* id = cbor::find(item, "id");
      const auto* version = cbor::find(item, "version");
      if (id) manifest.dependencies.push_back(
          {std::string(id->as_string()), version ? std::string(version->as_string("*")) : "*"});
    }
  const auto read_strings = [&](const std::string_view name, std::vector<std::string>& output) {
    if (const auto* field = cbor::find(*value, name); field && field->as_array())
      for (const auto& item : *field->as_array()) output.emplace_back(item.as_string());
  };
  read_strings("conflicts", manifest.conflicts);
  read_strings("optical_before", manifest.optical_before);
  read_strings("optical_after", manifest.optical_after);
  if (const auto* resources = cbor::find(*value, "resources")) {
    if (const auto* field = cbor::find(*resources, "memory_mb"))
      manifest.resources.memory_mb = static_cast<std::size_t>(field->as_integer(256));
    if (const auto* field = cbor::find(*resources, "output_bytes"))
      manifest.resources.output_bytes = static_cast<std::size_t>(field->as_integer(1024 * 1024));
    if (const auto* field = cbor::find(*resources, "deadline_ms"))
      manifest.resources.deadline = std::chrono::milliseconds(field->as_integer(30'000));
  }
  if (const auto* field = cbor::find(*value, "replacement"))
    manifest.replacement = std::string(field->as_string("R1"));
  if (const auto* field = cbor::find(*value, "schema_bundle"))
    manifest.schema_bundle = std::string(field->as_string());
  if (const auto* field = cbor::find(*value, "sbom"))
    manifest.sbom = std::string(field->as_string());
  if (const auto* field = cbor::find(*value, "observes"); field && field->as_array()) {
    for (const auto& item : *field->as_array())
      manifest.observes.push_back({std::string(cbor::find(item, "kind")->as_string()),
                                   std::string(cbor::find(item, "schema")->as_string())});
  }
  if (const auto* field = cbor::find(*value, "refracts"); field && field->as_array()) {
    for (const auto& item : *field->as_array())
      manifest.refracts.push_back({std::string(cbor::find(item, "kind")->as_string()),
                                   std::string(cbor::find(item, "schema")->as_string())});
  }
  if (manifest.id.empty() || manifest.abi_major != TOKMON_LENS_ABI_MAJOR)
    return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                     "Lens manifest ABI or id is invalid"));
  return manifest;
}

}  // namespace

struct CAbiLens::Impl {
#if defined(_WIN32)
  HMODULE library{nullptr};
#else
  void* library{nullptr};
#endif
  TokmonLensApiV1 api{};
  TokmonOpticalQueryExtensionV1 optical{};
  bool has_optical{false};
  void* instance{nullptr};
  LensManifest manifest;
};

CAbiLens::CAbiLens() : impl_(std::make_unique<Impl>()) {}
CAbiLens::~CAbiLens() {
  if (!impl_) return;
  if (impl_->instance && impl_->api.request_stop) impl_->api.request_stop(impl_->instance);
  if (impl_->instance && impl_->api.destroy) impl_->api.destroy(impl_->instance);
#if defined(_WIN32)
  if (impl_->library) FreeLibrary(impl_->library);
#else
  if (impl_->library) dlclose(impl_->library);
#endif
}

Result<std::shared_ptr<CAbiLens>> CAbiLens::load(const std::filesystem::path& path) {
  auto lens = std::shared_ptr<CAbiLens>(new CAbiLens());
#if defined(_WIN32)
  lens->impl_->library = LoadLibraryW(path.c_str());
  if (!lens->impl_->library)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "LoadLibrary failed for " + path.string()));
  auto entry = reinterpret_cast<TokmonLensEntryV1>(
      GetProcAddress(lens->impl_->library, "tokmon_lens_entry_v1"));
#else
  lens->impl_->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lens->impl_->library)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     std::string("dlopen failed: ") + dlerror()));
  auto entry = reinterpret_cast<TokmonLensEntryV1>(
      dlsym(lens->impl_->library, "tokmon_lens_entry_v1"));
#endif
  if (!entry)
    return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                     "tokmon_lens_entry_v1 is missing"));
  lens->impl_->api = entry();
  TokmonLensGetExtensionV1 get_extension = nullptr;
#if defined(_WIN32)
  get_extension = reinterpret_cast<TokmonLensGetExtensionV1>(
      GetProcAddress(lens->impl_->library, "tokmon_lens_get_extension_v1"));
#else
  get_extension = reinterpret_cast<TokmonLensGetExtensionV1>(
      dlsym(lens->impl_->library, "tokmon_lens_get_extension_v1"));
#endif
  if (get_extension) {
    const auto* extension = static_cast<const TokmonOpticalQueryExtensionV1*>(
        get_extension(TOKMON_OPTICAL_QUERY_EXTENSION_V1));
    if (extension && extension->struct_size >= sizeof(TokmonOpticalQueryExtensionV1) &&
        extension->version == 1u) {
      lens->impl_->optical = *extension;
      lens->impl_->has_optical = true;
    }
  }
  if (lens->impl_->api.abi_major != TOKMON_LENS_ABI_MAJOR ||
      lens->impl_->api.abi_minor > TOKMON_LENS_ABI_MINOR)
    return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                     "unsupported Lens ABI"));
  auto manifest = parse_manifest(lens->impl_->api.manifest_cbor);
  if (!manifest) return tl::unexpected(manifest.error());
  lens->impl_->manifest = std::move(*manifest);
  lens->impl_->instance = lens->impl_->api.create ? lens->impl_->api.create() : nullptr;
  if (!lens->impl_->instance)
    return tl::unexpected(make_error(ErrorCode::lens_crashed,
                                     "Lens instance creation failed"));
  return lens;
}

const LensManifest& CAbiLens::manifest() const noexcept { return impl_->manifest; }

Result<void> CAbiLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  const auto request = cbor::encode(to_cbor(photons));
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 error{};
  const auto status = impl_->api.view(impl_->instance,
      TokmonBytesV1{request.data(), request.size()}, &output, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::lens_crashed,
                                                        "C ABI Lens view failed"));
    release(output); release(error); return tl::unexpected(failure);
  }
  auto value = cbor::decode(std::span(output.data, output.size));
  release(output); release(error);
  if (!value) return tl::unexpected(value.error());
  auto snapshot = surface_from_cbor(*value);
  if (!snapshot) return tl::unexpected(snapshot.error());
  for (auto& contribution : snapshot->contributions) {
    auto result = surface.add(std::move(contribution.channel), std::move(contribution.key),
                              std::move(contribution.value), contribution.priority);
    if (!result) return result;
  }
  for (auto& proposal : snapshot->proposals) {
    auto result = surface.propose(std::move(proposal));
    if (!result) return result;
  }
  return {};
}

Result<RefractionResult> CAbiLens::refract(const PhotonWindow& photons, const Act& act,
                                           RefractionBeam& beam) {
  const auto window = cbor::encode(to_cbor(photons));
  const auto encoded_act = cbor::encode(to_cbor(act));
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 drafts{}; TokmonOwnedBytesV1 error{};
  const auto status = impl_->api.refract(impl_->instance,
      TokmonBytesV1{window.data(), window.size()},
      TokmonBytesV1{encoded_act.data(), encoded_act.size()}, &output, &drafts, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::lens_crashed,
                                                        "C ABI Lens refract failed"));
    release(output); release(drafts); release(error); return tl::unexpected(failure);
  }
  auto result_value = cbor::decode(std::span(output.data, output.size));
  auto drafts_value = cbor::decode(std::span(drafts.data, drafts.size));
  release(output); release(drafts); release(error);
  if (!result_value) return tl::unexpected(result_value.error());
  if (!drafts_value) return tl::unexpected(drafts_value.error());
  RefractionResult result{.status = RefractionStatus::completed};
  if (const auto* detail = cbor::find(*result_value, "detail")) result.detail = detail->as_string();
  if (const auto* items = cbor::find(*drafts_value, "drafts"); items && items->as_array()) {
    for (const auto& item : *items->as_array()) {
      const auto* kind = cbor::find(item, "kind"); const auto* schema = cbor::find(item, "schema");
      const auto* payload = cbor::find(item, "payload");
      if (!kind || !schema || !payload)
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "emitted PhotonDraft is incomplete"));
      auto photon = beam.emit(std::string(kind->as_string()), std::string(schema->as_string()), *payload);
      if (!photon) return tl::unexpected(photon.error());
      result.emitted.push_back(photon->id);
    }
  }
  return result;
}

void CAbiLens::request_stop() noexcept {
  if (impl_->instance && impl_->api.request_stop) impl_->api.request_stop(impl_->instance);
}

bool CAbiLens::supports_derive() const noexcept {
  return impl_->has_optical && impl_->optical.derive;
}

bool CAbiLens::supports_coordinate() const noexcept {
  return impl_->has_optical && impl_->optical.coordinate;
}

bool CAbiLens::supports_query() const noexcept {
  return impl_->has_optical && impl_->optical.query;
}

Result<cbor::Value> CAbiLens::derive(const PhotonWindow& photons) {
  if (!supports_derive()) return cbor::Value{};
  const auto window = cbor::encode(to_cbor(photons));
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 error{};
  const auto status = impl_->optical.derive(impl_->instance,
      TokmonBytesV1{window.data(), window.size()}, &output, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::provider_failed,
                                                        "C ABI Lens derive failed"));
    release(output); release(error); return tl::unexpected(failure);
  }
  auto value = cbor::decode(std::span(output.data, output.size));
  release(output); release(error);
  if (!value) return tl::unexpected(value.error());
  return value;
}

Result<void> CAbiLens::coordinate(const PhotonWindow& photons,
                                  const OpticalContext& optical,
                                  SurfaceBuilder& surface) {
  if (!supports_coordinate()) return {};
  const auto window = cbor::encode(to_cbor(photons));
  const TokmonOpticalHostV1 host{sizeof(TokmonOpticalHostV1),
      const_cast<OpticalContext*>(&optical), &optical_get, &optical_get_all,
      &optical_query_callback};
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 error{};
  const auto status = impl_->optical.coordinate(impl_->instance,
      TokmonBytesV1{window.data(), window.size()}, &host, &output, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::provider_failed,
                                                        "C ABI Lens coordinate failed"));
    release(output); release(error); return tl::unexpected(failure);
  }
  auto value = cbor::decode(std::span(output.data, output.size));
  release(output); release(error);
  if (!value) return tl::unexpected(value.error());
  auto snapshot = surface_from_cbor(*value);
  if (!snapshot) return tl::unexpected(snapshot.error());
  for (auto& item : snapshot->contributions) {
    auto added = surface.add(std::move(item.channel), std::move(item.key),
                             std::move(item.value), item.priority);
    if (!added) return added;
  }
  for (auto& proposal : snapshot->proposals) {
    auto added = surface.propose(std::move(proposal));
    if (!added) return added;
  }
  return {};
}

Result<cbor::Value> CAbiLens::optical_query(
    const FrozenLensState& state, const std::string_view capability,
    const cbor::Value& parameters, const QueryBudget& budget) const {
  if (!supports_query())
    return tl::unexpected(make_error(ErrorCode::unsupported,
                                     "C ABI Lens has no optical query extension"));
  const auto state_value = cbor::encode(cbor::object({
      {"lens", state.lens}, {"artifact_hash", state.artifact_hash},
      {"epoch", static_cast<std::int64_t>(state.epoch)},
      {"generation", static_cast<std::int64_t>(state.generation)},
      {"path_index", static_cast<std::int64_t>(state.path_index)},
      {"value", state.data()}}));
  const auto remaining = std::max<std::int64_t>(0,
      std::chrono::duration_cast<std::chrono::milliseconds>(
          budget.deadline - std::chrono::steady_clock::now()).count());
  const auto request = cbor::encode(cbor::object({
      {"capability", std::string(capability)}, {"parameters", parameters},
      {"timeout_ms", remaining},
      {"max_request_bytes", static_cast<std::int64_t>(budget.max_request_bytes)},
      {"max_response_bytes", static_cast<std::int64_t>(budget.max_response_bytes)},
      {"call_index", static_cast<std::int64_t>(budget.call_index)}}));
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 error{};
  const auto status = impl_->optical.query(impl_->instance,
      TokmonBytesV1{state_value.data(), state_value.size()},
      TokmonBytesV1{request.data(), request.size()}, &output, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::provider_failed,
                                                        "C ABI Lens query failed"));
    release(output); release(error); return tl::unexpected(failure);
  }
  auto value = cbor::decode(std::span(output.data, output.size));
  release(output); release(error);
  if (!value) return tl::unexpected(value.error());
  return value;
}

}  // namespace tokmon
