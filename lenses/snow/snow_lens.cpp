#include "lenses/snow/snow_lens.hpp"

namespace tokmon::builtin {

SnowLens::SnowLens() : LensBase(make_manifest("snow", "Snow / 本地协议纯白投影幕",
    {"snow.protocol", "cli.output", "diagnostic.connection"},
    {{"ui.intent", "*"}, {"command.*", "*"}, {"snow.*", "*"}, {"system.started", "*"}},
    {{"snow.intent", "tokmon.snow.intent.v1"},
     {"command.invoke", "tokmon.command.invoke.v1"},
     {"snow.cancel", "tokmon.snow.cancel.v1"},
     {"snow.reconnect", "tokmon.snow.reconnect.v1"}})) {}

Result<void> SnowLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* tail = photons.latest();
  if (auto result = identify(surface, "snow.protocol", cbor::object({
      {"protocol_major", 1}, {"protocol_minor", 0}, {"canonical_cbor", true},
      {"max_frame_bytes", 16 * 1024 * 1024},
      {"cursor", tail ? static_cast<std::int64_t>(tail->sequence) : 0},
      {"client_can_commit_photon", false}})); !result) return result;
  const auto* assistant = photons.latest("assistant.message");
  if (auto result = surface.add("cli.output", "latest", cbor::object({
      {"text", assistant ? text(*assistant) : ""},
      {"source", assistant ? assistant->id : ""}, {"machine_stream_separate", true}}), 20);
      !result) return result;
  return surface.add("diagnostic.connection", "local-daemon", cbor::object({
      {"state", "connected"}, {"writer", "tokmond"},
      {"reconnect_from_cursor", true}}), 10);
}

Result<RefractionResult> SnowLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const std::string kind = act.kind == "command.invoke" ? "command.observed" :
      act.kind == "snow.cancel" ? "snow.cancel-observed" :
      act.kind == "snow.reconnect" ? "snow.reconnect-observed" : "ui.intent-observed";
  return emit(beam, kind, act.kind == "command.invoke" ? "tokmon.command.observed.v1" :
      "tokmon.snow.result.v1", cbor::object({
      {"intent", act.parameters}, {"request_id", act.id},
      {"committed_by_client", false}}));
}

}  // namespace tokmon::builtin
