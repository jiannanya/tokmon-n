#include "tokmon/c_abi_loader.hpp"

#include "tokmon_lens_api.h"

#include <cstring>
#include <span>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tokmon {
namespace {

void release(TokmonOwnedBytes& bytes) {
  if (bytes.release && bytes.data) bytes.release(bytes.data, bytes.size, bytes.user);
  bytes = {};
}

Error decode_error(const TokmonOwnedBytes& bytes, const Error fallback) {
  if (!bytes.data || bytes.size == 0) return fallback;
  auto value = cbor::decode(std::span(bytes.data, bytes.size));
  if (!value) return fallback;
  const auto* message = cbor::find(*value, "message");
  auto error = fallback;
  if (message) error.message = std::string(message->as_string(error.message));
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

Result<LensManifest> parse_manifest(const TokmonBytes bytes) {
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
  const auto read_ports = [&](const std::string_view name,
                              std::vector<OpticalPortSpec>& output) -> Result<void> {
    const auto* field = cbor::find(*value, name);
    if (!field || !field->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens manifest port array is missing"));
    for (const auto& item : *field->as_array()) {
      auto port = optical_port_spec_from_cbor(item);
      if (!port) return tl::unexpected(port.error());
      output.push_back(std::move(*port));
    }
    return {};
  };
  if (auto parsed = read_ports("inputs", manifest.inputs); !parsed)
    return tl::unexpected(parsed.error());
  if (auto parsed = read_ports("outputs", manifest.outputs); !parsed)
    return tl::unexpected(parsed.error());
  if (const auto* field = cbor::find(*value, "trigger")) {
    auto trigger = trigger_policy_from_string(field->as_string());
    if (!trigger) return tl::unexpected(trigger.error());
    manifest.trigger = *trigger;
  }
  if (const auto* field = cbor::find(*value, "monotone"))
    manifest.monotone = field->as_bool();
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
  TokmonLensApi api{};
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
  auto entry = reinterpret_cast<TokmonLensEntry>(
      GetProcAddress(lens->impl_->library, "tokmon_lens_entry"));
#else
  lens->impl_->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lens->impl_->library)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     std::string("dlopen failed: ") + dlerror()));
  auto entry = reinterpret_cast<TokmonLensEntry>(
      dlsym(lens->impl_->library, "tokmon_lens_entry"));
#endif
  if (!entry)
    return tl::unexpected(make_error(ErrorCode::abi_mismatch,
                                     "tokmon_lens_entry is missing"));
  lens->impl_->api = entry();
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

Result<void> CAbiLens::view(const OpticalInput& input,
                            WavefrontBuilder& outgoing) {
  const auto request = cbor::encode(to_cbor(input));
  TokmonOwnedBytes output{}; TokmonOwnedBytes error{};
  const auto status = impl_->api.view(impl_->instance,
      TokmonBytes{request.data(), request.size()}, &output, &error);
  if (status != 0) {
    const auto failure = decode_error(error, make_error(ErrorCode::lens_crashed,
                                                        "C ABI Lens view failed"));
    release(output); release(error); return tl::unexpected(failure);
  }
  auto value = cbor::decode(std::span(output.data, output.size));
  release(output); release(error);
  if (!value) return tl::unexpected(value.error());
  const auto* cells = cbor::find(*value, "cells");
  if (!cells || !cells->as_array())
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "C ABI Lens returned no wavefront cells"));
  for (const auto& encoded : *cells->as_array()) {
    auto cell = field_cell_from_cbor(encoded);
    if (!cell) return tl::unexpected(cell.error());
    if (cell->band == "act.proposal") {
      auto act = act_from_cbor(cell->value);
      if (!act) return tl::unexpected(act.error());
      auto result = outgoing.propose(std::move(*act), cell->provenance.input_cells);
      if (!result) return result;
    } else {
      auto result = outgoing.emit(cell->provenance.output_port, std::move(cell->key),
                                  std::move(cell->value),
                                  cell->provenance.input_cells, cell->priority);
      if (!result) return tl::unexpected(result.error());
    }
  }
  return {};
}

Result<RefractionResult> CAbiLens::refract(const PhotonWindow& photons, const Act& act,
                                           RefractionBeam& beam) {
  const auto window = cbor::encode(to_cbor(photons));
  const auto encoded_act = cbor::encode(to_cbor(act));
  TokmonOwnedBytes output{}; TokmonOwnedBytes drafts{}; TokmonOwnedBytes error{};
  const auto status = impl_->api.refract(impl_->instance,
      TokmonBytes{window.data(), window.size()},
      TokmonBytes{encoded_act.data(), encoded_act.size()}, &output, &drafts, &error);
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

}  // namespace tokmon
