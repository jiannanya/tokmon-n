#include "lenses/iris/iris_lens.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "lenses/common/http_client.hpp"
#include "lenses/common/process_runner.hpp"
#include "lenses/common/secret_store.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/ids.hpp"
#include "tokmon/json.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

std::string string_field(const cbor::Value& value, const std::string_view key,
                         const std::string_view fallback = {}) {
  const auto* field = cbor::find(value, key);
  return field ? std::string(field->as_string(fallback)) : std::string(fallback);
}

Result<std::vector<std::string>> argv_field(const cbor::Value& value) {
  const auto* field = cbor::find(value, "argv");
  if (!field || !field->as_array() || field->as_array()->empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "stdio endpoint requires argv"));
  std::vector<std::string> result;
  result.reserve(field->as_array()->size());
  for (const auto& item : *field->as_array()) {
    if (!std::holds_alternative<std::string>(item.data) || item.as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "stdio endpoint argv must contain strings"));
    result.emplace_back(item.as_string());
  }
  return result;
}

const cbor::Value* connection(const PhotonWindow& photons,
                              const std::string_view reference) {
  for (auto iterator = photons.photons().rbegin(); iterator != photons.photons().rend();
       ++iterator) {
    if (string_field(iterator->payload, "connection_ref") != reference) continue;
    if (iterator->kind == "external.connection-closed") return nullptr;
    if (iterator->kind == "external.connection-opened") return &iterator->payload;
  }
  return nullptr;
}

Result<cbor::Value> parse_stdio_response(const std::string_view output,
                                         const std::string_view framing,
                                         const std::string_view request_id) {
  if (framing == "content-length") {
    std::size_t cursor = 0;
    std::optional<cbor::Value> fallback;
    while (cursor < output.size()) {
      const auto separator = output.find("\r\n\r\n", cursor);
      if (separator == std::string_view::npos) break;
      const auto header = output.substr(cursor, separator - cursor);
      const auto marker = header.find("Content-Length:");
      if (marker == std::string_view::npos)
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "LSP response lacks Content-Length"));
      const auto line_end = header.find("\r\n", marker);
      auto digits = header.substr(marker + 15u,
          line_end == std::string_view::npos ? std::string_view::npos
                                             : line_end - marker - 15u);
      while (!digits.empty() && digits.front() == ' ') digits.remove_prefix(1);
      std::size_t length = 0;
      try { length = static_cast<std::size_t>(std::stoull(std::string(digits))); }
      catch (...) {
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "LSP Content-Length is invalid"));
      }
      const auto body_start = separator + 4u;
      if (body_start + length > output.size())
        return tl::unexpected(make_error(ErrorCode::protocol_error,
                                         "LSP response body is incomplete"));
      auto parsed = json::parse(output.substr(body_start, length));
      if (!parsed) return tl::unexpected(parsed.error());
      if (cbor::find(*parsed, "result") || cbor::find(*parsed, "error")) fallback = *parsed;
      if (const auto* id = cbor::find(*parsed, "id");
          id && id->as_string() == request_id) return *parsed;
      cursor = body_start + length;
    }
    if (fallback) return *fallback;
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "LSP response contains no matching request id"));
  }
  std::size_t cursor = 0;
  Result<cbor::Value> last = tl::unexpected(
      make_error(ErrorCode::protocol_error, "stdio response contains no JSON-RPC message"));
  while (cursor <= output.size()) {
    const auto end = output.find('\n', cursor);
    auto line = output.substr(cursor, end == std::string_view::npos
        ? output.size() - cursor : end - cursor);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) {
      auto parsed = json::parse(line);
      if (parsed) {
        last = *parsed;
        if (const auto* id = cbor::find(*parsed, "id");
            id && id->as_string() == request_id) return *parsed;
      }
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1u;
  }
  return last;
}

