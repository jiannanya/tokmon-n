#include "tokmon/snow_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"

namespace tokmon {
namespace {

constexpr std::size_t header_size = 32;

void put16(std::uint8_t* output, const std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8u);
  output[1] = static_cast<std::uint8_t>(value);
}
void put32(std::uint8_t* output, const std::uint32_t value) {
  for (int index = 0; index < 4; ++index)
    output[index] = static_cast<std::uint8_t>(value >> ((3 - index) * 8));
}
void put64(std::uint8_t* output, const std::uint64_t value) {
  for (int index = 0; index < 8; ++index)
    output[index] = static_cast<std::uint8_t>(value >> ((7 - index) * 8));
}
std::uint16_t get16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8u) |
                                    static_cast<std::uint16_t>(input[1]));
}
std::uint32_t get32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) value = (value << 8u) | input[index];
  return value;
}
std::uint64_t get64(const std::uint8_t* input) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) value = (value << 8u) | input[index];
  return value;
}

std::array<std::uint8_t, header_size> encode_header(const SnowFrameHeader& header) {
  std::array<std::uint8_t, header_size> bytes{};
  put32(bytes.data(), header.magic);
  put16(bytes.data() + 4, header.protocol_major);
  put16(bytes.data() + 6, header.protocol_minor);
  put32(bytes.data() + 8, header.flags);
  put32(bytes.data() + 12, header.payload_size);
  put64(bytes.data() + 16, header.request_id);
  put64(bytes.data() + 24, header.cursor);
  return bytes;
}

Result<SnowFrameHeader> decode_header(const std::array<std::uint8_t, header_size>& bytes) {
  SnowFrameHeader header;
  header.magic = get32(bytes.data());
  header.protocol_major = get16(bytes.data() + 4);
  header.protocol_minor = get16(bytes.data() + 6);
  header.flags = get32(bytes.data() + 8);
  header.payload_size = get32(bytes.data() + 12);
  header.request_id = get64(bytes.data() + 16);
  header.cursor = get64(bytes.data() + 24);
  if (header.magic != snow_frame_magic)
    return tl::unexpected(make_error(ErrorCode::protocol_error, "invalid Snow frame magic"));
  if (header.protocol_major != snow_protocol_major)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "incompatible Snow protocol major"));
  if (header.payload_size > snow_max_payload_bytes)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Snow payload exceeds configured limit"));
  return header;
}

#if defined(_WIN32)
using Channel = HANDLE;
const Channel invalid_channel = INVALID_HANDLE_VALUE;
bool read_all(const Channel channel, void* output, std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(output);
  while (size != 0) {
    DWORD read = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20u));
    if (!ReadFile(channel, cursor, chunk, &read, nullptr) || read == 0) return false;
    cursor += read; size -= read;
  }
  return true;
}
bool write_all(const Channel channel, const void* input, std::size_t size) {
  const auto* cursor = static_cast<const std::uint8_t*>(input);
  while (size != 0) {
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20u));
    if (!WriteFile(channel, cursor, chunk, &written, nullptr) || written == 0) return false;
    cursor += written; size -= written;
  }
  return true;
}
void close_channel(const Channel channel) {
  if (channel != invalid_channel) CloseHandle(channel);
}
void interrupt_channel(const Channel channel) {
  if (channel != invalid_channel) {
    CancelIoEx(channel, nullptr);
    DisconnectNamedPipe(channel);
  }
}
#else
using Channel = int;
constexpr Channel invalid_channel = -1;
bool read_all(const Channel channel, void* output, std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(output);
  while (size != 0) {
    const auto count = ::read(channel, cursor, size);
    if (count <= 0) return false;
    cursor += count; size -= static_cast<std::size_t>(count);
  }
  return true;
}
bool write_all(const Channel channel, const void* input, std::size_t size) {
  const auto* cursor = static_cast<const std::uint8_t*>(input);
  while (size != 0) {
    const auto count = ::write(channel, cursor, size);
    if (count <= 0) return false;
    cursor += count; size -= static_cast<std::size_t>(count);
  }
  return true;
}
void close_channel(const Channel channel) { if (channel != invalid_channel) ::close(channel); }
void interrupt_channel(const Channel channel) {
  if (channel != invalid_channel) ::shutdown(channel, SHUT_RDWR);
}
#endif

