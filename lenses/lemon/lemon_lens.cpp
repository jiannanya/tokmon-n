#include "lenses/lemon/lemon_lens.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tokmon::builtin {
namespace {

std::string string_field(const cbor::Value& value, const std::string_view key,
                         const std::string_view fallback = {}) {
  const auto* field = cbor::find(value, key);
  return field ? std::string(field->as_string(fallback)) : std::string(fallback);
}

std::string cursor_key(const cbor::Value& value) {
  return string_field(value, "conduit", "default") + ":" +
      string_field(value, "consumer", "default");
}

}  // namespace

LemonLens::LemonLens() : LensBase(make_manifest("lemon", "Lemon / 有界光纤波导",
    {"diagnostic.transport", "ui.stream"},
    {{"waveguide.frame-*", "*"}, {"waveguide.cursor-*", "*"},
     {"waveguide.subscription-*", "*"}, {"worker.progress", "*"},
     {"model.*chunk", "*"}},
    {{"waveguide.send-frame", "tokmon.waveguide.frame.v1"},
     {"waveguide.broadcast", "tokmon.waveguide.frame.v1"},
     {"waveguide.advance-cursor", "tokmon.waveguide.cursor.v1"},
     {"waveguide.subscribe", "tokmon.waveguide.subscription.v1"},
     {"waveguide.unsubscribe", "tokmon.waveguide.subscription.v1"},
     {"waveguide.reconnect", "tokmon.waveguide.reconnect.v1"}})) {}

Result<void> LemonLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  struct Conduit {
    std::int64_t frames{0};
    std::int64_t bytes{0};
    std::uint64_t tail{0};
    std::set<std::string> subscribers;
  };
  std::map<std::string, Conduit, std::less<>> conduits;
  std::map<std::string, std::int64_t, std::less<>> cursors;
  cbor::Value::Array stream;
  for (const auto& photon : photons.photons()) {
    if (photon.kind == "waveguide.frame-sent") {
      auto& conduit = conduits[string_field(photon.payload, "conduit", "default")];
      ++conduit.frames;
      conduit.tail = photon.sequence;
      if (const auto* bytes = cbor::find(photon.payload, "payload_bytes"))
        conduit.bytes += bytes->as_integer();
    } else if (photon.kind == "waveguide.subscription-opened") {
      conduits[string_field(photon.payload, "conduit", "default")].subscribers.insert(
          string_field(photon.payload, "consumer"));
    } else if (photon.kind == "waveguide.subscription-closed") {
      conduits[string_field(photon.payload, "conduit", "default")].subscribers.erase(
          string_field(photon.payload, "consumer"));
    } else if (photon.kind == "waveguide.cursor-advanced") {
      cursors[cursor_key(photon.payload)] = cbor::find(photon.payload, "cursor")
          ? cbor::find(photon.payload, "cursor")->as_integer() : 0;
    }
    if (photon.kind == "model.chunk" || photon.kind == "model.content-chunk" ||
        photon.kind == "model.reasoning-chunk" || photon.kind == "worker.progress") {
      if (stream.size() == 256u) stream.erase(stream.begin());
      stream.push_back(cbor::object({
          {"sequence", static_cast<std::int64_t>(photon.sequence)},
          {"kind", photon.kind}, {"payload", photon.payload}}));
    }
  }
  cbor::Value::Array conduit_surface;
  for (const auto& [name, conduit] : conduits) {
    std::int64_t minimum_cursor = static_cast<std::int64_t>(conduit.tail);
    bool has_cursor = false;
    for (const auto& [key, value] : cursors) {
      if (!key.starts_with(name + ":")) continue;
      minimum_cursor = std::min(minimum_cursor, value);
      has_cursor = true;
    }
    conduit_surface.push_back(cbor::object({{"name", name}, {"frames", conduit.frames},
        {"bytes", conduit.bytes}, {"tail", static_cast<std::int64_t>(conduit.tail)},
        {"subscribers", static_cast<std::int64_t>(conduit.subscribers.size())},
        {"lag", has_cursor ? static_cast<std::int64_t>(conduit.tail) - minimum_cursor : 0}}));
  }
  if (auto result = identify(surface, "diagnostic.transport", cbor::object({
      {"capacity", 2048}, {"max_frame_bytes", 1'048'576},
      {"conduits", std::move(conduit_surface)}, {"backpressure", "per-conduit"},
      {"durable_cursor", true}, {"broadcast", true}})); !result) return result;
  return surface.add("ui.stream", "recent", std::move(stream), 10);
}

