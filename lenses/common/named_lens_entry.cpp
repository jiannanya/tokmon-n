#include "tokmon_lens_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stop_token>
#include <vector>

#ifndef TOKMON_LENS_HEADER
#error "TOKMON_LENS_HEADER must name the concrete built-in Lens header"
#endif

#ifndef TOKMON_LENS_TYPE
#error "TOKMON_LENS_TYPE must name the concrete built-in Lens type"
#endif

#include TOKMON_LENS_HEADER

#ifndef TOKMON_LENS_SHORT_ID
#define TOKMON_LENS_SHORT_ID "unknown"
#endif

namespace {

using namespace tokmon;
using SelectedLens = TOKMON_LENS_TYPE;

struct Instance {
  std::shared_ptr<ILens> lens{std::make_shared<SelectedLens>()};
  std::stop_source stop;
};

TokmonOwnedBytesV1 owned(std::vector<std::uint8_t> bytes) {
  TokmonOwnedBytesV1 result{};
  if (bytes.empty()) return result;
  result.data = static_cast<std::uint8_t*>(std::malloc(bytes.size()));
  if (!result.data) return result;
  std::memcpy(result.data, bytes.data(), bytes.size());
  result.size = bytes.size();
  result.release = [](std::uint8_t* data, std::size_t, void*) { std::free(data); };
  return result;
}

cbor::Value error_value(const Error& error) {
  return cbor::object({{"code", std::string(to_string(error.code))},
                       {"message", error.message}, {"lens", error.lens},
                       {"retryable", error.retryable}});
}

cbor::Value manifest_value(const LensManifest& manifest) {
  cbor::Value::Array channels;
  for (const auto& channel : manifest.view_channels) channels.emplace_back(channel);
  cbor::Value::Array observes;
  for (const auto& pattern : manifest.observes)
    observes.push_back(cbor::object({{"kind", pattern.kind}, {"schema", pattern.schema}}));
  cbor::Value::Array refracts;
  for (const auto& pattern : manifest.refracts)
    refracts.push_back(cbor::object({{"kind", pattern.kind}, {"schema", pattern.schema}}));
  cbor::Value::Array permissions;
  for (const auto& permission : manifest.light_permissions) permissions.emplace_back(permission);
  cbor::Value::Array dependencies;
  for (const auto& dependency : manifest.dependencies)
    dependencies.push_back(cbor::object({{"id", dependency.id},
                                         {"version", dependency.version}}));
  cbor::Value::Array conflicts;
  for (const auto& conflict : manifest.conflicts) conflicts.emplace_back(conflict);
  cbor::Value::Array before;
  for (const auto& id : manifest.optical_before) before.emplace_back(id);
  cbor::Value::Array after;
  for (const auto& id : manifest.optical_after) after.emplace_back(id);
  cbor::Value::Array provides;
  for (const auto& item : manifest.provides_queries)
    provides.push_back(cbor::object({
        {"capability", item.capability}, {"request_schema", item.request_schema},
        {"response_schema", item.response_schema}, {"deterministic", item.deterministic},
        {"priority", item.priority},
        {"default_timeout_ms", static_cast<std::int64_t>(item.default_timeout.count())},
        {"max_timeout_ms", static_cast<std::int64_t>(item.max_timeout.count())},
        {"max_request_bytes", static_cast<std::int64_t>(item.max_request_bytes)},
        {"max_response_bytes", static_cast<std::int64_t>(item.max_response_bytes)},
        {"max_concurrent_queries", static_cast<std::int64_t>(item.max_concurrent_queries)},
        {"max_queries_per_beat", static_cast<std::int64_t>(item.max_queries_per_beat)},
        {"cache", std::string(to_string(item.cache))}}));
  cbor::Value::Array consumes;
  for (const auto& item : manifest.consumes_queries)
    consumes.push_back(cbor::object({{"capability", item.capability},
        {"cardinality", std::string(to_string(item.cardinality))},
        {"required", item.required}, {"merge", std::string(to_string(item.merge))}}));
  return cbor::object({{"id", manifest.id}, {"display_name", manifest.display_name},
      {"version", manifest.version}, {"abi_major", static_cast<std::int64_t>(manifest.abi_major)},
      {"abi_minor", static_cast<std::int64_t>(manifest.abi_minor)},
      {"runtime", std::string(to_string(manifest.runtime))},
      {"runtime_version", manifest.runtime_version},
      {"runtime_entry", manifest.runtime_entry},
      {"trust", static_cast<std::int64_t>(manifest.trust)},
      {"observes", std::move(observes)}, {"view_channels", std::move(channels)},
      {"provides_queries", std::move(provides)},
      {"consumes_queries", std::move(consumes)},
      {"refracts", std::move(refracts)}, {"permissions", std::move(permissions)},
      {"stateless", manifest.stateless}, {"dependencies", std::move(dependencies)},
      {"conflicts", std::move(conflicts)}, {"optical_before", std::move(before)},
      {"optical_after", std::move(after)},
      {"resources", cbor::object({
          {"memory_mb", static_cast<std::int64_t>(manifest.resources.memory_mb)},
          {"output_bytes", static_cast<std::int64_t>(manifest.resources.output_bytes)},
          {"deadline_ms", static_cast<std::int64_t>(manifest.resources.deadline.count())}})},
      {"replacement", manifest.replacement}, {"schema_bundle", manifest.schema_bundle},
      {"sbom", manifest.sbom}});
}

class CollectingHost final : public OpticalHost {
 public:
  Result<Photon> emit(PhotonDraft draft) override {
    drafts.push_back(draft);
    Photon photon;
    photon.id = make_id("worker-photon"); photon.ray = draft.ray;
    photon.kind = draft.kind; photon.schema = draft.schema; photon.payload = draft.payload;
    photon.epoch = draft.epoch; photon.caused_by_act = draft.caused_by_act;
    return photon;
  }
  void log(std::string_view level, std::string_view message, const LensId& lens) override {
    logs.push_back(cbor::object({{"level", std::string(level)},
                                 {"message", std::string(message)}, {"lens", lens}}));
  }
  std::vector<PhotonDraft> drafts;
  cbor::Value::Array logs;
};

cbor::Value drafts_value(const std::vector<PhotonDraft>& drafts) {
  cbor::Value::Array items;
  for (const auto& draft : drafts) {
    items.push_back(cbor::object({{"ray", draft.ray},
        {"parent", draft.parent ? cbor::Value(*draft.parent) : cbor::Value(nullptr)},
        {"kind", draft.kind}, {"schema", draft.schema}, {"payload", draft.payload},
        {"epoch", static_cast<std::int64_t>(draft.epoch)},
        {"caused_by_act", draft.caused_by_act}}));
  }
  return cbor::object({{"drafts", std::move(items)}});
}

void* create_instance() {
  try { return new Instance(); } catch (...) { return nullptr; }
}

int32_t view_instance(void* raw, TokmonBytesV1 bytes, TokmonOwnedBytesV1* output,
                      TokmonOwnedBytesV1* error) {
  if (!raw || !output || !error) return -1;
  auto decoded = cbor::decode(std::span(bytes.data, bytes.size));
  if (!decoded) { *error = owned(cbor::encode(error_value(decoded.error()))); return 1; }
  auto window = photon_window_from_cbor(*decoded);
  if (!window) { *error = owned(cbor::encode(error_value(window.error()))); return 1; }
  auto* instance = static_cast<Instance*>(raw);
  if (!instance->lens) return -1;
  SurfaceBuilder builder(instance->lens->manifest().id);
  auto result = instance->lens->view(*window, builder);
  if (!result) { *error = owned(cbor::encode(error_value(result.error()))); return 1; }
  SurfaceSnapshot surface{.contributions = builder.contributions(),
                          .proposals = builder.proposals()};
  *output = owned(cbor::encode(to_cbor(surface)));
  return 0;
}

int32_t refract_instance(void* raw, TokmonBytesV1 window_bytes, TokmonBytesV1 act_bytes,
                         TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* drafts,
                         TokmonOwnedBytesV1* error) {
  if (!raw || !output || !drafts || !error) return -1;
  auto window_value = cbor::decode(std::span(window_bytes.data, window_bytes.size));
  auto act_value = cbor::decode(std::span(act_bytes.data, act_bytes.size));
  if (!window_value) { *error = owned(cbor::encode(error_value(window_value.error()))); return 1; }
  if (!act_value) { *error = owned(cbor::encode(error_value(act_value.error()))); return 1; }
  auto window = photon_window_from_cbor(*window_value);
  auto act = act_from_cbor(*act_value);
  if (!window) { *error = owned(cbor::encode(error_value(window.error()))); return 1; }
  if (!act) { *error = owned(cbor::encode(error_value(act.error()))); return 1; }
  auto* instance = static_cast<Instance*>(raw);
  CollectingHost host;
  RefractionBeam beam(host, *act, instance->stop.get_token(),
                      std::chrono::steady_clock::now() + act->timeout);
  if (!instance->lens) return -1;
  auto result = instance->lens->refract(*window, *act, beam);
  if (!result) { *error = owned(cbor::encode(error_value(result.error()))); return 1; }
  cbor::Value::Array emitted;
  for (const auto& id : result->emitted) emitted.emplace_back(id);
  *output = owned(cbor::encode(cbor::object({
      {"status", std::string(to_string(result->status))},
      {"emitted", std::move(emitted)}, {"detail", result->detail},
      {"logs", std::move(host.logs)}})));
  *drafts = owned(cbor::encode(drafts_value(host.drafts)));
  return 0;
}

Result<cbor::Value> call_optical(const TokmonOpticalHostV1* host,
                                const int operation, cbor::Value request) {
  if (!host || host->struct_size < sizeof(TokmonOpticalHostV1))
    return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                     "optical host table is incomplete"));
  const auto encoded = cbor::encode(request);
  TokmonOwnedBytesV1 output{}; TokmonOwnedBytesV1 error{};
  int32_t status = -1;
  if (operation == 0 && host->get)
    status = host->get(host->user, TokmonBytesV1{encoded.data(), encoded.size()},
                       &output, &error);
  else if (operation == 1 && host->get_all)
    status = host->get_all(host->user, TokmonBytesV1{encoded.data(), encoded.size()},
                           &output, &error);
  else if (operation == 2 && host->query)
    status = host->query(host->user, TokmonBytesV1{encoded.data(), encoded.size()},
                         &output, &error);
  if (status != 0) {
    std::string message = "host optical operation failed";
    auto code = ErrorCode::provider_failed;
    bool retryable = false;
    if (error.data && error.size) {
      auto decoded = cbor::decode(std::span(error.data, error.size));
      if (decoded) {
        if (const auto* part = cbor::find(*decoded, "message"))
          message = std::string(part->as_string(message));
        if (const auto* part = cbor::find(*decoded, "code"))
          code = error_code_from_string(part->as_string(), code);
        if (const auto* part = cbor::find(*decoded, "retryable"))
          retryable = part->as_bool();
      }
    }
    if (output.release && output.data) output.release(output.data, output.size, output.user);
    if (error.release && error.data) error.release(error.data, error.size, error.user);
    return tl::unexpected(make_error(code, std::move(message), retryable));
  }
  auto result = cbor::decode(std::span(output.data, output.size));
  if (output.release && output.data) output.release(output.data, output.size, output.user);
  if (error.release && error.data) error.release(error.data, error.size, error.user);
  return result;
}

