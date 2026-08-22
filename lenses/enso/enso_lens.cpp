#include "lenses/enso/enso_lens.hpp"

namespace tokmon::builtin {

EnsoLens::EnsoLens() : LensBase(make_manifest("enso", "Enso / 上下文全息定影镜",
    {"model.context", "ui.context-sources"},
    {{"instruction.observed", "*"}, {"skill.mounted", "*"}, {"memory.*", "*"},
     {"rag.document-*", "*"}, {"rag.index-*", "*"}},
    {{"context.retrieve", "tokmon.context.retrieve.v1"},
     {"memory.propose", "tokmon.memory.propose.v1"},
     {"rag.reindex", "tokmon.rag.reindex.v1"}})) {}

Result<void> EnsoLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array sources;
  for (const auto& photon : photons.photons()) {
    const bool instruction = photon.kind == "instruction.observed";
    const bool skill = photon.kind == "skill.mounted";
    const bool memory = photon.kind.starts_with("memory.");
    const bool rag = photon.kind.starts_with("rag.");
    if (!(instruction || skill || memory || rag)) continue;
    const auto content_text = cbor::diagnostic(photon.payload);
    const auto priority = instruction ? 400 : skill ? 300 : memory ? 200 : 100;
    sources.push_back(cbor::object({{"kind", photon.kind}, {"source", photon.id},
        {"hash", photon.hash},
        {"classification", instruction || skill ? "instruction" : "data"},
        {"trust", instruction ? "local-explicit" : skill ? "mounted" : "untrusted-data"},
        {"sensitivity", "normal"}, {"priority", priority},
        {"token_estimate", static_cast<std::int64_t>(content_text.size() / 3u + 1u)},
        {"content", photon.payload}}));
  }
  if (auto result = surface.add("model.context", "enso.sources", sources, 30); !result)
    return result;
  return surface.add("ui.context-sources", "enso.sources", std::move(sources), 10);
}

Result<RefractionResult> EnsoLens::refract(const PhotonWindow&, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "context.retrieve") {
    const auto* query = cbor::find(act.parameters, "query");
    if (!query || query->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "context.retrieve requires query"));
  }
  if (act.kind == "memory.propose" && !cbor::find(act.parameters, "content"))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "memory.propose requires content"));
  const std::string kind = act.kind == "context.retrieve" ? "context.retrieved" :
      act.kind == "memory.propose" ? "memory.proposed" : "rag.index-rebuilt";
  return emit(beam, kind, "tokmon.context.result.v1",
              cbor::object({{"source_class", "data"}, {"request", act.parameters},
                            {"policy_required", act.kind == "memory.propose"},
                            {"index_is_fact_source", false}}));
}

}  // namespace tokmon::builtin