Result<cbor::Value> call_stdio(const cbor::Value& endpoint,
                              const cbor::Value& request,
                              const Act& act, RefractionBeam& beam) {
  auto argv = argv_field(endpoint);
  if (!argv) return tl::unexpected(argv.error());
  const auto cwd = string_field(endpoint, "cwd", std::filesystem::current_path().string());
  const auto framing = string_field(endpoint, "framing",
      string_field(endpoint, "protocol") == "lsp" ? "content-length" : "newline");
  const auto frame = [&](const cbor::Value& message) {
    const auto encoded = json::stringify(message);
    return framing == "content-length"
        ? "Content-Length: " + std::to_string(encoded.size()) + "\r\n\r\n" + encoded
        : encoded + "\n";
  };
  std::string input;
  if (string_field(endpoint, "protocol") == "lsp") {
    const auto request_id = string_field(request, "id");
    input += frame(cbor::object({{"jsonrpc", "2.0"},
        {"id", request_id + ":initialize"}, {"method", "initialize"},
        {"params", cbor::object({{"processId", nullptr},
          {"rootUri", string_field(endpoint, "root_uri")},
          {"capabilities", cbor::Value::Map{}}})}}));
    input += frame(cbor::object({{"jsonrpc", "2.0"}, {"method", "initialized"},
                                 {"params", cbor::Value::Map{}}}));
    if (const auto* document = cbor::find(endpoint, "document"); document && !document->is_null())
      input += frame(cbor::object({{"jsonrpc", "2.0"},
          {"method", "textDocument/didOpen"},
          {"params", cbor::object({{"textDocument", *document}})}}));
    input += frame(request);
    input += frame(cbor::object({{"jsonrpc", "2.0"},
        {"id", request_id + ":shutdown"}, {"method", "shutdown"},
        {"params", nullptr}}));
    input += frame(cbor::object({{"jsonrpc", "2.0"}, {"method", "exit"},
                                 {"params", nullptr}}));
  } else {
    input = frame(request);
  }
  auto output = run_process(ProcessRequest{.argv = std::move(*argv), .cwd = cwd,
      .timeout = act.timeout, .max_output_bytes = 16u * 1024u * 1024u,
      .stdin_text = input, .stop = beam.stop_token()});
  if (!output) return tl::unexpected(output.error());
  if (output->timed_out)
    return tl::unexpected(make_error(ErrorCode::timeout, "external stdio call timed out", true));
  if (output->cancelled)
    return tl::unexpected(make_error(ErrorCode::cancelled, "external stdio call cancelled"));
  if (output->exit_code != 0)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
        "external stdio process failed: " + redact(output->stderr_text)));
  return parse_stdio_response(output->stdout_text, framing,
                              string_field(request, "id"));
}

bool loopback_url(const std::string_view url) {
  return is_loopback_network_url(url);
}

bool loopback_http_url(const std::string_view url) {
  return url.starts_with("http://") && loopback_url(url);
}

bool loopback_websocket_url(const std::string_view url) {
  return url.starts_with("ws://") && loopback_url(url);
}

Result<cbor::Value> call_http(const cbor::Value& endpoint,
                             const cbor::Value& request,
                             const Act& act, RefractionBeam& beam) {
  const auto url = string_field(endpoint, "url");
  if (url.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "HTTP endpoint requires url"));
  std::vector<std::pair<std::string, std::string>> headers{
      {"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
  if (!url.starts_with("https://") && !loopback_http_url(url))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "external HTTP requires HTTPS or loopback HTTP"));
  std::string secret;
  if (const auto binding = string_field(act.parameters, "secret_binding"); !binding.empty()) {
    auto resolved = resolve_secret_binding(binding,
        string_field(act.parameters, "secret_purpose", "external-api"),
        act_secret_scope_hash(act), act.target, act.generation, act.epoch);
    if (!resolved) return tl::unexpected(resolved.error());
    secret = std::move(*resolved);
    headers.emplace_back(string_field(endpoint, "credential_header", "Authorization"),
        string_field(endpoint, "credential_prefix", "Bearer ") + secret);
  } else if (!loopback_http_url(url)) {
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "external HTTP credentials require a one-shot Cista binding"));
  }
  auto response = perform_http(HttpRequest{.url = url, .headers = std::move(headers),
      .body = json::stringify(request), .timeout = act.timeout,
      .max_response_bytes = 16u * 1024u * 1024u,
      .response_mode = HttpResponseMode::server_sent_events,
      .stop = beam.stop_token()});
  std::fill(secret.begin(), secret.end(), '\0');
  if (!response) return tl::unexpected(response.error());
  if (response->status < 200 || response->status >= 300)
    return tl::unexpected(make_error(response->status >= 500 ? ErrorCode::io_error
                                                              : ErrorCode::protocol_error,
        "external HTTP " + std::to_string(response->status) + ": " +
            redact(response->body.substr(0, 1024)), response->status >= 500));
  if (response->server_sent_events) {
    Result<cbor::Value> fallback = tl::unexpected(make_error(
        ErrorCode::protocol_error, "SSE response contains no JSON-RPC message"));
    for (const auto& frame : response->events) {
      if (frame.data == "[DONE]" || frame.data.empty()) continue;
      auto parsed = json::parse(frame.data);
      if (!parsed) return tl::unexpected(parsed.error());
      fallback = *parsed;
      const auto* id = cbor::find(*parsed, "id");
      if (id && id->as_string() == string_field(request, "id")) return *parsed;
    }
    return fallback;
  }
  return json::parse(response->body);
}

