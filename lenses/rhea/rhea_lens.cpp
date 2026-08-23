#include "lenses/rhea/rhea_lens.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <thread>

#include "lenses/common/http_client.hpp"
#include "lenses/common/secret_store.hpp"
#include "tokmon/json.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

struct ProviderPlan {
  std::string provider;
  std::string protocol;
  std::string model;
  std::string endpoint;
  std::string secret_ref;
  std::string auth{"protocol-default"};
  std::string curl_executable{"curl"};
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
  cbor::Value direct_arguments;
};

struct ParsedResponse {
  std::vector<std::string> reasoning_chunks;
  std::vector<std::string> content_chunks;
  std::map<std::string, ToolCall, std::less<>> tools;
  std::int64_t input_tokens{0};
  std::int64_t output_tokens{0};
};

struct SecretBuffer {
  std::string value;
  ~SecretBuffer() { std::fill(value.begin(), value.end(), '\0'); }
};

std::string string_field(const cbor::Value& value, const std::string_view key,
                         const std::string_view fallback = {}) {
  if (const auto* field = cbor::find(value, key))
    return std::string(field->as_string(fallback));
  return std::string(fallback);
}

double number_field(const cbor::Value& value, const std::string_view key) {
  const auto* field = cbor::find(value, key);
  if (!field) return 0.0;
  if (const auto* number = std::get_if<double>(&field->data)) return *number;
  if (const auto* integer = std::get_if<std::int64_t>(&field->data))
    return static_cast<double>(*integer);
  return 0.0;
}

std::optional<std::string> arithmetic_expression(const std::string& value) {
  for (std::size_t start = 0; start < value.size(); ++start) {
    const auto character = static_cast<unsigned char>(value[start]);
    if (std::isdigit(character) == 0 && value[start] != '.' && value[start] != '-' &&
        value[start] != '+') continue;
    char* left_end = nullptr;
    (void)std::strtod(value.c_str() + start, &left_end);
    if (!left_end || left_end == value.c_str() + start) continue;
    auto* operation = left_end;
    while (*operation != '\0' && std::isspace(static_cast<unsigned char>(*operation)) != 0)
      ++operation;
    if (*operation != '+' && *operation != '-' && *operation != '*' && *operation != '/')
      continue;
    auto* right = operation + 1;
    while (*right != '\0' && std::isspace(static_cast<unsigned char>(*right)) != 0) ++right;
    char* right_end = nullptr;
    (void)std::strtod(right, &right_end);
    if (!right_end || right_end == right) continue;
    return value.substr(start, static_cast<std::size_t>(right_end - value.c_str()) - start);
  }
  return std::nullopt;
}

bool local_endpoint(const std::string_view endpoint) {
  return endpoint.starts_with("http://127.0.0.1") ||
      endpoint.starts_with("http://localhost") || endpoint.starts_with("http://[::1]");
}

Result<ProviderPlan> provider_plan(const cbor::Value& value,
                                   const ProviderPlan* inherited = nullptr) {
  ProviderPlan plan = inherited ? *inherited : ProviderPlan{};
  if (const auto provider = string_field(value, "provider"); !provider.empty())
    plan.provider = provider;
  if (const auto protocol = string_field(value, "protocol"); !protocol.empty())
    plan.protocol = protocol;
  if (const auto model = string_field(value, "model"); !model.empty()) plan.model = model;
  if (const auto endpoint = string_field(value, "endpoint"); !endpoint.empty())
    plan.endpoint = endpoint;
  if (const auto reference = string_field(value, "secret_ref"); !reference.empty())
    plan.secret_ref = reference;
  if (const auto auth = string_field(value, "auth"); !auth.empty()) plan.auth = auth;
  if (const auto curl = string_field(value, "curl_executable"); !curl.empty())
    plan.curl_executable = curl;
  if (plan.provider.empty() || plan.model.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "model provider and model are required"));
  // Legacy direct Acts used provider as the protocol. Configured platforms
  // always supply protocol explicitly.
  if (plan.protocol.empty())
    plan.protocol = plan.provider == "anthropic" ? "anthropic" :
                    plan.provider == "gemini" ? "gemini" : "openai-compatible";
  if (plan.endpoint.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "model provider endpoint is required"));
  if (!plan.endpoint.starts_with("https://") && !local_endpoint(plan.endpoint))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "model endpoint must use HTTPS or loopback HTTP"));
  return plan;
}

