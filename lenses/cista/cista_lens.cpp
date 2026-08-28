#include "lenses/cista/cista_lens.hpp"

#include <algorithm>
#include <chrono>

#include "lenses/common/secret_store.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

std::string field(const cbor::Value& value, const std::string_view name,
                  const std::string_view fallback = {}) {
  const auto* item = cbor::find(value, name);
  return item ? std::string(item->as_string(fallback)) : std::string(fallback);
}

struct WipedText {
  std::string value;
  ~WipedText() { std::fill(value.begin(), value.end(), '\0'); }
};

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

CistaLens::CistaLens() : LensBase(make_manifest("cista", "Cista / Secret 遮光秘盒",
    {"act.secrets", "diagnostic.redaction"},
    {{"secret.ref-observed", "*"}, {"secret.*", "*"}, {"redaction.*", "*"}},
    {{"secret.create", "tokmon.secret.create.v1"},
     {"secret.read", "tokmon.secret.read.v1"},
     {"secret.rotate", "tokmon.secret.rotate.v1"},
     {"secret.delete", "tokmon.secret.delete.v1"},
     {"secret.list-metadata", "tokmon.secret.list-metadata.v1"},
     {"secret.bind", "tokmon.secret.bind.v1"},
     {"redaction.apply", "tokmon.redaction.apply.v1"}},
    {"photon.emit", "secret.bind", "os.keyring", "log.write"})) {}

Result<void> CistaLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto backend = std::string(keyring_backend());
  cbor::Value::Map references;
  for (const auto& photon : photons.photons()) {
    if (photon.kind != "secret.ref-observed" && photon.kind != "secret.created" &&
        photon.kind != "secret.rotated" && photon.kind != "secret.deleted") continue;
    const auto name = field(photon.payload, "name");
    const auto id = name.empty()
        ? field(photon.payload, "id")
        : model_credential_id(name);
    if (id.empty()) continue;
    if (photon.kind == "secret.deleted") { references.erase(id); continue; }
    references[id] = cbor::object({{"backend", backend}, {"id", id},
        {"purpose", field(photon.payload, "purpose")}, {"available", keyring_supported()},
        {"last_rotated_ms", cbor::find(photon.payload, "last_rotated_ms")
            ? cbor::find(photon.payload, "last_rotated_ms")->as_integer() : 0},
        {"plaintext", false}});
  }
  cbor::Value::Array items;
  for (auto& [_, value] : references) items.push_back(std::move(value));
  if (auto result = identify(surface, "act.secrets", cbor::object({
      {"references", std::move(items)}, {"backend", backend},
      {"plaintext_visible", false}, {"binding_max_lifetime_ms", 300000}})); !result)
    return result;
  return surface.add("diagnostic.redaction", "policy", cbor::object({
      {"schema_aware", true}, {"fail_closed", true}, {"plaintext_logged", false},
      {"binding_one_shot", true}}), 10);
}

Result<RefractionResult> CistaLens::refract(const PhotonWindow&, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto backend = std::string(keyring_backend());
  if (act.kind == "redaction.apply") {
    const auto* content = cbor::find(act.parameters, "content");
    if (!content)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "redaction.apply requires content"));
    return emit(beam, "redaction.applied", "tokmon.redaction.result.v1",
        cbor::object({{"content", redact_value(*content)},
                      {"plaintext_retained", false}, {"fail_closed", true}}));
  }

  if (act.kind == "secret.list-metadata") {
    auto metadata = keyring_list();
    if (!metadata) return tl::unexpected(metadata.error());
    cbor::Value::Array items;
    for (const auto& item : *metadata)
      items.push_back(cbor::object({{"backend", backend}, {"id", item.id},
          {"purpose", item.purpose}, {"last_rotated_ms", item.last_rotated_ms},
          {"available", true}, {"plaintext", false}}));
    return emit(beam, "secret.metadata-listed", "tokmon.secret.metadata.v1",
                cbor::object({{"items", std::move(items)}}));
  }

  const auto model_name = field(act.parameters, "name");
  const auto id = model_name.empty()
      ? field(act.parameters, "id")
      : model_credential_id(model_name);
  if (id.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "secret operation requires name or id"));
  if (act.kind == "secret.create" || act.kind == "secret.rotate") {
    const auto purpose = field(act.parameters, "purpose");
    const auto input_handle = field(act.parameters, "input_handle");
    if (purpose.empty() || input_handle.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "secret write requires purpose and input_handle"));
    auto supplied = consume_secret_input_handle(input_handle);
    if (!supplied) return tl::unexpected(supplied.error());
    WipedText value{std::move(*supplied)};
    if (auto stored = keyring_write(id, purpose, value.value); !stored)
      return tl::unexpected(stored.error());
    const auto rotated = now_ms();
    auto metadata = cbor::object({{"backend", backend}, {"purpose", purpose},
        {"last_rotated_ms", rotated}, {"available", true}, {"plaintext", false}});
    (*metadata.as_map())[model_name.empty() ? "id" : "name"] =
        model_name.empty() ? id : model_name;
    return emit(beam, act.kind == "secret.create" ? "secret.created" : "secret.rotated",
                "tokmon.secret.metadata.v1", std::move(metadata));
  }
  if (act.kind == "secret.delete") {
    if (auto removed = keyring_delete(id); !removed) return tl::unexpected(removed.error());
    auto metadata = cbor::object({{"backend", backend}, {"plaintext", false}});
    (*metadata.as_map())[model_name.empty() ? "id" : "name"] =
        model_name.empty() ? id : model_name;
    return emit(beam, "secret.deleted", "tokmon.secret.metadata.v1",
                std::move(metadata));
  }

  const auto purpose = field(act.parameters, "purpose");
  const auto act_hash = field(act.parameters, "act_hash");
  const auto consumer_target = field(act.parameters, "consumer_target");
  const auto* target_generation = cbor::find(act.parameters, "target_generation");
  const auto* lifetime = cbor::find(act.parameters, "lifetime_ms");
  if (purpose.empty() || act_hash.size() != 64u || consumer_target.empty() ||
      !target_generation || target_generation->as_integer() <= 0 || !lifetime ||
      lifetime->as_integer() <= 0 || lifetime->as_integer() > 300'000)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "secret binding requires purpose, act_hash, consumer_target, target_generation and bounded lifetime"));
  auto binding = create_secret_binding(id, purpose, act_hash, consumer_target,
      static_cast<GenerationId>(target_generation->as_integer()), act.epoch,
      std::chrono::milliseconds(lifetime->as_integer()));
  if (!binding) return tl::unexpected(binding.error());
  return emit(beam, "secret.bound", "tokmon.secret.binding.v1", cbor::object({
      {"binding_id", *binding}, {"secret_ref", id}, {"purpose", purpose},
      {"act_hash", act_hash}, {"consumer_target", consumer_target},
      {"target_generation", *target_generation}, {"lifetime_ms", *lifetime},
      {"epoch", static_cast<std::int64_t>(act.epoch)}, {"one_shot", true},
      {"plaintext", false}}));
}

}  // namespace tokmon::builtin
