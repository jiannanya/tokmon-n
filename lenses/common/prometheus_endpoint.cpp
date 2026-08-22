#include "lenses/common/prometheus_endpoint.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tokmon::builtin {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr auto invalid_socket = INVALID_SOCKET;
void close_socket(const Socket socket) { if (socket != invalid_socket) closesocket(socket); }
struct WinsockLifetime {
  WinsockLifetime() { WSADATA data{}; available = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
  ~WinsockLifetime() { if (available) WSACleanup(); }
  bool available{false};
};
WinsockLifetime& winsock() { static WinsockLifetime lifetime; return lifetime; }
#else
using Socket = int;
constexpr auto invalid_socket = -1;
void close_socket(const Socket socket) { if (socket != invalid_socket) ::close(socket); }
#endif

void send_all(const Socket client, const std::string& response) {
  std::size_t offset = 0;
  while (offset < response.size()) {
#if defined(_WIN32)
    const auto sent = ::send(client, response.data() + offset,
                             static_cast<int>(response.size() - offset), 0);
#else
    const auto sent = ::send(client, response.data() + offset, response.size() - offset,
                             MSG_NOSIGNAL);
#endif
    if (sent <= 0) return;
    offset += static_cast<std::size_t>(sent);
  }
}

}  // namespace

struct PrometheusEndpoint::Impl {
  std::atomic_bool active{false};
  std::atomic_uint16_t bound_port{0};
  Socket listener{invalid_socket};
  std::function<std::string()> snapshot;
  std::jthread worker;

  void serve() {
    while (active.load(std::memory_order_acquire)) {
      const auto client = ::accept(listener, nullptr, nullptr);
      if (client == invalid_socket) {
        if (!active.load(std::memory_order_acquire)) return;
        continue;
      }
      std::array<char, 8193> request{};
#if defined(_WIN32)
      const auto received = ::recv(client, request.data(), 8192, 0);
#else
      const auto received = ::recv(client, request.data(), 8192, 0);
#endif
      const bool metrics = received > 0 &&
          (std::string_view(request.data(), static_cast<std::size_t>(received))
               .starts_with("GET /metrics ") ||
           std::string_view(request.data(), static_cast<std::size_t>(received))
               .starts_with("GET /metrics?"));
      const auto body = metrics && snapshot ? snapshot() : std::string("not found\n");
      const auto status = metrics ? "200 OK" : "404 Not Found";
      send_all(client, "HTTP/1.1 " + std::string(status) + "\r\n"
          "Content-Type: " + (metrics ? std::string("text/plain; version=0.0.4; charset=utf-8")
                                       : std::string("text/plain; charset=utf-8")) + "\r\n"
          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body);
      close_socket(client);
    }
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
#if defined(_WIN32)
  if (!winsock().available) {
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::io_error, "Winsock initialization failed"));
  }
#endif
  impl_->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (impl_->listener == invalid_socket) {
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot create Prometheus listener"));
  }
  int reuse = 1;
  (void)::setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(impl_->listener, 8) != 0) {
    close_socket(impl_->listener); impl_->listener = invalid_socket;
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot bind Prometheus loopback endpoint"));
  }
  sockaddr_in actual{};
#if defined(_WIN32)
  int actual_size = sizeof(actual);
#else
  socklen_t actual_size = sizeof(actual);
#endif
  if (::getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
    close_socket(impl_->listener); impl_->listener = invalid_socket;
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot inspect Prometheus endpoint"));
  }
  impl_->bound_port.store(ntohs(actual.sin_port), std::memory_order_release);
  impl_->snapshot = std::move(snapshot);
  impl_->worker = std::jthread([this] { impl_->serve(); });
  return impl_->bound_port.load(std::memory_order_acquire);
}

void PrometheusEndpoint::stop() noexcept {
  if (!impl_ || !impl_->active.exchange(false, std::memory_order_acq_rel)) return;
  const auto wake = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (wake != invalid_socket) {
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(impl_->bound_port.load(std::memory_order_acquire));
    (void)::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    close_socket(wake);
  }
  if (impl_->worker.joinable()) impl_->worker.join();
  close_socket(impl_->listener); impl_->listener = invalid_socket;
  impl_->bound_port.store(0, std::memory_order_release);
  impl_->snapshot = {};
}

bool PrometheusEndpoint::running() const noexcept {
  return impl_ && impl_->active.load(std::memory_order_acquire);
}
std::uint16_t PrometheusEndpoint::port() const noexcept {
  return impl_ ? impl_->bound_port.load(std::memory_order_acquire) : 0;
}

}  // namespace tokmon::builtin