Result<cbor::Value> call_websocket(const cbor::Value& endpoint,
                                  const cbor::Value& request,
                                  const Act& act, RefractionBeam& beam) {
  const auto url = string_field(endpoint, "url");
  if (url.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "WebSocket endpoint requires url"));
  if (!url.starts_with("wss://") && !loopback_websocket_url(url))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "external WebSocket requires WSS or loopback WS"));
  std::vector<std::pair<std::string, std::string>> headers;
  if (const auto protocol = string_field(endpoint, "subprotocol"); !protocol.empty())
    headers.emplace_back("Sec-WebSocket-Protocol", protocol);
  std::string secret;
  if (const auto binding = string_field(act.parameters, "secret_binding");
      !binding.empty()) {
    auto resolved = resolve_secret_binding(binding,
        string_field(act.parameters, "secret_purpose", "external-api"),
        act_secret_scope_hash(act), act.target, act.generation, act.epoch);
    if (!resolved) return tl::unexpected(resolved.error());
    secret = std::move(*resolved);
    headers.emplace_back(string_field(endpoint, "credential_header", "Authorization"),
        string_field(endpoint, "credential_prefix", "Bearer ") + secret);
  } else if (!loopback_websocket_url(url)) {
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "external WebSocket credentials require a one-shot Cista binding"));
  }
  auto response = perform_websocket(WebSocketRequest{.url = url,
      .headers = std::move(headers), .message = json::stringify(request),
      .timeout = act.timeout, .max_response_bytes = 16u * 1024u * 1024u,
      .stop = beam.stop_token()});
  std::fill(secret.begin(), secret.end(), '\0');
  if (!response) return tl::unexpected(response.error());
  if (response->binary)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
        "JSON-RPC WebSocket response must be a text message"));
  return json::parse(response->message);
}

Result<cbor::Value> rpc_result(const cbor::Value& response) {
  if (!response.is_map())
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "JSON-RPC response must be an object"));
  if (const auto* error = cbor::find(response, "error"))
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "JSON-RPC error: " + redact(cbor::diagnostic(*error))));
  const auto* result = cbor::find(response, "result");
  if (!result)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "JSON-RPC response lacks result"));
  return *result;
}

}  // namespace

IrisLens::IrisLens() : LensBase(make_manifest("iris", "Iris / 跨界折射镜",
    {"model.tools", "diagnostic.external", "workspace.diagnostics"},
    {{"external.catalog-observed", "*"}, {"external.connection-*", "*"},
     {"external.schema-*", "*"}, {"lsp.diagnostics-observed", "*"}},
    {{"external.connect", "tokmon.external.connect.v1"},
     {"external.disconnect", "tokmon.external.disconnect.v1"},
     {"external.call", "tokmon.external.call.v1"},
     {"external.poll", "tokmon.external.poll.v1"},
     {"lsp.request", "tokmon.lsp.request.v1"},
     {"external.serve", "tokmon.external.serve.v1"}},
    {"photon.emit", "io.http", "io.process", "log.write"})) {}

