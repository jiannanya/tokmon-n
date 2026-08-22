#include "lenses/common/http_client.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

#include "lenses/common/process_runner.hpp"

namespace tokmon::builtin {
namespace {

std::string curl_config_string(const std::string_view value) {
  std::string result;
  result.reserve(value.size() + 2u);
  result.push_back('"');
  for (const auto character : value) {
    switch (character) {
      case '\\': result.append("\\\\"); break;
      case '"': result.append("\\\""); break;
      case '\n': result.append("\\n"); break;
      case '\r': result.append("\\r"); break;
      case '\t': result.append("\\t"); break;
      default: result.push_back(character); break;
    }
  }
  result.push_back('"');
  return result;
}

}  // namespace

Result<HttpResponse> perform_http(HttpRequest request) {
  if (request.url.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "HTTP URL is required"));
  if (request.method.empty() || request.timeout <= std::chrono::milliseconds::zero() ||
      request.max_response_bytes == 0u)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "HTTP method and positive limits are required"));

  constexpr std::string_view marker = "\n__TOKMON_HTTP_STATUS__:";
  constexpr std::string_view retry_marker = "\n__TOKMON_RETRY_AFTER__:";
  std::string config;
  config.append("silent\nshow-error\nno-buffer\nlocation\n");
  config.append("request = ").append(curl_config_string(request.method)).append("\n");
  config.append("url = ").append(curl_config_string(request.url)).append("\n");
  config.append("connect-timeout = ")
      .append(std::to_string(std::max<std::int64_t>(1, request.timeout.count() / 1000)))
      .append("\n");
  config.append("max-time = ")
      .append(std::to_string(std::max<std::int64_t>(1, request.timeout.count() / 1000)))
      .append("\n");
  for (const auto& [name, value] : request.headers) {
    if (name.empty() || name.find_first_of("\r\n:") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "HTTP header contains invalid characters"));
    config.append("header = ")
        .append(curl_config_string(name + ": " + value)).append("\n");
  }
  if (!request.body.empty())
    config.append("data = ").append(curl_config_string(request.body)).append("\n");
  config.append("write-out = ")
      .append(curl_config_string(std::string(marker) + "%{http_code}" +
                                 std::string(retry_marker) + "%header{retry-after}\n"))
      .append("\n");

  auto output = run_process(ProcessRequest{
      .argv = {std::move(request.executable), "--config", "-"},
      .cwd = std::move(request.cwd), .timeout = request.timeout,
      .first_output_timeout = request.first_byte_timeout,
      .idle_output_timeout = request.idle_timeout,
      .max_output_bytes = request.max_response_bytes + 128u,
      .stdin_text = std::move(config), .stop = request.stop});
  if (!output) return tl::unexpected(output.error());
  if (output->cancelled)
    return tl::unexpected(make_error(ErrorCode::cancelled, "HTTP request cancelled"));
  if (output->timed_out)
    return tl::unexpected(make_error(ErrorCode::timeout,
        "HTTP request timed out (" + output->timeout_reason + ")", true));
  if (output->exit_code != 0)
    return tl::unexpected(make_error(ErrorCode::io_error,
        "HTTP transport failed: " + output->stderr_text, true));

  const auto marker_at = output->stdout_text.rfind(marker);
  if (marker_at == std::string::npos)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "HTTP response is missing status marker"));
  const auto status_begin = marker_at + marker.size();
  const auto status_end = output->stdout_text.find(retry_marker, status_begin);
  int status = 0;
  const auto parsed = std::from_chars(output->stdout_text.data() + status_begin,
                                      output->stdout_text.data() +
                                          (status_end == std::string::npos ?
                                               output->stdout_text.size() : status_end),
                                      status);
  if (parsed.ec != std::errc{} || status < 100 || status > 599)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "HTTP response status is invalid"));
  std::string retry_after;
  if (status_end != std::string::npos) {
    const auto retry_begin = status_end + retry_marker.size();
    const auto retry_end = output->stdout_text.find('\n', retry_begin);
    retry_after = output->stdout_text.substr(retry_begin,
        retry_end == std::string::npos ? std::string::npos : retry_end - retry_begin);
  }
  output->stdout_text.resize(marker_at);
  return HttpResponse{.status = status, .body = std::move(output->stdout_text),
      .stderr_text = std::move(output->stderr_text),
      .truncated = output->stdout_truncated, .retry_after = std::move(retry_after)};
}

}  // namespace tokmon::builtin
