#include "lenses/techor/techor_lens.hpp"

namespace tokmon::builtin {

TechorLens::TechorLens() : LensBase(make_manifest("techor", "Techor / Tool 光能作动镜",
    {"model.tools", "act.candidates", "diagnostic.tool-decode"},
    {{"model.tool-call", "*"}, {"tool.result", "*"}, {"code.frame", "*"}},
    {{"tool.decode", "tokmon.tool.decode.v1"}})) {}

Result<void> TechorLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  if (auto result = surface.add("model.tools", "calculate", cbor::object({
      {"name", "calculate"}, {"description", "计算一个二元算术表达式"},
      {"arguments_schema", "tokmon.math.calculate.v1"},
      {"target", "org.tokmon.lens.calculator"}}), 40); !result) return result;
  const auto* call = photons.latest("model.tool-call");
  const auto* result = photons.latest("tool.result");
  if (!call || (result && result->sequence > call->sequence)) return {};
  const auto* tool = cbor::find(call->payload, "tool");
  const auto* schema = cbor::find(call->payload, "schema");
  const auto* arguments = cbor::find(call->payload, "arguments");
  if (!tool || tool->as_string() != "calculate" || !schema ||
      schema->as_string() != "tokmon.math.calculate.v1" || !arguments ||
      !arguments->is_map()) {
    return surface.add("diagnostic.tool-decode", call->id,
        cbor::object({{"status", "rejected"}, {"reason", "unknown tool or schema"}}), 100);
  }
  auto act = propose(*call, "tool.calculate", "tokmon.math.calculate.v1",
      "org.tokmon.lens.calculator", *arguments);
  if (auto added = surface.add("act.candidates", act.id, to_cbor(act), 50); !added)
    return added;
  return surface.propose(std::move(act));
}

Result<RefractionResult> TechorLens::refract(const PhotonWindow&, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  return emit(beam, "tool.decoded", "tokmon.tool.decoded.v1",
              cbor::object({
                  {"normalized", act.parameters},
                  {"epoch", static_cast<std::int64_t>(act.epoch)}}));
}

}  // namespace tokmon::builtin
