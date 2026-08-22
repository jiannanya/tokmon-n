#include "lenses/textus/textus_lens.hpp"

#include <algorithm>

#include "tokmon/hash.hpp"

namespace tokmon::builtin {

TextusLens::TextusLens() : LensBase(make_manifest("textus", "Textus / ModelSurface 光谱滤波镜",
    {"model.messages", "model.context", "diagnostic.context-budget"},
    {{"user.input", "*"}, {"assistant.message", "*"}, {"tool.result", "*"},
     {"summary.created", "*"}, {"model.budget", "*"}},
    {{"text.compact", "tokmon.text.compact.v1"},
     {"text.summarize", "tokmon.text.summarize.v1"}})) {}

Result<void> TextusLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array messages;
  std::vector<std::int64_t> token_costs;
  std::int64_t estimated = 0;
  std::int64_t budget = 32'768;
  if (const auto* configured = photons.latest("model.budget"))
    if (const auto* field = cbor::find(configured->payload, "max_tokens"))
      budget = std::max<std::int64_t>(1, field->as_integer(budget));
  for (const auto& photon : photons.photons()) {
    std::string role;
    if (photon.kind == "user.input") role = "user";
    else if (photon.kind == "assistant.message") role = "assistant";
    else if (photon.kind == "tool.result") role = "tool";
    else continue;
    const auto content = photon.kind == "tool.result" ? cbor::diagnostic(photon.payload)
                                                       : text(photon);
    const auto cost = static_cast<std::int64_t>(content.size() / 3 + 1);
    estimated += cost;
    token_costs.push_back(cost);
    messages.push_back(cbor::object({{"role", role}, {"content", content},
                                     {"source_photon", photon.id}}));
  }
  bool truncated = false;
  while (estimated > budget && messages.size() > 1u) {
    estimated -= token_costs.front();
    token_costs.erase(token_costs.begin());
    messages.erase(messages.begin());
    truncated = true;
  }
  if (auto result = surface.add("model.messages", "active-ray", std::move(messages), 50);
      !result) return result;
  if (const auto* summary = photons.latest("summary.created"))
    if (auto result = surface.add("model.context", "latest-summary", summary->payload, 30);
        !result) return result;
  return identify(surface, "diagnostic.context-budget", cbor::object({
      {"estimated_tokens", estimated}, {"budget_tokens", budget},
      {"truncated", truncated},
      {"truncation_reason", truncated ? "model_budget" : "none"},
      {"reducer_version", "textus-v1"},
      {"tail_sequence", photons.latest() ?
          static_cast<std::int64_t>(photons.latest()->sequence) : 0}}));
}

Result<RefractionResult> TextusLens::refract(const PhotonWindow& photons, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto first = photons.photons().empty() ? 0 : photons.photons().front().sequence;
  const auto last = photons.latest() ? photons.latest()->sequence : 0;
  const auto encoded_window = cbor::encode(to_cbor(photons));
  return emit(beam, "summary.created", "tokmon.text.summary.v1", cbor::object({
      {"covered_from", static_cast<std::int64_t>(first)},
      {"covered_to", static_cast<std::int64_t>(last)},
      {"covered_hash", sha256_hex(encoded_window)},
      {"strategy", act.kind == "text.compact" ? "compact" : "summarize"},
      {"summary", cbor::diagnostic(act.parameters)}}));
}

}  // namespace tokmon::builtin