int32_t derive_instance(void* raw, TokmonBytesV1 bytes,
                        TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* error) {
  if (!raw || !output || !error) return -1;
  auto decoded = cbor::decode(std::span(bytes.data, bytes.size));
  auto window = decoded ? photon_window_from_cbor(*decoded) :
      Result<PhotonWindow>(tl::unexpected(decoded.error()));
  if (!window) { *error = owned(cbor::encode(error_value(window.error()))); return 1; }
  auto* instance = static_cast<Instance*>(raw);
  auto* extension = dynamic_cast<IOpticalLensExtension*>(instance->lens.get());
  if (!extension || !extension->supports_derive()) {
    *output = owned(cbor::encode(cbor::Value{})); return 0;
  }
  auto result = extension->derive(*window);
  if (!result) { *error = owned(cbor::encode(error_value(result.error()))); return 1; }
  *output = owned(cbor::encode(*result));
  return 0;
}

int32_t coordinate_instance(void* raw, TokmonBytesV1 bytes,
                            const TokmonOpticalHostV1* host,
                            TokmonOwnedBytesV1* output, TokmonOwnedBytesV1* error) {
  if (!raw || !host || !output || !error) return -1;
  auto decoded = cbor::decode(std::span(bytes.data, bytes.size));
  auto window = decoded ? photon_window_from_cbor(*decoded) :
      Result<PhotonWindow>(tl::unexpected(decoded.error()));
  if (!window) { *error = owned(cbor::encode(error_value(window.error()))); return 1; }
  auto* instance = static_cast<Instance*>(raw);
  auto* extension = dynamic_cast<IOpticalLensExtension*>(instance->lens.get());
  SurfaceBuilder surface(instance->lens->manifest().id);
  if (extension && extension->supports_coordinate()) {
    auto optical = OpticalContext::from_callbacks(
        [host](const std::string_view channel, const std::string_view key) {
          return call_optical(host, 0, cbor::object({{"channel", std::string(channel)},
                                                     {"key", std::string(key)}}));
        },
        [host](const std::string_view channel) -> Result<std::vector<cbor::Value>> {
          auto value = call_optical(host, 1,
                                    cbor::object({{"channel", std::string(channel)}}));
          if (!value) return tl::unexpected(value.error());
          if (!value->as_array())
            return tl::unexpected(make_error(ErrorCode::protocol_error,
                                             "optical get_all response must be an array"));
          return *value->as_array();
        },
        [host](OpticalQueryRequest request) {
          return call_optical(host, 2, cbor::object({
              {"capability", request.capability}, {"parameters", request.parameters},
              {"request_schema", request.request_schema},
              {"response_schema", request.response_schema},
              {"timeout_ms", static_cast<std::int64_t>(request.timeout.count())},
              {"max_response_bytes", static_cast<std::int64_t>(request.max_response_bytes)}}));
        });
    auto result = extension->coordinate(*window, optical, surface);
    if (!result) { *error = owned(cbor::encode(error_value(result.error()))); return 1; }
  }
  *output = owned(cbor::encode(to_cbor(SurfaceSnapshot{
      .contributions = surface.contributions(), .proposals = surface.proposals()})));
  return 0;
}