Result<SnowMessage> read_message(const Channel channel) {
  std::array<std::uint8_t, header_size> header_bytes{};
  if (!read_all(channel, header_bytes.data(), header_bytes.size()))
    return tl::unexpected(make_error(ErrorCode::io_error, "Snow connection closed"));
  auto header = decode_header(header_bytes);
  if (!header) return tl::unexpected(header.error());
  std::vector<std::uint8_t> payload(header->payload_size);
  if (!payload.empty() && !read_all(channel, payload.data(), payload.size()))
    return tl::unexpected(make_error(ErrorCode::io_error, "incomplete Snow payload"));
  auto decoded = cbor::decode(payload, 64);
  if (!decoded) return tl::unexpected(decoded.error());
  if (cbor::encode(*decoded) != payload)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Snow payload is not canonical CBOR"));
  auto message = snow_message_from_cbor(*decoded);
  if (!message) return tl::unexpected(message.error());
  if (message->request_id != header->request_id || message->cursor != header->cursor)
    return tl::unexpected(make_error(ErrorCode::protocol_error,
                                     "Snow header and payload cursor mismatch"));
  return message;
}

Result<void> write_message(const Channel channel, const SnowMessage& message) {
  const auto payload = cbor::encode(to_cbor(message));
  if (payload.size() > snow_max_payload_bytes)
    return tl::unexpected(make_error(ErrorCode::protocol_error, "Snow payload too large"));
  const SnowFrameHeader header{.payload_size = static_cast<std::uint32_t>(payload.size()),
      .request_id = message.request_id, .cursor = message.cursor};
  const auto header_bytes = encode_header(header);
  if (!write_all(channel, header_bytes.data(), header_bytes.size()) ||
      (!payload.empty() && !write_all(channel, payload.data(), payload.size())))
    return tl::unexpected(make_error(ErrorCode::io_error, "failed to write Snow frame"));
  return {};
}

SnowMessage error_message(const SnowMessage& request, const Error& error) {
  return SnowMessage{.kind = SnowMessageKind::error, .request_id = request.request_id,
      .cursor = request.cursor,
      .payload = cbor::object({{"code", std::string(to_string(error.code))},
                               {"message", error.describe()}})};
}

}  // namespace

std::filesystem::path default_snow_endpoint(const std::filesystem::path& run_directory) {
#if defined(_WIN32)
  const auto key = sha256_hex(run_directory.lexically_normal().generic_string()).substr(0, 20);
  const std::wstring wide_key(key.begin(), key.end());
  return std::filesystem::path(L"\\\\.\\pipe\\tokmon-snow-" + wide_key);
#else
  return run_directory / "snow.sock";
#endif
}

SnowClient::SnowClient(std::filesystem::path endpoint,
                       const std::chrono::milliseconds connect_timeout)
    : endpoint_(std::move(endpoint)), connect_timeout_(connect_timeout) {}

Result<SnowMessage> SnowClient::request(const SnowMessage& message) const {
  return request_stream(message, {});
}

Result<SnowMessage> SnowClient::request_stream(const SnowMessage& message,
    const std::function<Result<void>(const SnowMessage&)>& on_stream) const {
#if defined(_WIN32)
  const auto endpoint = endpoint_.wstring();
  const auto connect_deadline = std::chrono::steady_clock::now() + connect_timeout_;
  Channel channel = invalid_channel;
  while (std::chrono::steady_clock::now() < connect_deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        connect_deadline - std::chrono::steady_clock::now());
    const auto wait_ms = static_cast<DWORD>(std::clamp<std::int64_t>(remaining.count(), 1, 100));
    if (WaitNamedPipeW(endpoint.c_str(), wait_ms)) {
      channel = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (channel != invalid_channel) break;
    }
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY &&
        error != ERROR_SEM_TIMEOUT)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (channel == invalid_channel)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Tokmon daemon Snow endpoint is unavailable"));
#else
  const auto channel = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (channel == invalid_channel)
    return tl::unexpected(make_error(ErrorCode::io_error, "cannot create Snow socket"));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto endpoint = endpoint_.string();
  if (endpoint.size() >= sizeof(address.sun_path)) {
    close_channel(channel);
    return tl::unexpected(make_error(ErrorCode::invalid_argument, "Snow endpoint is too long"));
  }
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1u);
  if (::connect(channel, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_channel(channel);
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "Tokmon daemon Snow endpoint is unavailable"));
  }
#endif
  if (auto written = write_message(channel, message); !written) {
    close_channel(channel); return tl::unexpected(written.error());
  }
  while (true) {
    auto response = read_message(channel);
    if (!response) { close_channel(channel); return tl::unexpected(response.error()); }
    if (response->kind != SnowMessageKind::stream) {
      close_channel(channel);
      return response;
    }
    if (on_stream) {
      auto observed = on_stream(*response);
      if (!observed) { close_channel(channel); return tl::unexpected(observed.error()); }
    }
  }
}

struct SnowServer::Impl {
  std::filesystem::path endpoint;
  Handler handler;
  std::jthread worker;
  std::mutex clients_mutex;
  std::vector<std::jthread> clients;
  std::set<Channel> live_channels;
  std::atomic_bool active{false};

