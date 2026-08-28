#include "lenses/common/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <chhttp/chhttp.hpp>

namespace tokmon::builtin {
namespace {

Result<chhttp::Headers> transport_headers(
    const std::vector<std::pair<std::string, std::string>>& values) {
  chhttp::Headers result;
  for (const auto& [name, value] : values) {
    if (name.empty() || name.find_first_of("\r\n:") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "HTTP header contains invalid characters"));
    result.add(name, value);
  }
  return result;
}

Error transport_error(const chhttp::ErrorInfo& error,
                      const std::string_view operation) {
  ErrorCode code = ErrorCode::io_error;
  bool retryable = true;
  switch (error.code) {
    case chhttp::Error::cancelled:
      code = ErrorCode::cancelled;
      retryable = false;
      break;
    case chhttp::Error::timeout:
      code = ErrorCode::timeout;
      break;
    case chhttp::Error::invalid_argument:
    case chhttp::Error::invalid_url:
      code = ErrorCode::invalid_argument;
      retryable = false;
      break;
    case chhttp::Error::protocol:
    case chhttp::Error::body_too_large:
    case chhttp::Error::redirect_limit:
    case chhttp::Error::websocket_handshake:
    case chhttp::Error::websocket_closed:
      code = ErrorCode::protocol_error;
      retryable = false;
      break;
    case chhttp::Error::tls_configuration:
    case chhttp::Error::tls_verification:
      code = ErrorCode::permission_denied;
      retryable = false;
      break;
    default:
      break;
  }
  auto detail = error.message.empty() ? std::string("unknown transport error")
                                      : error.message;
  return make_error(code, std::string(operation) + " failed: " + detail,
                    retryable);
}

bool event_stream_content_type(std::string content_type) {
  std::ranges::transform(content_type, content_type.begin(),
                         [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                         });
  const auto separator = content_type.find(';');
  if (separator != std::string::npos) content_type.resize(separator);
  while (!content_type.empty() && std::isspace(
             static_cast<unsigned char>(content_type.back())) != 0)
    content_type.pop_back();
  const auto first = content_type.find_first_not_of(" \t");
  if (first != std::string::npos) content_type.erase(0, first);
  return content_type == "text/event-stream";
}

chhttp::ClientOptions client_options(const std::chrono::milliseconds timeout,
                                     const std::size_t max_response_bytes) {
  chhttp::ClientOptions options;
  options.connect_timeout = timeout;
  options.read_timeout = timeout;
  options.write_timeout = timeout;
  options.max_response_body_size = max_response_bytes;
  options.follow_redirects = true;
  options.keep_alive = true;
  options.tls.verify_peer = true;
  options.tls.use_system_certificates = true;
  return options;
}

std::shared_ptr<chhttp::Client> http_client(const std::string_view url) {
  const auto scheme_end = url.find("://");
  const auto authority_end = scheme_end == std::string_view::npos
      ? std::string_view::npos : url.find_first_of("/?#", scheme_end + 3u);
  const std::string origin(url.substr(0, authority_end));
  static std::mutex mutex;
  static std::unordered_map<std::string, std::shared_ptr<chhttp::Client>> clients;
  std::scoped_lock lock(mutex);
  if (const auto found = clients.find(origin); found != clients.end())
    return found->second;
  auto created = std::make_shared<chhttp::Client>(origin, client_options(
      std::chrono::seconds(60), 128u * 1024u * 1024u));
  if (clients.size() >= 64u) clients.erase(clients.begin());
  clients.emplace(origin, created);
  return created;
}

}  // namespace

bool is_loopback_network_url(const std::string_view url) noexcept {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return false;
  const auto equals_ascii = [](const std::string_view left,
                               const std::string_view right) {
    return left.size() == right.size() &&
        std::ranges::equal(left, right, [](const char a, const char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
              std::tolower(static_cast<unsigned char>(b));
        });
  };
  const auto scheme = url.substr(0, scheme_end);
  if (!equals_ascii(scheme, "http") && !equals_ascii(scheme, "ws")) return false;
  auto authority = url.substr(scheme_end + 3u);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
  std::string_view host;
  if (authority.front() == '[') {
    const auto closing = authority.find(']');
    if (closing == std::string_view::npos) return false;
    host = authority.substr(1u, closing - 1u);
    const auto remainder = authority.substr(closing + 1u);
    if (!remainder.empty() && remainder.front() != ':') return false;
  } else {
    const auto colon = authority.rfind(':');
    host = colon == std::string_view::npos ? authority : authority.substr(0, colon);
    if (host.find(':') != std::string_view::npos) return false;
  }
  return host == "127.0.0.1" || equals_ascii(host, "localhost") || host == "::1";
}

