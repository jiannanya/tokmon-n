#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "tokmon/cbor.hpp"
#include "tokmon/photon.hpp"
#include "tokmon/snow_protocol.hpp"

namespace tokmon::desk {

struct DaemonStreamResult {
  bool success{false};
  tokmon::SnowMessageKind kind{tokmon::SnowMessageKind::error};
  std::uint64_t cursor{0};
  tokmon::cbor::Value payload{tokmon::cbor::Value::Map{}};
  std::string error;
};

class DaemonClient final {
 public:
  explicit DaemonClient(std::filesystem::path endpoint = {});
  void set_endpoint(std::filesystem::path endpoint);
  [[nodiscard]] bool available(std::string& error) const;
  [[nodiscard]] bool snapshot(tokmon::cbor::Value& payload, std::string& error) const;
  [[nodiscard]] bool intent(std::string action, tokmon::cbor::Value payload,
                            tokmon::cbor::Value& response, std::string& error) const;
  [[nodiscard]] DaemonStreamResult stream_intent(
      std::string action, tokmon::cbor::Value payload, std::uint64_t cursor,
      const std::function<void(tokmon::Photon)>& on_photon) const;

 private:
  std::filesystem::path endpoint_;
};

} // namespace tokmon::desk