Result<void> IrisLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Map endpoint_state;
  cbor::Value::Array tools;
  cbor::Value::Array diagnostics;
  for (const auto& photon : photons.photons()) {
    if (photon.kind == "external.connection-opened") {
      const auto reference = string_field(photon.payload, "connection_ref");
      endpoint_state[reference] = cbor::object({
          {"connection_ref", string_field(photon.payload, "connection_ref")},
          {"endpoint_ref", string_field(photon.payload, "endpoint_ref")},
          {"transport", string_field(photon.payload, "transport")},
          {"protocol", string_field(photon.payload, "protocol")}, {"healthy", true}});
    }
    if (photon.kind == "external.connection-closed")
      endpoint_state.erase(string_field(photon.payload, "connection_ref"));
    if (photon.kind == "external.catalog-observed") {
      const auto* catalog_tools = cbor::find(photon.payload, "tools");
      if (catalog_tools && catalog_tools->as_array())
        tools.insert(tools.end(), catalog_tools->as_array()->begin(),
                     catalog_tools->as_array()->end());
    }
    if (photon.kind == "lsp.diagnostics-observed") diagnostics.push_back(photon.payload);
  }
  if (auto result = surface.add("model.tools", "external.catalog", std::move(tools), 20);
      !result) return result;
  if (auto result = surface.add("workspace.diagnostics", "lsp", std::move(diagnostics), 20);
      !result) return result;
  cbor::Value::Array endpoints;
  for (auto& [_, endpoint] : endpoint_state) endpoints.push_back(std::move(endpoint));
  return identify(surface, "diagnostic.external", cbor::object({
      {"endpoints", std::move(endpoints)}, {"remote_text_class", "data"},
      {"schema_hash_required", true}, {"endpoint_ref_only", true},
      {"mcp_client", true}, {"mcp_server", true}, {"lsp_client", true},
      {"network_library", "chhttp"},
      {"transports", cbor::Value::Array{"stdio", "http", "websocket"}}}));
}