Result<HttpResponse> perform_http(HttpRequest request) {
  if (request.url.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "HTTP URL is required"));
  if (request.method.empty() || request.timeout <= std::chrono::milliseconds::zero() ||
      request.max_response_bytes == 0u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "HTTP method and positive limits are required"));
  if (request.stop.stop_requested())
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     "HTTP request cancelled"));

  auto headers = transport_headers(request.headers);
  if (!headers) return tl::unexpected(headers.error());

  chhttp::Request outgoing;
  outgoing.method = std::move(request.method);
  outgoing.target = request.url;
  outgoing.headers = std::move(*headers);
  outgoing.body = std::move(request.body);

  HttpResponse result;
  chhttp::ErrorInfo parser_error;
  std::optional<Error> event_error;
  chhttp::SseParser parser({.max_line_size = request.max_response_bytes,
                            .max_event_size = request.max_response_bytes});
  parser.on_message([&](const chhttp::SseEvent& event) {
    ServerSentEvent translated{.data = event.data, .event = event.event,
        .id = event.id, .retry = event.retry};
    result.events.push_back(translated);
    if (request.on_server_sent_event && !event_error) {
      auto observed = request.on_server_sent_event(translated);
      if (!observed) event_error = observed.error();
    }
  });

  chhttp::RequestOptions options;
  options.stop_token = request.stop;
  options.total_timeout = request.timeout;
  options.connect_timeout = request.timeout;
  options.read_timeout = request.timeout;
  options.write_timeout = request.timeout;
  options.max_response_body_size = request.max_response_bytes;
  if (request.first_byte_timeout > std::chrono::milliseconds::zero())
    options.first_body_byte_timeout = request.first_byte_timeout;
  if (request.idle_timeout > std::chrono::milliseconds::zero())
    options.idle_timeout = request.idle_timeout;
  if (request.response_mode == HttpResponseMode::server_sent_events) {
    options.on_response_head = [&](const chhttp::ResponseHead& head) {
      result.server_sent_events = head.status >= 200 && head.status < 300 &&
          event_stream_content_type(head.headers.get("Content-Type"));
      return true;
    };
    options.on_data = [&](const std::string_view bytes) {
      if (!result.server_sent_events) {
        result.body.append(bytes);
        return true;
      }
      parser_error = parser.feed(bytes);
      return !parser_error && !event_error;
    };
  }

  auto response = http_client(request.url)->request(std::move(outgoing),
                                                    std::move(options));
  if (event_error) return tl::unexpected(std::move(*event_error));
  if (parser_error)
    return tl::unexpected(transport_error(parser_error, "SSE parsing"));
  if (!response)
    return tl::unexpected(transport_error(response.error(), "HTTP transport"));

  result.status = response->status;
  result.retry_after = response->headers.get("Retry-After");
  if (request.response_mode == HttpResponseMode::buffered)
    result.body = std::move(response->body);
  if (result.server_sent_events) {
    if (const auto error = parser.finish(); error)
      return tl::unexpected(transport_error(error, "SSE parsing"));
  } else if (request.response_mode == HttpResponseMode::server_sent_events &&
             result.body.starts_with("data:")) {
    // Some compatible gateways omit the media type. Keep compatibility while
    // still using chhttp's incremental WHATWG parser rather than line splitting.
    if (const auto error = parser.feed(result.body); error)
      return tl::unexpected(transport_error(error, "SSE parsing"));
    if (event_error) return tl::unexpected(std::move(*event_error));
    if (const auto error = parser.finish(); error)
      return tl::unexpected(transport_error(error, "SSE parsing"));
    if (!result.events.empty()) {
      result.server_sent_events = true;
      result.body.clear();
    }
  }
  return result;
}

Result<WebSocketResponse> perform_websocket(WebSocketRequest request) {
  if (request.url.empty() || request.timeout <= std::chrono::milliseconds::zero() ||
      request.max_response_bytes == 0u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
        "WebSocket URL and positive limits are required"));
  if (request.stop.stop_requested())
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     "WebSocket request cancelled"));
  auto headers = transport_headers(request.headers);
  if (!headers) return tl::unexpected(headers.error());
  auto connected = chhttp::AsyncWebSocketClient::connect(
      request.url, std::move(*headers),
      client_options(request.timeout, request.max_response_bytes)).get();
  if (!connected)
    return tl::unexpected(transport_error(connected.error(),
                                          "WebSocket connection"));
  auto socket = *connected;
  if (request.stop.stop_requested()) {
    socket->close(1000, "cancelled").get();
    return tl::unexpected(make_error(ErrorCode::cancelled,
                                     "WebSocket request cancelled"));
  }
  if (!socket->send_text(request.message).get()) {
    socket->close(1011, "send failed").get();
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "WebSocket send failed", true));
  }
  for (;;) {
    auto incoming = socket->read().get();
    if (!incoming) {
      socket->close(1011, "read failed").get();
      return tl::unexpected(transport_error(incoming.error(),
                                            "WebSocket read"));
    }
    if (incoming->type == chhttp::WebSocket::MessageType::ping ||
        incoming->type == chhttp::WebSocket::MessageType::pong)
      continue;
    if (incoming->type == chhttp::WebSocket::MessageType::close)
      return tl::unexpected(make_error(ErrorCode::protocol_error,
                                       "WebSocket closed before a response"));
    if (incoming->data.size() > request.max_response_bytes) {
      socket->close(1009, "response too large").get();
      return tl::unexpected(make_error(ErrorCode::protocol_error,
          "WebSocket response exceeded the configured limit"));
    }
    WebSocketResponse response{.message = std::move(incoming->data),
        .subprotocol = socket->subprotocol(),
        .binary = incoming->type == chhttp::WebSocket::MessageType::binary};
    socket->close(1000, "complete").get();
    return response;
  }
}

}  // namespace tokmon::builtin
