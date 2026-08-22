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

CalculatorLens::CalculatorLens() : LensBase(make_manifest("calculator",
    "Calculator / 动态透镜参考实现", {"model.tools"}, {{"user.input", "*"}},
    {{"tool.calculate", "tokmon.math.calculate.v1"}})) {}

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

}  // namespace tokmon::builtin