int32_t query_instance(void* raw, TokmonBytesV1 state_bytes,
                       TokmonBytesV1 request_bytes, TokmonOwnedBytesV1* output,
                       TokmonOwnedBytesV1* error) {
  if (!raw || !output || !error) return -1;
  auto state_value = cbor::decode(std::span(state_bytes.data, state_bytes.size));
  auto request = cbor::decode(std::span(request_bytes.data, request_bytes.size));
  if (!state_value || !request) {
    const auto failure = !state_value ? state_value.error() : request.error();
    *error = owned(cbor::encode(error_value(failure))); return 1;
  }
  auto* instance = static_cast<Instance*>(raw);
  auto* extension = dynamic_cast<IOpticalLensExtension*>(instance->lens.get());
  if (!extension || !extension->supports_query()) {
    *error = owned(cbor::encode(error_value(make_error(
        ErrorCode::unsupported, "Lens has no optical query handler")))); return 1;
  }
  FrozenLensState state;
  if (const auto* part = cbor::find(*state_value, "lens")) state.lens = part->as_string();
  if (const auto* part = cbor::find(*state_value, "artifact_hash")) state.artifact_hash = part->as_string();
  if (const auto* part = cbor::find(*state_value, "epoch")) state.epoch = static_cast<MountEpoch>(part->as_integer());
  if (const auto* part = cbor::find(*state_value, "generation")) state.generation = static_cast<GenerationId>(part->as_integer());
  if (const auto* part = cbor::find(*state_value, "path_index")) state.path_index = static_cast<std::size_t>(part->as_integer());
  state.value = std::make_shared<const cbor::Value>(
      cbor::find(*state_value, "value") ? *cbor::find(*state_value, "value") : cbor::Value{});
  const auto timeout = cbor::find(*request, "timeout_ms") ?
      cbor::find(*request, "timeout_ms")->as_integer(1) : 1;
  QueryBudget budget{.deadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(std::max<std::int64_t>(1, timeout)),
      .max_request_bytes = static_cast<std::size_t>(cbor::find(*request, "max_request_bytes") ?
          cbor::find(*request, "max_request_bytes")->as_integer() : 0),
      .max_response_bytes = static_cast<std::size_t>(cbor::find(*request, "max_response_bytes") ?
          cbor::find(*request, "max_response_bytes")->as_integer() : 0),
      .call_index = static_cast<std::size_t>(cbor::find(*request, "call_index") ?
          cbor::find(*request, "call_index")->as_integer() : 0)};
  const auto* capability = cbor::find(*request, "capability");
  const auto* parameters = cbor::find(*request, "parameters");
  if (!capability || !parameters) return -1;
  auto result = extension->optical_query(state, capability->as_string(), *parameters, budget);
  if (!result) { *error = owned(cbor::encode(error_value(result.error()))); return 1; }
  *output = owned(cbor::encode(*result));
  return 0;
}