Result<std::string> credential(const ProviderPlan& plan, const bool allow_anonymous,
                               const Act& act) {
  if (plan.auth == "none" || (allow_anonymous && local_endpoint(plan.endpoint)))
    return std::string{};
  if (const auto binding = string_field(act.parameters, "secret_binding");
      !binding.empty()) {
    return resolve_secret_binding(binding,
        string_field(act.parameters, "secret_purpose", "model-api"),
        act_secret_scope_hash(act), act.target, act.generation, act.epoch);
  }
  if (!plan.secret_ref.empty()) {
    if (plan.secret_ref != "model-provider/" + plan.provider)
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          "model provider SecretRef is outside its platform scope"));
    auto binding = create_secret_binding(plan.secret_ref, "model-api",
        act_secret_scope_hash(act), act.target, act.generation, act.epoch,
        std::chrono::minutes(2));
    if (!binding) return tl::unexpected(binding.error());
    return resolve_secret_binding(*binding, "model-api", act_secret_scope_hash(act),
                                  act.target, act.generation, act.epoch);
  }
  return tl::unexpected(make_error(ErrorCode::permission_denied,
      "model provider credentials require a one-shot Cista Secret binding"));
}

cbor::Value request_messages(const cbor::Value& parameters, const std::string& prompt) {
  if (const auto* messages = cbor::find(parameters, "messages");
      messages && messages->as_array()) return *messages;
  return cbor::Value::Array{cbor::object({{"role", "user"}, {"content", prompt}})};
}

cbor::Value request_body(const ProviderPlan& plan, const cbor::Value& parameters,
                         const std::string& prompt) {
  if (const auto* supplied = cbor::find(parameters, "request_body");
      supplied && supplied->is_map()) return *supplied;
  const auto max_tokens = cbor::find(parameters, "max_output_tokens")
      ? cbor::find(parameters, "max_output_tokens")->as_integer(4096) : 4096;
  auto messages = request_messages(parameters, prompt);
  if (plan.protocol == "anthropic") {
    auto body = cbor::object({{"model", plan.model}, {"messages", std::move(messages)},
        {"max_tokens", max_tokens}, {"stream", true}});
    if (const auto* tools = cbor::find(parameters, "tools"))
      (*body.as_map())["tools"] = *tools;
    return body;
  }
  if (plan.protocol == "gemini") {
    cbor::Value::Array contents;
    for (const auto& message : *messages.as_array()) {
      const auto role = string_field(message, "role") == "assistant" ? "model" : "user";
      const auto content = string_field(message, "content");
      contents.push_back(cbor::object({{"role", role},
          {"parts", cbor::Value::Array{cbor::object({{"text", content}})}}}));
    }
    auto body = cbor::object({{"contents", std::move(contents)}});
    if (const auto* tools = cbor::find(parameters, "tools"))
      (*body.as_map())["tools"] = *tools;
    return body;
  }
  auto body = cbor::object({{"model", plan.model}, {"messages", std::move(messages)},
      {"stream", true}, {"max_tokens", max_tokens}});
  if (const auto* thinking = cbor::find(parameters, "thinking");
      thinking && thinking->as_bool())
    (*body.as_map())["thinking"] = cbor::object({{"type", "enabled"}});
  if (const auto effort = string_field(parameters, "reasoning_effort"); !effort.empty())
    (*body.as_map())["reasoning_effort"] = effort;
  if (const auto* tools = cbor::find(parameters, "tools"))
    (*body.as_map())["tools"] = *tools;
  return body;
}

std::vector<std::pair<std::string, std::string>> headers(
    const ProviderPlan& plan, const std::string& secret) {
  std::vector<std::pair<std::string, std::string>> result{{"Content-Type", "application/json"},
      {"Accept", "text/event-stream, application/json"}};
  if (secret.empty()) return result;
  const auto auth = plan.auth == "protocol-default"
      ? (plan.protocol == "anthropic" ? "x-api-key" :
         plan.protocol == "gemini" ? "x-goog-api-key" : "bearer")
      : plan.auth;
  if (auth == "x-api-key") {
    result.emplace_back("x-api-key", secret);
    if (plan.protocol == "anthropic")
      result.emplace_back("anthropic-version", "2023-06-01");
  } else if (auth == "x-goog-api-key") {
    result.emplace_back("x-goog-api-key", secret);
  } else if (auth == "bearer") {
    result.emplace_back("Authorization", "Bearer " + secret);
  }
  return result;
}

