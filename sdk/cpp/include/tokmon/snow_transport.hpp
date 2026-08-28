#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <chrono>

#include "tokmon/snow_protocol.hpp"

namespace tokmon {

inline constexpr std::uint32_t snow_frame_magic = 0x534e4f57u;
inline constexpr std::uint16_t snow_protocol_major = 1;
inline constexpr std::uint16_t snow_protocol_minor = 0;
inline constexpr std::size_t snow_max_payload_bytes = 16u * 1024u * 1024u;

struct SnowFrameHeader {
  std::uint32_t magic{snow_frame_magic};
  std::uint16_t protocol_major{snow_protocol_major};
  std::uint16_t protocol_minor{snow_protocol_minor};
  std::uint32_t flags{0};
  std::uint32_t payload_size{0};
  std::uint64_t request_id{0};
  std::uint64_t cursor{0};
};

[[nodiscard]] std::filesystem::path default_snow_endpoint(
    const std::filesystem::path& run_directory);

class SnowClient final {
 public:
  explicit SnowClient(std::filesystem::path endpoint,
      std::chrono::milliseconds connect_timeout = std::chrono::seconds(3));
  [[nodiscard]] Result<SnowMessage> request(const SnowMessage& message) const;
  [[nodiscard]] Result<SnowMessage> request_stream(const SnowMessage& message,
      const std::function<Result<void>(const SnowMessage&)>& on_stream) const;

 private:
  std::filesystem::path endpoint_;
  std::chrono::milliseconds connect_timeout_;
};

class SnowServer final {
 public:
  using StreamSender = std::function<Result<void>(const SnowMessage&)>;
  using Handler = std::function<SnowMessage(const SnowMessage&, const StreamSender&)>;
  using LegacyHandler = std::function<SnowMessage(const SnowMessage&)>;

  SnowServer();
  ~SnowServer();
  SnowServer(const SnowServer&) = delete;
  SnowServer& operator=(const SnowServer&) = delete;

  Result<void> start(std::filesystem::path endpoint, Handler handler);
  Result<void> start(std::filesystem::path endpoint, LegacyHandler handler);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tokmon