void request_stop(void* raw) {
  if (!raw) return;
  auto* instance = static_cast<Instance*>(raw);
  instance->stop.request_stop();
  if (instance->lens) instance->lens->request_stop();
}

void destroy_instance(void* raw) { delete static_cast<Instance*>(raw); }

}  // namespace

extern "C" TOKMON_LENS_EXPORT TokmonLensApiV1 tokmon_lens_entry_v1(void) {
  static const auto manifest = [] {
    const SelectedLens lens;
    return tokmon::cbor::encode(manifest_value(lens.manifest()));
  }();
  return TokmonLensApiV1{
      TOKMON_LENS_ABI_MAJOR, TOKMON_LENS_ABI_MINOR,
      TokmonBytesV1{manifest.data(), manifest.size()}, &create_instance,
      &view_instance, &refract_instance, &request_stop, &destroy_instance};
}

extern "C" TOKMON_LENS_EXPORT const void* tokmon_lens_get_extension_v1(
    const char* extension_id) {
  if (!extension_id || std::strcmp(extension_id, TOKMON_OPTICAL_QUERY_EXTENSION_V1) != 0)
    return nullptr;
  static const TokmonOpticalQueryExtensionV1 extension = [] {
    SelectedLens lens;
    const auto* optical = dynamic_cast<const IOpticalLensExtension*>(&lens);
    return TokmonOpticalQueryExtensionV1{
        sizeof(TokmonOpticalQueryExtensionV1), 1u,
        optical && optical->supports_derive() ? &derive_instance : nullptr,
        optical && optical->supports_coordinate() ? &coordinate_instance : nullptr,
        optical && optical->supports_query() ? &query_instance : nullptr};
  }();
  return &extension;
}
