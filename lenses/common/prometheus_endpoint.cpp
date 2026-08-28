#include "lenses/common/prometheus_endpoint.hpp"

#include <atomic>
#include <utility>

#include <chhttp/chhttp.hpp>

namespace tokmon::builtin {

struct PrometheusEndpoint::Impl {
  std::atomic_bool active{false};
  std::atomic_uint16_t bound_port{0};
  std::function<std::string()> snapshot;
  chhttp::Server server;

  Impl() {
    server.get("/metrics", [this](const chhttp::Request&,
                                  chhttp::Response& response) {
      response.headers.set("Cache-Control", "no-store");
      response.set_content(snapshot ? snapshot() : std::string{},
                           "text/plain; version=0.0.4; charset=utf-8");
    });
  }
};

PrometheusEndpoint::PrometheusEndpoint() : impl_(std::make_unique<Impl>()) {}
PrometheusEndpoint::~PrometheusEndpoint() { stop(); }

Result<std::uint16_t> PrometheusEndpoint::start(
    const std::uint16_t port, std::function<std::string()> snapshot) {
  if (!snapshot)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "Prometheus snapshot callback is required"));
  if (impl_->active.exchange(true, std::memory_order_acq_rel))
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "Prometheus endpoint is already running"));
  impl_->snapshot = std::move(snapshot);
  if (!impl_->server.start("127.0.0.1", port)) {
    impl_->snapshot = {};
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot bind Prometheus loopback endpoint"));
  }
  impl_->bound_port.store(impl_->server.port(), std::memory_order_release);
  return impl_->bound_port.load(std::memory_order_acquire);
}

void PrometheusEndpoint::stop() noexcept {
  if (!impl_ || !impl_->active.exchange(false, std::memory_order_acq_rel)) return;
  impl_->server.stop();
  impl_->bound_port.store(0, std::memory_order_release);
  impl_->snapshot = {};
}

bool PrometheusEndpoint::running() const noexcept {
  return impl_ && impl_->active.load(std::memory_order_acquire) &&
      impl_->server.running();
}

std::uint16_t PrometheusEndpoint::port() const noexcept {
  return impl_ ? impl_->bound_port.load(std::memory_order_acquire) : 0;
}

}  // namespace tokmon::builtin