const cbor::Value* array_item(const cbor::Value* value, const std::size_t index = 0) {
  if (!value || !value->as_array() || value->as_array()->size() <= index) return nullptr;
  return &(*value->as_array())[index];
}

void parse_usage(const cbor::Value& event, ParsedResponse& parsed) {
  const auto* usage = cbor::find(event, "usage");
  if (!usage) usage = cbor::find(event, "usageMetadata");
  if (!usage) return;
  const auto read = [usage](const std::initializer_list<std::string_view> keys) {
    for (const auto key : keys)
      if (const auto* value = cbor::find(*usage, key)) return value->as_integer();
    return std::int64_t{0};
  };
  parsed.input_tokens = std::max(parsed.input_tokens,
      read({"input_tokens", "prompt_tokens", "inputTokenCount", "promptTokenCount"}));
  parsed.output_tokens = std::max(parsed.output_tokens,
      read({"output_tokens", "completion_tokens", "outputTokenCount",
            "candidatesTokenCount"}));
}

void append_tool_delta(ParsedResponse& parsed, const cbor::Value& item,
                       const std::size_t fallback_index) {
  const auto index = cbor::find(item, "index")
      ? cbor::find(item, "index")->as_integer(static_cast<std::int64_t>(fallback_index))
      : static_cast<std::int64_t>(fallback_index);
  auto key = string_field(item, "id", std::to_string(index));
  auto& tool = parsed.tools[key];
  if (tool.id.empty()) tool.id = key;
  const auto* function = cbor::find(item, "function");
  if (!function) function = &item;
  if (const auto name = string_field(*function, "name"); !name.empty()) tool.name = name;
  if (const auto arguments = string_field(*function, "arguments"); !arguments.empty())
    tool.arguments.append(arguments);
  if (const auto* direct = cbor::find(*function, "args")) tool.direct_arguments = *direct;
  if (const auto* direct = cbor::find(*function, "input")) tool.direct_arguments = *direct;
}

void parse_event(const ProviderPlan& plan, const cbor::Value& event,
                 ParsedResponse& parsed) {
  parse_usage(event, parsed);
  if (const auto* error = cbor::find(event, "error")) {
    const auto message = string_field(*error, "message", cbor::diagnostic(*error));
    if (!message.empty()) parsed.content_chunks.push_back("[provider error] " + message);
    return;
  }

  if (plan.protocol == "anthropic") {
    if (const auto* delta = cbor::find(event, "delta")) {
      if (const auto thinking = string_field(*delta, "thinking"); !thinking.empty())
        parsed.reasoning_chunks.push_back(thinking);
      if (const auto text = string_field(*delta, "text"); !text.empty())
        parsed.content_chunks.push_back(text);
      if (const auto partial = string_field(*delta, "partial_json"); !partial.empty()) {
        const auto index = cbor::find(event, "index")
            ? cbor::find(event, "index")->as_integer() : 0;
        parsed.tools[std::to_string(index)].arguments.append(partial);
      }
    }
    if (const auto* block = cbor::find(event, "content_block"))
      append_tool_delta(parsed, *block, static_cast<std::size_t>(
          cbor::find(event, "index") ? cbor::find(event, "index")->as_integer() : 0));
    return;
  }

  if (plan.protocol == "gemini") {
    const auto* candidate = array_item(cbor::find(event, "candidates"));
    const auto* content = candidate ? cbor::find(*candidate, "content") : nullptr;
    const auto* parts = content ? cbor::find(*content, "parts") : nullptr;
    if (parts && parts->as_array()) {
      for (std::size_t index = 0; index < parts->as_array()->size(); ++index) {
        const auto& part = (*parts->as_array())[index];
        if (const auto text = string_field(part, "text"); !text.empty()) {
          if (cbor::find(part, "thought") && cbor::find(part, "thought")->as_bool())
            parsed.reasoning_chunks.push_back(text);
          else
            parsed.content_chunks.push_back(text);
        }
        if (const auto* call = cbor::find(part, "functionCall"))
          append_tool_delta(parsed, *call, index);
      }
    }
    return;
  }

  const auto* choice = array_item(cbor::find(event, "choices"));
  const auto* delta = choice ? cbor::find(*choice, "delta") : nullptr;
  if (!delta && choice) delta = cbor::find(*choice, "message");
  if (delta) {
    if (const auto reasoning = string_field(*delta, "reasoning_content",
        string_field(*delta, "reasoning")); !reasoning.empty())
      parsed.reasoning_chunks.push_back(reasoning);
    if (const auto content = string_field(*delta, "content"); !content.empty())
      parsed.content_chunks.push_back(content);
    if (const auto* calls = cbor::find(*delta, "tool_calls"); calls && calls->as_array())
      for (std::size_t index = 0; index < calls->as_array()->size(); ++index)
        append_tool_delta(parsed, (*calls->as_array())[index], index);
  } else {
    if (const auto content = string_field(event, "content",
        string_field(event, "text")); !content.empty()) parsed.content_chunks.push_back(content);
    if (const auto* call = cbor::find(event, "tool_call")) append_tool_delta(parsed, *call, 0);
  }
}

