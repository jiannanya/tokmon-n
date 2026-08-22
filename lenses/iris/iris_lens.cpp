#include "lenses/iris/iris_lens.hpp"

namespace tokmon::builtin {

IrisLens::IrisLens() : LensBase(make_manifest("iris", "Iris / 跨界折射镜",
    {"model.tools", "diagnostic.external"},
    {{"external.catalog-observed", "*"}, {"external.connection-*", "*"},
     {"external.schema-*", "*"}},
    {{"external.connect", "tokmon.external.connect.v1"},
     {"external.disconnect", "tokmon.external.disconnect.v1"},
     {"external.call", "tokmon.external.call.v1"},
     {"external.poll", "tokmon.external.poll.v1"}},
    {"photon.emit", "io.http", "log.write"})) {}

Result<void> IrisLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* catalog = photons.latest("external.catalog-observed");
  const auto* connected = photons.latest("external.connection-opened");
  if (catalog) {
    if (auto result = surface.add("model.tools", "external.catalog", catalog->payload, 20);
        !result) return result;
  }
  return identify(surface, "diagnostic.external", cbor::object({
      {"catalog_available", catalog != nullptr}, {"connected", connected != nullptr},
      {"remote_text_class", "data"},
      {"schema_hash_required", true}, {"endpoint_ref_only", true}}));
}

Result<RefractionResult> IrisLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto* endpoint = cbor::find(act.parameters, "endpoint_ref");
  if (act.kind == "external.connect" || act.kind == "external.call") {
    if (!endpoint || endpoint->as_string().empty() ||
        endpoint->as_string().find("://") != std::string_view::npos)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "external action requires an opaque endpoint_ref, not a raw URL"));
  }
  if (act.kind == "external.call") {
    const auto* operation = cbor::find(act.parameters, "operation");
    const auto* schema_hash = cbor::find(act.parameters, "schema_hash");
    if (!operation || operation->as_string().empty() || !schema_hash ||
        schema_hash->as_string().size() != 64u || act.idempotency_key.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "external.call requires operation, SHA-256 schema_hash and idempotency key"));
    const auto* response = cbor::find(act.parameters, "adapter_response");
    if (!response)
      return emit(beam, "external.outcome-unknown", "tokmon.external.result.v1",
          cbor::object({{"endpoint_ref", std::string(endpoint->as_string())},
                        {"operation", std::string(operation->as_string())},
                        {"idempotency_key", act.idempotency_key},
                        {"retry_safe", false},
                        {"reason", "no external adapter response was supplied"}}),
          "external outcome unknown");
    return emit(beam, "external.call-completed", "tokmon.external.result.v1",
        cbor::object({{"endpoint_ref", std::string(endpoint->as_string())},
                      {"operation", std::string(operation->as_string())},
                      {"schema_hash", std::string(schema_hash->as_string())},
                      {"result", *response}, {"remote_text_class", "data"},
                      {"idempotency_key", act.idempotency_key}}));
  }
  if ((act.kind == "external.disconnect" || act.kind == "external.poll") &&
      (!cbor::find(act.parameters, "connection_ref") ||
       cbor::find(act.parameters, "connection_ref")->as_string().empty()))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "external action requires connection_ref"));
  const std::string kind = act.kind == "external.connect" ? "external.connection-opened" :
      act.kind == "external.disconnect" ? "external.connection-closed" :
                                            "external.poll-completed";
  return emit(beam, kind, "tokmon.external.result.v1",
              cbor::object({{"request", act.parameters},
                            {"idempotency_key", act.idempotency_key}}));
}

}  // namespace tokmon::builtin