Result<RefractionResult> LemonLens::refract(const PhotonWindow& photons, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto conduit = string_field(act.parameters, "conduit", "default");
  if (conduit.empty() || conduit.size() > 128u)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "waveguide conduit is invalid"));

  if (act.kind == "waveguide.send-frame" || act.kind == "waveguide.broadcast") {
    const auto* frame_kind = cbor::find(act.parameters, "frame_kind");
    const auto* payload = cbor::find(act.parameters, "payload");
    auto payload_bytes = cbor::find(act.parameters, "payload_bytes")
        ? cbor::find(act.parameters, "payload_bytes")->as_integer() :
          static_cast<std::int64_t>(payload ? cbor::encode(*payload).size() : 0u);
    if (!frame_kind || frame_kind->as_string().empty() || payload_bytes < 0 ||
        payload_bytes > 1'048'576)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
          "waveguide frame requires frame_kind and at most one MiB"));
    const auto capacity = std::clamp<std::int64_t>(
        cbor::find(act.parameters, "capacity")
            ? cbor::find(act.parameters, "capacity")->as_integer(2048) : 2048, 1, 65'536);
    std::int64_t pending = 0;
    for (const auto& photon : photons.photons())
      if (photon.kind == "waveguide.frame-sent" &&
          string_field(photon.payload, "conduit", "default") == conduit) ++pending;
    if (pending >= capacity)
      return emit(beam, "waveguide.backpressured", "tokmon.waveguide.result.v1",
          cbor::object({{"conduit", conduit}, {"capacity", capacity},
                        {"queued", pending}, {"frame_rejected", true}}),
          "conduit backpressured");
    auto result = act.parameters;
    (*result.as_map())["conduit"] = conduit;
    (*result.as_map())["payload_bytes"] = payload_bytes;
    (*result.as_map())["delivery"] = act.kind == "waveguide.broadcast" ? "broadcast" : "direct";
    return emit(beam, "waveguide.frame-sent", "tokmon.waveguide.result.v1",
                std::move(result));
  }

  if (act.kind == "waveguide.advance-cursor") {
    const auto* next = cbor::find(act.parameters, "cursor");
    const auto consumer = string_field(act.parameters, "consumer", "default");
    if (!next || next->as_integer() < 0 || consumer.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "cursor and consumer are required"));
    std::int64_t current = -1;
    for (auto iterator = photons.photons().rbegin(); iterator != photons.photons().rend();
         ++iterator) {
      if (iterator->kind == "waveguide.cursor-advanced" &&
          string_field(iterator->payload, "conduit", "default") == conduit &&
          string_field(iterator->payload, "consumer", "default") == consumer) {
        current = cbor::find(iterator->payload, "cursor")
            ? cbor::find(iterator->payload, "cursor")->as_integer() : -1;
        break;
      }
    }
    if (next->as_integer() <= current)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "waveguide cursor must move forward"));
    return emit(beam, "waveguide.cursor-advanced", "tokmon.waveguide.result.v1",
        cbor::object({{"conduit", conduit}, {"consumer", consumer}, {"cursor", *next}}));
  }

  if (act.kind == "waveguide.subscribe" || act.kind == "waveguide.unsubscribe") {
    const auto consumer = string_field(act.parameters, "consumer");
    const auto mode = string_field(act.parameters, "mode", "durable");
    if (consumer.empty() || (mode != "durable" && mode != "ephemeral" && mode != "group"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "subscription requires consumer and valid mode"));
    return emit(beam, act.kind == "waveguide.subscribe"
        ? "waveguide.subscription-opened" : "waveguide.subscription-closed",
        "tokmon.waveguide.subscription-result.v1",
        cbor::object({{"conduit", conduit}, {"consumer", consumer}, {"mode", mode}}));
  }

  const auto* cursor = cbor::find(act.parameters, "cursor");
  if (!cursor || cursor->as_integer() < 0)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "reconnect requires the last cursor"));
  const auto earliest = photons.photons().empty() ? 0u : photons.photons().front().sequence;
  const auto tail = photons.latest() ? photons.latest()->sequence : 0u;
  return emit(beam, "waveguide.reconnect-requested", "tokmon.waveguide.result.v1",
      cbor::object({{"conduit", conduit}, {"cursor", *cursor},
          {"tail", static_cast<std::int64_t>(tail)},
          {"snapshot_required", static_cast<std::uint64_t>(cursor->as_integer()) < earliest}}));
}

}  // namespace tokmon::builtin
