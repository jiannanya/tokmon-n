#include "tokmon_lens_api.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <stop_token>
#include <vector>

#include "tokmon/builtin_lens.hpp"

#ifndef TOKMON_LENS_SHORT_ID
#define TOKMON_LENS_SHORT_ID "unknown"
#endif

namespace {

using namespace tokmon;

struct Instance {
  std::shared_ptr<ILens> lens{make_builtin_lens(TOKMON_LENS_SHORT_ID)};
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
  return cbor::object({{"id", manifest.id}, {"display_name", manifest.display_name},
      {"version", manifest.version}, {"abi_major", static_cast<std::int64_t>(manifest.abi_major)},
      {"abi_minor", static_cast<std::int64_t>(manifest.abi_minor)},
      {"runtime", std::string(to_string(manifest.runtime))},
      {"runtime_version", manifest.runtime_version},
      {"runtime_entry", manifest.runtime_entry},
      {"trust", static_cast<std::int64_t>(manifest.trust)},
      {"observes", std::move(observes)}, {"view_channels", std::move(channels)},
      {"refracts", std::move(refracts)}, {"permissions", std::move(permissions)},
      {"stateless", manifest.stateless}});
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

void request_stop(void* raw) {
  if (!raw) return;
  auto* instance = static_cast<Instance*>(raw);
  instance->stop.request_stop();
  if (instance->lens) instance->lens->request_stop();
}

void destroy_instance(void* raw) { delete static_cast<Instance*>(raw); }

}  // namespace

extern "C" TOKMON_LENS_EXPORT TokmonLensApiV1 tokmon_lens_entry_v1(void) {
  static const auto manifest = tokmon::cbor::encode(
      manifest_value(tokmon::builtin_lens_manifest(TOKMON_LENS_SHORT_ID)));
  return TokmonLensApiV1{
      TOKMON_LENS_ABI_MAJOR, TOKMON_LENS_ABI_MINOR,
      TokmonBytesV1{manifest.data(), manifest.size()}, &create_instance,
      &view_instance, &refract_instance, &request_stop, &destroy_instance};
}
