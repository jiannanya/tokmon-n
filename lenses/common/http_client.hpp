#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon::builtin {

struct HttpRequest {
  std::string url;
  std::string method{"POST"};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::milliseconds timeout{30'000};
  std::chrono::milliseconds first_byte_timeout{0};
  std::chrono::milliseconds idle_timeout{0};
  std::size_t max_response_bytes{8u * 1024u * 1024u};
  std::filesystem::path cwd{std::filesystem::current_path()};
  std::string executable{"curl"};
  std::stop_token stop;
};

struct HttpResponse {
  int status{0};
  std::string body;
  std::string stderr_text;
  bool truncated{false};
  std::string retry_after;
};

[[nodiscard]] Result<HttpResponse> perform_http(HttpRequest request);

}  // namespace tokmon::builtin