Result<ParsedResponse> parse_response(const ProviderPlan& plan,
                                      const std::string_view body) {
  ParsedResponse parsed;
  std::size_t cursor = 0;
  bool saw_sse = false;
  while (cursor <= body.size()) {
    const auto end = body.find('\n', cursor);
    auto line = body.substr(cursor, end == std::string_view::npos ? body.size() - cursor
                                                                  : end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.starts_with("data:")) {
      saw_sse = true;
      line.remove_prefix(5);
      while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      if (line != "[DONE]" && !line.empty()) {
        auto event = json::parse(line);
        if (!event) return tl::unexpected(event.error());
        parse_event(plan, *event, parsed);
      }
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1u;
  }
  if (!saw_sse) {
    auto event = json::parse(body);
    if (!event) return tl::unexpected(event.error());
    parse_event(plan, *event, parsed);
  }
  return parsed;
}

bool retryable_status(const int status) {
  return status == 408 || status == 409 || status == 425 || status == 429 || status >= 500;
}

}  // namespace

RheaLens::RheaLens() : LensBase(make_manifest("rhea", "Rhea / 模型网关神谕聚焦镜",
    {"model.catalog", "diagnostic.model"},
    {{"model.provider-*", "*"}, {"model.usage", "*"}, {"config.selected", "*"}},
    {{"model.call", "tokmon.model.call.v1"}},
    {"photon.emit", "io.http", "secret.bind", "log.write"})) {}

Result<void> RheaLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  if (auto result = surface.add("model.catalog", "local-deterministic", cbor::object({
      {"id", "local-deterministic"}, {"provider", "local"},
      {"context_window", 32768}, {"structured_tools", true}, {"healthy", true}}), 20);
      !result) return result;
  std::int64_t providers = 1;
  for (const auto& photon : photons.photons()) {
    if (photon.kind != "model.provider-configured" &&
        photon.kind != "model.provider-observed") continue;
    const auto provider = string_field(photon.payload, "provider");
    if (provider.empty()) continue;
    ++providers;
    if (auto result = surface.add("model.catalog", provider, photon.payload, 15); !result)
      return result;
  }
  const auto* failure = photons.latest("model.failed");
  const auto* usage = photons.latest("model.usage");
  return identify(surface, "diagnostic.model", cbor::object({
      {"providers", providers}, {"credential", "SecretRef/binding only"},
      {"last_failure", failure ? failure->payload : cbor::Value(nullptr)},
      {"last_usage", usage ? usage->payload : cbor::Value(nullptr)},
      {"streaming", true}, {"provider_broker", true}}));
}

