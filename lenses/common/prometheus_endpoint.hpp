#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "tokmon/error.hpp"

namespace tokmon::builtin {

// A deliberately small loopback-only HTTP/1.1 endpoint for Prometheus scrapes.
// It owns its listener and worker thread and never participates in Photon commit.
class PrometheusEndpoint final {
 public:
  PrometheusEndpoint();
  ~PrometheusEndpoint();
  PrometheusEndpoint(const PrometheusEndpoint&) = delete;
  PrometheusEndpoint& operator=(const PrometheusEndpoint&) = delete;

  Result<std::uint16_t> start(std::uint16_t port,
                              std::function<std::string()> snapshot);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon::builtin