Result<RefractionResult> IrisLens::refract(const PhotonWindow& photons, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "external.connect") {
    const auto endpoint_ref = string_field(act.parameters, "endpoint_ref");
    const auto transport = string_field(act.parameters, "transport");
    const auto protocol = string_field(act.parameters, "protocol");
    if (endpoint_ref.empty() || endpoint_ref.find("://") != std::string::npos ||
        (transport != "stdio" && transport != "http" && transport != "websocket") ||
        (protocol != "mcp" && protocol != "lsp" && protocol != "jsonrpc"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "external.connect requires opaque endpoint_ref, transport and protocol"));
    if (transport == "stdio") {
      auto argv = argv_field(act.parameters);
      if (!argv) return tl::unexpected(argv.error());
      (void)argv;
    } else {
      const auto url = string_field(act.parameters, "url");
      if (url.empty())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "HTTP endpoint requires url"));
      const bool allowed = transport == "websocket"
          ? url.starts_with("wss://") || loopback_websocket_url(url)
          : url.starts_with("https://") || loopback_http_url(url);
      if (!allowed)
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            transport == "websocket"
                ? "external WebSocket requires WSS or loopback WS"
                : "external HTTP requires HTTPS or loopback HTTP"));
    }
    for (const auto* forbidden : {"credential_env", "api_key", "token", "password",
                                  "secret_value", "secret_binding"})
      if (cbor::find(act.parameters, forbidden))
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            "external.connect cannot persist credentials or bindings"));
    auto payload = cbor::object({{"endpoint_ref", endpoint_ref}, {"transport", transport},
        {"protocol", protocol},
        {"argv", cbor::find(act.parameters, "argv")
            ? *cbor::find(act.parameters, "argv") : cbor::Value(nullptr)},
        {"cwd", string_field(act.parameters, "cwd")},
        {"framing", string_field(act.parameters, "framing",
            protocol == "lsp" ? "content-length" : "newline")},
        {"url", string_field(act.parameters, "url")},
        {"subprotocol", string_field(act.parameters, "subprotocol")},
        {"credential_header", string_field(act.parameters, "credential_header", "Authorization")},
        {"credential_prefix", string_field(act.parameters, "credential_prefix", "Bearer ")},
        {"secret_ref", string_field(act.parameters, "secret_ref")},
        {"root_uri", string_field(act.parameters, "root_uri")},
        {"document", cbor::find(act.parameters, "document")
            ? *cbor::find(act.parameters, "document") : cbor::Value(nullptr)}});
    (*payload.as_map())["connection_ref"] = "connection-" +
        sha256_hex(endpoint_ref + act.idempotency_key).substr(0, 24);
    return emit(beam, "external.connection-opened", "tokmon.external.connection.v1",
                std::move(payload));
  }

  if (act.kind == "external.disconnect") {
    const auto reference = string_field(act.parameters, "connection_ref");
    if (reference.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "external.disconnect requires connection_ref"));
    return emit(beam, "external.connection-closed", "tokmon.external.connection.v1",
                cbor::object({{"connection_ref", reference}, {"history_deleted", false}}));
  }

  if (act.kind == "external.poll") {
    const auto reference = string_field(act.parameters, "connection_ref");
    const auto* endpoint = connection(photons, reference);
    if (reference.empty() || !endpoint)
      return tl::unexpected(make_error(ErrorCode::not_found,
                                       "external connection was not found"));
    const auto protocol = string_field(*endpoint, "protocol");
    const auto operation = string_field(act.parameters, "operation",
        protocol == "mcp" ? "tools/list" : protocol == "lsp" ? "workspace/symbol" : "ping");
    auto arguments = cbor::find(act.parameters, "arguments")
        ? *cbor::find(act.parameters, "arguments") : cbor::Value(cbor::Value::Map{});
    if (protocol == "lsp" && operation == "workspace/symbol" && !cbor::find(arguments, "query"))
      (*arguments.as_map())["query"] = "";
    const auto request = cbor::object({{"jsonrpc", "2.0"}, {"id", act.id},
        {"method", operation}, {"params", std::move(arguments)}});
    const auto started = std::chrono::steady_clock::now();
    const auto transport = string_field(*endpoint, "transport");
    Result<cbor::Value> response = transport == "stdio"
        ? call_stdio(*endpoint, request, act, beam)
        : transport == "websocket"
            ? call_websocket(*endpoint, request, act, beam)
            : call_http(*endpoint, request, act, beam);
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (!response) {
      return emit(beam, "external.health-observed", "tokmon.external.health.v1",
          cbor::object({{"connection_ref", reference}, {"healthy", false},
              {"latency_ms", latency},
              {"error_code", std::string(to_string(response.error().code))},
              {"error", redact(response.error().message)}, {"reconnected", false}}),
          "external endpoint health check failed");
    }
    auto result = rpc_result(*response);
    if (!result)
      return emit(beam, "external.health-observed", "tokmon.external.health.v1",
          cbor::object({{"connection_ref", reference}, {"healthy", false},
              {"latency_ms", latency}, {"error_code", "protocol_error"},
              {"error", result.error().message}, {"reconnected", true}}),
          "external endpoint returned a protocol error");
    std::vector<PhotonId> emitted;
    if (operation == "tools/list" && result->is_map()) {
      auto catalog = beam.emit("external.catalog-observed", "tokmon.external.catalog.v1",
          cbor::object({{"connection_ref", reference}, {"operation", operation},
              {"schema_hash", sha256_hex(cbor::encode(*result))},
              {"tools", cbor::find(*result, "tools")
                  ? *cbor::find(*result, "tools") : cbor::Value::Array{}},
              {"catalog", *result}, {"remote_text_class", "data"}}));
      if (!catalog) return tl::unexpected(catalog.error());
      emitted.push_back(catalog->id);
    }
    auto healthy = beam.emit("external.health-observed", "tokmon.external.health.v1",
        cbor::object({{"connection_ref", reference}, {"healthy", true},
            {"latency_ms", latency}, {"protocol", protocol},
            {"operation", operation}, {"reconnected", true},
            {"capability_hash", sha256_hex(cbor::encode(*result))}}));
    if (!healthy) return tl::unexpected(healthy.error());
    emitted.push_back(healthy->id);
    return RefractionResult{.status = RefractionStatus::completed,
        .emitted = std::move(emitted), .detail = "external endpoint is healthy"};
  }

  if (act.kind == "external.serve") {
    const auto* argv = cbor::find(act.parameters, "argv");
    if (!argv || !argv->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "external.serve requires a supervised server argv"));
    return emit(beam, "external.server-launch-requested", "tokmon.external.server.v1",
        cbor::object({{"request", act.parameters}, {"supervisor", "styx"}}));
  }

  const auto reference = string_field(act.parameters, "connection_ref");
  const auto* endpoint = connection(photons, reference);
  if (!endpoint) endpoint = &act.parameters;
  const auto operation = string_field(act.parameters, "operation");
  const auto schema_hash = string_field(act.parameters, "schema_hash");
  if (operation.empty() || schema_hash.size() != 64u || act.idempotency_key.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "external call requires operation, SHA-256 schema_hash and idempotency key"));
  const auto request = cbor::object({{"jsonrpc", "2.0"}, {"id", act.id},
      {"method", operation},
      {"params", cbor::find(act.parameters, "arguments")
          ? *cbor::find(act.parameters, "arguments") : cbor::Value::Map{}}});
  const auto started = std::chrono::steady_clock::now();
  const auto transport = string_field(*endpoint, "transport");
  Result<cbor::Value> response = transport == "stdio"
      ? call_stdio(*endpoint, request, act, beam)
      : transport == "websocket"
          ? call_websocket(*endpoint, request, act, beam)
          : call_http(*endpoint, request, act, beam);
  const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count();
  if (!response) {
    const auto outcome_unknown = response.error().code == ErrorCode::timeout ||
        response.error().code == ErrorCode::io_error;
    return emit(beam, outcome_unknown ? "external.outcome-unknown" : "external.call-failed",
        "tokmon.external.result.v1", cbor::object({
            {"connection_ref", reference}, {"operation", operation},
            {"schema_hash", schema_hash},
            {"error_code", std::string(to_string(response.error().code))},
            {"error", redact(response.error().message)},
            {"retry_safe", false}, {"outcome_known", !outcome_unknown}}),
        outcome_unknown ? "external outcome unknown" : "external call failed");
  }
  auto result = rpc_result(*response);
  if (!result)
    return emit(beam, "external.call-failed", "tokmon.external.result.v1",
        cbor::object({{"connection_ref", reference}, {"operation", operation},
                      {"error", result.error().message}, {"outcome_known", true}}),
        "external call failed");

  std::vector<PhotonId> emitted;
  if (act.kind == "lsp.request") {
    const auto normalized = operation.starts_with("textDocument/")
        ? operation.substr(std::string_view("textDocument/").size()) : operation;
    auto observed = beam.emit("lsp.result-observed", "tokmon.lsp.result.v1",
        cbor::object({{"connection_ref", reference}, {"operation", operation},
          {"result_kind", normalized}, {"result", *result},
          {"lifecycle", cbor::Value::Array{"initialize", "initialized", "didOpen",
                                             "request", "shutdown", "exit"}},
          {"remote_text_class", "data"}}));
    if (!observed) return tl::unexpected(observed.error());
    emitted.push_back(observed->id);
  }
  if ((operation == "tools/list" || operation == "resources/list" ||
       operation == "prompts/list") && result->is_map()) {
    auto catalog = beam.emit("external.catalog-observed", "tokmon.external.catalog.v1",
        cbor::object({{"connection_ref", reference}, {"operation", operation},
                      {"schema_hash", schema_hash},
                      {"tools", cbor::find(*result, "tools")
                          ? *cbor::find(*result, "tools") : cbor::Value::Array{}},
                      {"catalog", *result}, {"remote_text_class", "data"}}));
    if (!catalog) return tl::unexpected(catalog.error());
    emitted.push_back(catalog->id);
  }
  if (act.kind == "lsp.request" && operation == "textDocument/publishDiagnostics") {
    auto diagnostic = beam.emit("lsp.diagnostics-observed", "tokmon.lsp.diagnostics.v1",
                                *result);
    if (!diagnostic) return tl::unexpected(diagnostic.error());
    emitted.push_back(diagnostic->id);
  }
  auto completed = beam.emit("external.call-completed", "tokmon.external.result.v1",
      cbor::object({{"connection_ref", reference}, {"operation", operation},
                    {"schema_hash", schema_hash}, {"result", *result},
                    {"latency_ms", latency},
                    {"remote_text_class", "data"},
                    {"idempotency_key", act.idempotency_key}}));
  if (!completed) return tl::unexpected(completed.error());
  emitted.push_back(completed->id);
  return RefractionResult{.status = RefractionStatus::completed,
      .emitted = std::move(emitted), .detail = "external call completed"};
}

}  // namespace tokmon::builtin
