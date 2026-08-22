#include "lenses/lemon/lemon_lens.hpp"

namespace tokmon::builtin {

LemonLens::LemonLens() : LensBase(make_manifest("lemon", "Lemon / 有界光纤波导",
    {"diagnostic.transport", "ui.stream"},
    {{"waveguide.frame-*", "*"}, {"waveguide.cursor-*", "*"},
     {"worker.progress", "*"}, {"model.chunk", "*"}},
    {{"waveguide.send-frame", "tokmon.waveguide.frame.v1"},
     {"waveguide.advance-cursor", "tokmon.waveguide.cursor.v1"},
     {"waveguide.reconnect", "tokmon.waveguide.reconnect.v1"}})) {}

Result<void> LemonLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  std::int64_t frames = 0; std::int64_t chunks = 0; std::uint64_t cursor = 0;
  cbor::Value::Array stream;
  for (const auto& photon : photons.photons()) {
    if (photon.kind.starts_with("waveguide.frame-")) ++frames;
    if (photon.kind == "model.chunk" || photon.kind == "worker.progress") {
      ++chunks;
      if (stream.size() == 32u) stream.erase(stream.begin());
      stream.push_back(cbor::object({
          {"sequence", static_cast<std::int64_t>(photon.sequence)},
          {"kind", photon.kind}, {"payload", photon.payload}}));
    }
    cursor = photon.sequence;
  }
  if (auto result = identify(surface, "diagnostic.transport", cbor::object({
      {"capacity", 256}, {"frames", frames}, {"stream_chunks", chunks},
      {"cursor", static_cast<std::int64_t>(cursor)}, {"backpressure", "bounded"},
      {"batch_limit", 32}})); !result) return result;
  return surface.add("ui.stream", "recent", std::move(stream), 10);
}

Result<RefractionResult> LemonLens::refract(const PhotonWindow& photons, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "waveguide.send-frame") {
    const auto* size = cbor::find(act.parameters, "payload_bytes");
    const auto* frame_kind = cbor::find(act.parameters, "frame_kind");
    if (!size || size->as_integer() < 0 || size->as_integer() > 1'048'576 ||
        !frame_kind || frame_kind->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
          "waveguide frame requires frame_kind and at most one MiB"));
  }
  if (act.kind == "waveguide.advance-cursor") {
    const auto* next = cbor::find(act.parameters, "cursor");
    if (!next || next->as_integer() < 0)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "cursor must be a non-negative integer"));
    std::int64_t current = 0;
    if (const auto* advanced = photons.latest("waveguide.cursor-advanced"))
      if (const auto* value = cbor::find(advanced->payload, "cursor"))
        current = value->as_integer();
    if (next->as_integer() <= current)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "waveguide cursor must move forward"));
  }
  if (act.kind == "waveguide.reconnect") {
    const auto* cursor = cbor::find(act.parameters, "cursor");
    if (!cursor || cursor->as_integer() < 0)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "reconnect requires the last cursor"));
  }
  const std::string kind = act.kind == "waveguide.send-frame" ? "waveguide.frame-sent" :
      act.kind == "waveguide.advance-cursor" ? "waveguide.cursor-advanced" :
                                               "waveguide.reconnect-requested";
  return emit(beam, kind, "tokmon.waveguide.result.v1", act.parameters);
}

}  // namespace tokmon::builtin
