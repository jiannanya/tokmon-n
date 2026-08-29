#include "integration/daemon_client.hpp"

#include "tokmon/daemon_lifecycle.hpp"
#include "tokmon/snow_protocol.hpp"
#include "tokmon/snow_transport.hpp"

namespace tokmon::desk {

DaemonClient::DaemonClient(std::filesystem::path endpoint)
    : endpoint_(std::move(endpoint)) {}

void DaemonClient::set_endpoint(std::filesystem::path endpoint) {
  endpoint_ = std::move(endpoint);
}

bool DaemonClient::available(std::string& error) const {
  const auto result = tokmon::daemon_available(endpoint_);
  if (!result) {
    error = result.error().describe();
    return false;
  }
  return *result;
}

bool DaemonClient::snapshot(tokmon::cbor::Value& payload, std::string& error) const {
  tokmon::SnowClient client(endpoint_);
  tokmon::SnowMessage request;
  request.kind = tokmon::SnowMessageKind::snapshot_request;
  request.request_id = tokmon::next_snow_request_id();
  request.payload = tokmon::cbor::object({{"scope", "desktop"}});
  const auto result = client.request(request);
  if (!result) {
    error = result.error().describe();
    return false;
  }
  if (result->kind == tokmon::SnowMessageKind::error) {
    error = "daemon rejected desktop snapshot";
    return false;
  }
  payload = result->payload;
  return true;
}

bool DaemonClient::intent(std::string action, tokmon::cbor::Value payload,
                          tokmon::cbor::Value& response, std::string& error) const {
  tokmon::SnowClient client(endpoint_);
  tokmon::SnowMessage request;
  request.kind = tokmon::SnowMessageKind::intent;
  request.request_id = tokmon::next_snow_request_id();
  if (auto* values = payload.as_map()) {
    (*values)["action"] = std::move(action);
    request.payload = std::move(payload);
  } else {
    request.payload = tokmon::cbor::object(
        {{"action", std::move(action)}, {"payload", std::move(payload)}});
  }
  const auto result = client.request(request);
  if (!result) {
    error = result.error().describe();
    return false;
  }
  if (result->kind == tokmon::SnowMessageKind::error) {
    error = "daemon rejected desktop intent";
    return false;
  }
  response = result->payload;
  return true;
}

DaemonStreamResult DaemonClient::stream_intent(
    std::string action, tokmon::cbor::Value payload, const std::uint64_t cursor,
    const std::function<void(tokmon::Photon)>& on_photon) const {
  DaemonStreamResult output;
  tokmon::SnowClient client(endpoint_);
  tokmon::SnowMessage request;
  request.kind = tokmon::SnowMessageKind::intent;
  request.request_id = tokmon::next_snow_request_id();
  request.cursor = cursor;
  if (auto* values = payload.as_map()) {
    (*values)["action"] = std::move(action);
    request.payload = std::move(payload);
  } else {
    request.payload = tokmon::cbor::object(
        {{"action", std::move(action)}, {"payload", std::move(payload)}});
  }

  auto response = client.request_stream(
      request, [&on_photon](const tokmon::SnowMessage& event)
                   -> tokmon::Result<void> {
        const auto* encoded = tokmon::cbor::find(event.payload, "photon");
        if (!encoded)
          return {};
        auto photon = tokmon::photon_from_cbor(*encoded);
        if (!photon)
          return tl::unexpected(photon.error());
        on_photon(std::move(*photon));
        return {};
      });
  if (!response) {
    output.error = response.error().describe();
    return output;
  }
  output.kind = response->kind;
  output.cursor = response->cursor;
  output.payload = response->payload;
  if (response->kind == tokmon::SnowMessageKind::error) {
    const auto* message = tokmon::cbor::find(response->payload, "message");
    output.error = message && !message->as_string().empty()
                       ? std::string(message->as_string())
                       : std::string("tokmon-daemon rejected the request");
    return output;
  }
  output.success = true;
  return output;
}

} // namespace tokmon::desk