  void launch_client(const Channel channel) {
    std::scoped_lock lock(clients_mutex);
    live_channels.insert(channel);
    clients.emplace_back([this, channel] {
      serve_channel(channel);
#if defined(_WIN32)
      FlushFileBuffers(channel);
      DisconnectNamedPipe(channel);
#endif
      {
        std::scoped_lock client_lock(clients_mutex);
        live_channels.erase(channel);
      }
      close_channel(channel);
    });
  }

  void serve_channel(const Channel channel) {
    while (active.load(std::memory_order_acquire)) {
      auto request = read_message(channel);
      if (!request) break;
      SnowMessage response;
      try { response = handler(*request); }
      catch (const std::exception& exception) {
        response = error_message(*request,
            make_error(ErrorCode::internal_error, exception.what()));
      } catch (...) {
        response = error_message(*request,
            make_error(ErrorCode::internal_error, "unknown Snow handler failure"));
      }
      if (response.kind == SnowMessageKind::intent_result) {
        if (const auto* photons = cbor::find(response.payload, "photons");
            photons && photons->as_array()) {
          std::uint64_t event_index = 0;
          for (const auto& photon : *photons->as_array()) {
            SnowMessage event{.kind = SnowMessageKind::stream,
                .request_id = request->request_id, .cursor = response.cursor,
                .payload = cbor::object({{"event_index", static_cast<std::int64_t>(event_index++)},
                    {"event", "photon"}, {"photon", photon}})};
            if (auto written = write_message(channel, event); !written) return;
          }
        }
      }
      if (auto written = write_message(channel, response); !written) break;
    }
  }

  void run() {
#if defined(_WIN32)
    while (active.load(std::memory_order_acquire)) {
      const auto channel = CreateNamedPipeW(endpoint.c_str(),
          PIPE_ACCESS_DUPLEX,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          PIPE_UNLIMITED_INSTANCES, static_cast<DWORD>(snow_max_payload_bytes),
          static_cast<DWORD>(snow_max_payload_bytes), 0, nullptr);
      if (channel == invalid_channel) {
        log_error("Snow CreateNamedPipe failed: {}", GetLastError()); break;
      }
      const auto connected = ConnectNamedPipe(channel, nullptr) != FALSE ||
                             GetLastError() == ERROR_PIPE_CONNECTED;
      if (connected && active.load(std::memory_order_acquire)) launch_client(channel);
      else close_channel(channel);
    }
#else
    std::error_code filesystem_error;
    std::filesystem::create_directories(endpoint.parent_path(), filesystem_error);
    ::unlink(endpoint.c_str());
    const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == invalid_channel) return;
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto text = endpoint.string();
    if (text.size() >= sizeof(address.sun_path)) { close_channel(listener); return; }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1u);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 16) != 0) { close_channel(listener); return; }
    ::chmod(endpoint.c_str(), 0600);
    while (active.load(std::memory_order_acquire)) {
      const auto channel = ::accept(listener, nullptr, nullptr);
      if (channel == invalid_channel) continue;
      if (active.load(std::memory_order_acquire)) launch_client(channel);
      else close_channel(channel);
    }
    close_channel(listener); ::unlink(endpoint.c_str());
#endif
  }
};

SnowServer::SnowServer() : impl_(std::make_unique<Impl>()) {}
SnowServer::~SnowServer() { stop(); }

Result<void> SnowServer::start(std::filesystem::path endpoint, Handler handler) {
  if (impl_->active.exchange(true, std::memory_order_acq_rel))
    return tl::unexpected(make_error(ErrorCode::invalid_state, "Snow server is already active"));
  if (!handler) {
    impl_->active.store(false, std::memory_order_release);
    return tl::unexpected(make_error(ErrorCode::invalid_argument, "Snow handler is required"));
  }
  impl_->endpoint = std::move(endpoint); impl_->handler = std::move(handler);
  impl_->worker = std::jthread([this] { impl_->run(); });
  return {};
}

void SnowServer::stop() noexcept {
  if (!impl_ || !impl_->active.exchange(false, std::memory_order_acq_rel)) return;
#if defined(_WIN32)
  const auto wake = CreateFileW(impl_->endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (wake != invalid_channel) close_channel(wake);
#else
  const auto wake = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (wake != invalid_channel) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto text = impl_->endpoint.string();
    if (text.size() < sizeof(address.sun_path)) {
      std::memcpy(address.sun_path, text.c_str(), text.size() + 1u);
      (void)::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    close_channel(wake);
  }
#endif
  if (impl_->worker.joinable()) impl_->worker.join();
  {
    std::scoped_lock lock(impl_->clients_mutex);
    for (const auto channel : impl_->live_channels) interrupt_channel(channel);
  }
  for (auto& client : impl_->clients)
    if (client.joinable()) client.join();
  impl_->clients.clear();
}

bool SnowServer::running() const noexcept {
  return impl_ && impl_->active.load(std::memory_order_acquire);
}

}  // namespace tokmon