Result<RefractionResult> RheaLens::refract(const PhotonWindow& photons, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (auto status = ready(); !status) return tl::unexpected(status.error());
  const auto* prompt_field = cbor::find(act.parameters, "prompt");
  const auto prompt = prompt_field ? std::string(prompt_field->as_string()) : std::string{};
  const auto model = string_field(act.parameters, "model");
  if ((prompt.empty() && !cbor::find(act.parameters, "messages")) || model.empty() ||
      act.idempotency_key.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "model.call requires model, prompt/messages and idempotency key"));
  if (cbor::find(act.parameters, "api_key") || cbor::find(act.parameters, "secret_value"))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "model.call cannot carry plaintext credentials"));
  const auto output_budget = cbor::find(act.parameters, "max_output_tokens")
      ? cbor::find(act.parameters, "max_output_tokens")->as_integer(4096) : 4096;
  if (output_budget <= 0 || output_budget > 1'000'000)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "model output token budget is invalid"));

  std::vector<PhotonId> emitted;
  const auto append = [&](std::string kind, std::string schema, cbor::Value payload)
      -> Result<void> {
    auto photon = beam.emit(std::move(kind), std::move(schema), std::move(payload));
    if (!photon) return tl::unexpected(photon.error());
    emitted.push_back(photon->id);
    return {};
  };
  const auto provider_name = string_field(act.parameters, "provider",
      model == "local-deterministic" ? "local" : "openai");
  if (auto result = append("model.requested", "tokmon.model.request.v1",
      cbor::object({{"model", model}, {"provider", provider_name},
                    {"idempotency_key", act.idempotency_key},
                    {"max_output_tokens", output_budget},
                    {"credential", "opaque-binding"}})); !result)
    return tl::unexpected(result.error());

  if (model == "local-deterministic") {
    if (auto result = append("model.dispatched", "tokmon.model.dispatch.v1",
        cbor::object({{"model", model}, {"provider", "local"}, {"attempt", 1}})); !result)
      return tl::unexpected(result.error());
    const auto* input = photons.latest("user.input");
    const auto* tool_result = photons.latest("tool.result");
    if (input && tool_result && tool_result->sequence > input->sequence) {
      const auto* value = cbor::find(tool_result->payload, "result");
      if (auto result = append("assistant.message", "tokmon.assistant.message.v1",
          cbor::object({{"text", "计算完成，结果是 " +
              (value ? cbor::diagnostic(*value) : std::string("未知"))},
                        {"model", model}, {"provider", "local"}})); !result)
        return tl::unexpected(result.error());
    } else if (const auto expression = arithmetic_expression(prompt)) {
      if (auto result = append("model.reasoning-chunk", "tokmon.model.reasoning.v1",
          cbor::object({{"text", "识别到算术意图，正在组合 calculate 透镜能力。"},
                        {"index", 0}})); !result)
        return tl::unexpected(result.error());
      if (auto result = append("model.tool-call", "tokmon.model.tool-call.v1",
          cbor::object({{"tool", "calculate"}, {"schema", "tokmon.math.calculate.v1"},
                        {"arguments", cbor::object({{"expression", *expression}})}})); !result)
        return tl::unexpected(result.error());
    } else if (auto result = append("assistant.message", "tokmon.assistant.message.v1",
        cbor::object({{"text", "已通过 A Lens to Them All 光路处理：" + prompt},
                      {"model", model}, {"provider", "local"}})); !result)
      return tl::unexpected(result.error());
    if (auto result = append("model.usage", "tokmon.model.usage.v1", cbor::object({
        {"input_tokens", static_cast<std::int64_t>(prompt.size() / 3u + 1u)},
        {"output_tokens", 1}, {"cached_tokens", 0}, {"provider", "local"}})); !result)
      return tl::unexpected(result.error());
    if (auto result = append("model.completed", "tokmon.model.completed.v1",
        cbor::object({{"model", model}, {"provider", "local"},
                      {"attempt", 1}, {"outcome", "complete"}})); !result)
      return tl::unexpected(result.error());
    return RefractionResult{.status = RefractionStatus::completed,
        .emitted = std::move(emitted), .detail = "local model completed"};
  }

  auto primary = provider_plan(act.parameters);
  if (!primary) return tl::unexpected(primary.error());
  std::vector<ProviderPlan> plans{*primary};
  if (const auto* fallbacks = cbor::find(act.parameters, "fallbacks");
      fallbacks && fallbacks->as_array()) {
    for (const auto& fallback : *fallbacks->as_array()) {
      auto plan = provider_plan(fallback, &*primary);
      if (!plan) return tl::unexpected(plan.error());
      plans.push_back(std::move(*plan));
    }
  }
  const auto allow_anonymous = cbor::find(act.parameters, "allow_anonymous") &&
      cbor::find(act.parameters, "allow_anonymous")->as_bool();
  const auto attempts_per_provider = std::clamp<std::int64_t>(
      cbor::find(act.parameters, "max_attempts")
          ? cbor::find(act.parameters, "max_attempts")->as_integer(2) : 2, 1, 5);
  const auto backoff_ms = std::clamp<std::int64_t>(
      cbor::find(act.parameters, "retry_backoff_ms")
          ? cbor::find(act.parameters, "retry_backoff_ms")->as_integer(100) : 100,
      0, 2'000);
  const auto first_token_timeout = std::chrono::milliseconds(std::clamp<std::int64_t>(
      cbor::find(act.parameters, "first_token_timeout_ms")
          ? cbor::find(act.parameters, "first_token_timeout_ms")->as_integer(10'000) : 10'000,
      1, act.timeout.count()));
  const auto idle_timeout = std::chrono::milliseconds(std::clamp<std::int64_t>(
      cbor::find(act.parameters, "idle_timeout_ms")
          ? cbor::find(act.parameters, "idle_timeout_ms")->as_integer(30'000) : 30'000,
      1, act.timeout.count()));

  Error last_error = make_error(ErrorCode::outcome_unknown,
                                "no model provider attempt completed", true);
  std::int64_t global_attempt = 0;
  for (const auto& plan : plans) {
    auto secret = credential(plan, allow_anonymous, act);
    if (!secret) { last_error = secret.error(); continue; }
    SecretBuffer secret_buffer{std::move(*secret)};
    for (std::int64_t attempt_index = 0; attempt_index < attempts_per_provider;
         ++attempt_index) {
      std::int64_t retry_after_ms = 0;
      ++global_attempt;
      if (beam.stop_requested())
        return tl::unexpected(make_error(ErrorCode::cancelled, "model call cancelled"));
      const auto body = json::stringify(request_body(plan, act.parameters, prompt));
      if (auto result = append("model.dispatched", "tokmon.model.dispatch.v1",
          cbor::object({{"model", plan.model}, {"provider", plan.provider},
              {"endpoint", plan.endpoint}, {"attempt", global_attempt},
              {"request_bytes", static_cast<std::int64_t>(body.size())},
              {"request_hash", sha256_hex(body)}})); !result)
        return tl::unexpected(result.error());
      auto response = perform_http(HttpRequest{.url = plan.endpoint,
          .headers = headers(plan, secret_buffer.value), .body = body, .timeout = act.timeout,
          .first_byte_timeout = first_token_timeout, .idle_timeout = idle_timeout,
          .max_response_bytes = 16u * 1024u * 1024u,
          .cwd = std::filesystem::current_path(), .executable = plan.curl_executable,
          .stop = beam.stop_token()});
      if (!response) {
        last_error = response.error();
      } else if (response->status < 200 || response->status >= 300) {
        if (!response->retry_after.empty()) {
          try { retry_after_ms = std::stoll(response->retry_after) * 1000; }
          catch (...) { retry_after_ms = 0; }
        }
        last_error = make_error(retryable_status(response->status)
            ? ErrorCode::io_error : ErrorCode::invalid_argument,
            "model provider HTTP " + std::to_string(response->status) + ": " +
                redact(response->body.substr(0, 1024)), retryable_status(response->status));
        if (!retryable_status(response->status)) attempt_index = attempts_per_provider;
      } else if (response->truncated) {
        last_error = make_error(ErrorCode::protocol_error,
                                "model response exceeded the configured limit");
      } else {
        auto parsed = parse_response(plan, response->body);
        if (!parsed) {
          last_error = parsed.error();
        } else if (parsed->reasoning_chunks.empty() && parsed->content_chunks.empty() &&
                   parsed->tools.empty()) {
          last_error = make_error(ErrorCode::protocol_error,
                                   "model response contained no content or tool call");
        } else {
          std::string final_text;
          for (const auto& chunk : parsed->reasoning_chunks)
            if (auto result = append("model.reasoning-chunk", "tokmon.model.reasoning.v1",
                cbor::object({{"text", chunk}, {"provider", plan.provider},
                              {"model", plan.model}, {"attempt", global_attempt},
                              {"visibility", "reasoning"}})); !result)
              return tl::unexpected(result.error());
          for (const auto& chunk : parsed->content_chunks) {
            final_text.append(chunk);
            if (auto result = append("model.content-chunk", "tokmon.model.chunk.v1",
                cbor::object({{"text", chunk}, {"provider", plan.provider},
                              {"model", plan.model}, {"attempt", global_attempt}})); !result)
              return tl::unexpected(result.error());
          }
          for (auto& [key, tool] : parsed->tools) {
            (void)key;
            cbor::Value arguments = tool.direct_arguments;
            if (!tool.arguments.empty()) {
              auto decoded = json::parse(tool.arguments);
              if (!decoded || !decoded->is_map()) {
                last_error = decoded ? make_error(ErrorCode::schema_mismatch,
                    "model tool arguments must be a JSON object") : decoded.error();
                continue;
              }
              arguments = std::move(*decoded);
            }
            if (tool.name.empty() || !arguments.is_map()) {
              last_error = make_error(ErrorCode::schema_mismatch,
                                       "model tool call is incomplete");
              continue;
            }
            if (auto result = append("model.tool-call", "tokmon.model.tool-call.v1",
                cbor::object({{"call_id", tool.id}, {"tool", tool.name},
                              {"arguments", std::move(arguments)},
                              {"provider", plan.provider}, {"model", plan.model}})); !result)
              return tl::unexpected(result.error());
          }
          if (!final_text.empty())
            if (auto result = append("assistant.message", "tokmon.assistant.message.v1",
                cbor::object({{"text", final_text}, {"provider", plan.provider},
                              {"model", plan.model}, {"attempt", global_attempt}})); !result)
              return tl::unexpected(result.error());
          if (auto result = append("model.usage", "tokmon.model.usage.v1",
              cbor::object({{"input_tokens", parsed->input_tokens},
                            {"output_tokens", parsed->output_tokens},
                            {"cost_usd", (static_cast<double>(parsed->input_tokens) *
                                number_field(act.parameters, "input_cost_per_million") +
                                static_cast<double>(parsed->output_tokens) *
                                number_field(act.parameters,
                                             "output_cost_per_million")) / 1'000'000.0},
                            {"provider", plan.provider}, {"model", plan.model},
                            {"attempt", global_attempt}})); !result)
            return tl::unexpected(result.error());
          if (auto result = append("model.completed", "tokmon.model.completed.v1",
              cbor::object({{"provider", plan.provider}, {"model", plan.model},
                            {"attempt", global_attempt},
                            {"response_hash", sha256_hex(response->body)},
                            {"outcome", "complete"}})); !result)
            return tl::unexpected(result.error());
          return RefractionResult{.status = RefractionStatus::completed,
              .emitted = std::move(emitted), .detail = "remote model completed"};
        }
      }
      if (attempt_index + 1 < attempts_per_provider &&
          (backoff_ms > 0 || retry_after_ms > 0)) {
        const auto exponential = std::min<std::int64_t>(
            30'000, backoff_ms * (std::int64_t{1} << attempt_index));
        const auto jitter = exponential * ((global_attempt * 37) % 26) / 100;
        const auto wait = std::chrono::milliseconds(
            std::max(retry_after_ms, exponential + jitter));
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < deadline && !beam.stop_requested())
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }
  if (auto result = append("model.failed", "tokmon.model.failed.v1", cbor::object({
      {"error_code", std::string(to_string(last_error.code))},
      {"error", redact(last_error.message)}, {"retryable", last_error.retryable},
      {"attempts", global_attempt}, {"outcome_known", false}})); !result)
    return tl::unexpected(result.error());
  return RefractionResult{.status = RefractionStatus::failed,
      .emitted = std::move(emitted), .detail = last_error.describe()};
}

}  // namespace tokmon::builtin
