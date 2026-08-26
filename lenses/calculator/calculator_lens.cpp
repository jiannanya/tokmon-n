#include "lenses/calculator/calculator_lens.hpp"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tokmon::builtin {
namespace {

Result<double> calculate(std::string expression) {
  expression.erase(std::remove_if(expression.begin(), expression.end(),
      [](unsigned char character) { return std::isspace(character) != 0; }), expression.end());
  const auto position = expression.find_first_of("+-*/", 1);
  if (position == std::string::npos)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "expected: number operator number"));
  char* end = nullptr;
  const auto left = std::strtod(expression.substr(0, position).c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(left))
    return tl::unexpected(make_error(ErrorCode::invalid_argument, "invalid left operand"));
  const auto right_text = expression.substr(position + 1);
  const auto right = std::strtod(right_text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(right))
    return tl::unexpected(make_error(ErrorCode::invalid_argument, "invalid right operand"));
  switch (expression[position]) {
    case '+': if (std::isfinite(left + right)) return left + right; break;
    case '-': if (std::isfinite(left - right)) return left - right; break;
    case '*': if (std::isfinite(left * right)) return left * right; break;
    case '/':
      if (right == 0.0)
        return tl::unexpected(make_error(ErrorCode::invalid_argument, "division by zero"));
      if (std::isfinite(left / right)) return left / right;
      break;
    default: return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                               "unsupported operator"));
  }
  return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                   "calculation result is not finite"));
}

}  // namespace

CalculatorLens::CalculatorLens() : LensBase([] {
  auto manifest = make_manifest("calculator", "Calculator / 动态透镜参考实现",
      {"model.tools"}, {{"user.input", "*"}},
      {{"tool.calculate", "tokmon.math.calculate.v1"}});
  manifest.provides_queries.push_back(OpticalQueryCapability{
      .capability = "math.evaluate", .request_schema = "tokmon.math.calculate.v1",
      .response_schema = "tokmon.math.result.v1", .deterministic = true,
      .priority = 50, .default_timeout = std::chrono::milliseconds(10),
      .max_timeout = std::chrono::milliseconds(100), .max_request_bytes = 4096,
      .max_response_bytes = 4096, .max_concurrent_queries = 4,
      .max_queries_per_beat = 1024, .cache = OpticalQueryCache::per_beat});
  return manifest;
}()) {}

Result<void> CalculatorLens::view(const PhotonWindow&, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  return surface.add("model.tools", "calculate", cbor::object({
      {"name", "calculate"}, {"description", "确定性计算二元算术表达式"},
      {"arguments_schema", "tokmon.math.calculate.v1"},
      {"target", manifest().id}}), 50);
}

Result<RefractionResult> CalculatorLens::refract(const PhotonWindow&, const Act& act,
                                                  RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto* expression = cbor::find(act.parameters, "expression");
  if (!expression || expression->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "calculator expression is required"));
  auto value = calculate(std::string(expression->as_string()));
  if (!value) return tl::unexpected(value.error());
  return emit(beam, "tool.result", "tokmon.math.result.v1", cbor::object({
      {"tool", "calculate"}, {"expression", std::string(expression->as_string())},
      {"result", *value}}));
}

bool CalculatorLens::supports_derive() const noexcept { return true; }
bool CalculatorLens::supports_query() const noexcept { return true; }

Result<cbor::Value> CalculatorLens::derive(const PhotonWindow&) {
  if (auto status = ready(); !status) return tl::unexpected(status.error());
  return cbor::object({{"implementation", "calculator-v1"},
                       {"operators", cbor::Value::Array{"+", "-", "*", "/"}}});
}

Result<cbor::Value> CalculatorLens::optical_query(
    const FrozenLensState& state, const std::string_view capability,
    const cbor::Value& parameters, const QueryBudget& budget) const {
  if (capability != "math.evaluate")
    return tl::unexpected(make_error(ErrorCode::unsupported,
                                     "unsupported Calculator query capability"));
  if (budget.expired())
    return tl::unexpected(make_error(ErrorCode::deadline_exceeded,
                                     "Calculator query deadline expired"));
  if (!cbor::find(state.data(), "implementation"))
    return tl::unexpected(make_error(ErrorCode::stale_generation,
                                     "Calculator frozen state is incomplete"));
  const auto* expression = cbor::find(parameters, "expression");
  if (!expression || expression->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "calculator expression is required"));
  auto value = calculate(std::string(expression->as_string()));
  if (!value) return tl::unexpected(value.error());
  return cbor::object({{"expression", std::string(expression->as_string())},
                       {"result", *value}, {"implementation", "calculator-v1"}});
}

}  // namespace tokmon::builtin
