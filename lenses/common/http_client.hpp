#pragma once

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon::builtin {

enum class HttpResponseMode { buffered, server_sent_events };

struct ServerSentEvent {
  std::string data;
  std::string event;
  std::string id;
  std::optional<std::chrono::milliseconds> retry;
};

struct HttpRequest {
  std::string url;
  std::string method{"POST"};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds first_byte_timeout{0};
  std::chrono::milliseconds idle_timeout{0};
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  HttpResponseMode response_mode{HttpResponseMode::buffered};
  std::stop_token stop;
};

struct HttpResponse {
  int status{0};
  std::string body;
  std::string retry_after;
  bool server_sent_events{false};
  std::vector<ServerSentEvent> events;
};

[[nodiscard]] Result<HttpResponse> perform_http(HttpRequest request);
[[nodiscard]] bool is_loopback_network_url(std::string_view url) noexcept;

struct WebSocketRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string message;
  std::chrono::milliseconds timeout{30'000};
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  std::stop_token stop;
};

struct WebSocketResponse {
  std::string message;
  std::string subprotocol;
  bool binary{false};
};

[[nodiscard]] Result<WebSocketResponse> perform_websocket(
    WebSocketRequest request);

}  // namespace tokmon::builtin
