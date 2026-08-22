#include "lenses/rhea/rhea_lens.hpp"

#include <cctype>
#include <algorithm>
#include <cstdlib>

namespace tokmon::builtin {
namespace {

bool arithmetic_expression(std::string value) {
  value.erase(std::remove_if(value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; }), value.end());
  const auto op = value.find_first_of("+-*/", 1);
  if (op == std::string::npos) return false;
  char* end = nullptr;
  (void)std::strtod(value.substr(0, op).c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  (void)std::strtod(value.substr(op + 1).c_str(), &end);
  return end != nullptr && *end == '\0';
}

}  // namespace

RheaLens::RheaLens() : LensBase(make_manifest("rhea", "Rhea / 模型网关神谕聚焦镜",
    {"model.catalog", "diagnostic.model"},
    {{"model.provider-*", "*"}, {"model.usage", "*"}, {"config.selected", "*"}},
    {{"model.call", "tokmon.model.call.v1"}},
    {"photon.emit", "io.http", "secret.bind", "log.write"})) {}

Result<void> RheaLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* failure = photons.latest("model.failed");
  if (auto result = surface.add("model.catalog", "local-deterministic", cbor::object({
      {"id", "local-deterministic"}, {"context_window", 32768},
      {"structured_tools", true}, {"healthy", failure == nullptr}}), 20); !result) return result;
  return identify(surface, "diagnostic.model", cbor::object({
      {"provider", "local-deterministic"}, {"credential", "SecretRef only"}}));
}

Result<RefractionResult> RheaLens::refract(const PhotonWindow& photons, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (auto status = ready(); !status) return tl::unexpected(status.error());
  const auto* prompt_field = cbor::find(act.parameters, "prompt");
  const auto* model_field = cbor::find(act.parameters, "model");
  if (!prompt_field || !std::holds_alternative<std::string>(prompt_field->data) ||
      !model_field || model_field->as_string() != "local-deterministic" ||
      act.idempotency_key.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "model.call requires local-deterministic model, prompt and idempotency key"));
  if (const auto* budget = cbor::find(act.parameters, "max_output_tokens");
      budget && (budget->as_integer() <= 0 || budget->as_integer() > 32'768))
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "model output token budget is invalid"));
  std::vector<PhotonId> emitted;
  const auto append = [&](std::string kind, std::string schema, cbor::Value payload)
      -> Result<void> {
    auto photon = beam.emit(std::move(kind), std::move(schema), std::move(payload));
    if (!photon) return tl::unexpected(photon.error());
    emitted.push_back(photon->id); return {};
  };
  if (auto result = append("model.requested", "tokmon.model.request.v1",
      cbor::object({{"model", "local-deterministic"},
                    {"idempotency_key", act.idempotency_key},
                    {"credential", "SecretRef only"}})); !result)
    return tl::unexpected(result.error());
  if (auto result = append("model.dispatched", "tokmon.model.dispatch.v1",
      cbor::object({{"model", "local-deterministic"},
                    {"request_hash", act.idempotency_key}})); !result)
    return tl::unexpected(result.error());
  const auto* input = photons.latest("user.input");
  const auto* tool_result = photons.latest("tool.result");
  const auto prompt = std::string(prompt_field ? prompt_field->as_string() : std::string_view{});
  if (input && tool_result && tool_result->sequence > input->sequence) {
    const auto* value = cbor::find(tool_result->payload, "result");
    if (auto result = append("assistant.message", "tokmon.assistant.message.v1",
        cbor::object({{"text", "计算完成，结果是 " +
            (value ? cbor::diagnostic(*value) : std::string("未知"))},
                      {"model", "local-deterministic"}})); !result)
      return tl::unexpected(result.error());
  } else if (arithmetic_expression(prompt)) {
    if (auto result = append("model.tool-call", "tokmon.model.tool-call.v1",
        cbor::object({{"tool", "calculate"}, {"schema", "tokmon.math.calculate.v1"},
                      {"arguments", cbor::object({{"expression", prompt}})}})); !result)
      return tl::unexpected(result.error());
  } else if (auto result = append("assistant.message", "tokmon.assistant.message.v1",
      cbor::object({{"text", "已通过 A Lens to Them All 光路处理：" + prompt},
                    {"model", "local-deterministic"}})); !result)
    return tl::unexpected(result.error());
  if (auto result = append("model.usage", "tokmon.model.usage.v1",
      cbor::object({{"input_tokens", static_cast<std::int64_t>(prompt.size() / 3 + 1)},
                    {"output_tokens", 1}, {"cached_tokens", 0}})); !result)
    return tl::unexpected(result.error());
  return RefractionResult{.status = RefractionStatus::completed,
                           .emitted = std::move(emitted), .detail = "model completed"};
}

}  // namespace tokmon::builtin
