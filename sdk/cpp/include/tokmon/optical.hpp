#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/cbor.hpp"
#include "tokmon/ids.hpp"

namespace tokmon {

class PhotonWindow;
class SurfaceBuilder;
struct SurfaceSnapshot;

using BeatId = std::string;

enum class OpticalQueryCardinality : std::uint8_t {
  single,
  optional_single,
  many,
};

enum class OpticalQueryMerge : std::uint8_t {
  first,
  all,
  priority_then_path,
};

enum class OpticalQueryCache : std::uint8_t { none, per_beat };

struct OpticalQueryCapability {
  std::string capability;
  std::string request_schema;
  std::string response_schema;
  bool deterministic{true};
  std::int32_t priority{0};
  std::chrono::milliseconds default_timeout{10};
  std::chrono::milliseconds max_timeout{100};
  std::size_t max_request_bytes{256u * 1024u};
  std::size_t max_response_bytes{1024u * 1024u};
  std::size_t max_concurrent_queries{4};
  std::size_t max_queries_per_beat{1024};
  OpticalQueryCache cache{OpticalQueryCache::per_beat};
  bool operator==(const OpticalQueryCapability&) const = default;
};

struct OpticalQueryConsumption {
  std::string capability;
  OpticalQueryCardinality cardinality{OpticalQueryCardinality::optional_single};
  bool required{false};
  OpticalQueryMerge merge{OpticalQueryMerge::first};
  bool operator==(const OpticalQueryConsumption&) const = default;
};

struct QueryBudget {
  std::chrono::steady_clock::time_point deadline;
  std::size_t max_request_bytes{0};
  std::size_t max_response_bytes{0};
  std::size_t call_index{0};

  [[nodiscard]] bool expired() const noexcept;
};

struct OpticalQueryRequest {
  std::string capability;
  cbor::Value parameters{cbor::Value::Map{}};
  std::string request_schema;
  std::string response_schema;
  std::chrono::milliseconds timeout{0};
  std::size_t max_response_bytes{0};
};

struct FrozenLensState {
  LensId lens;
  std::string artifact_hash;
  MountEpoch epoch{0};
  GenerationId generation{0};
  std::size_t path_index{0};
  std::shared_ptr<const cbor::Value> value;

  [[nodiscard]] const cbor::Value& data() const noexcept;
};

struct QueryTrace {
  BeatId beat;
  RayId ray;
  LensId consumer;
  GenerationId consumer_generation{0};
  LensId provider;
  GenerationId provider_generation{0};
  std::string capability;
  std::string request_schema;
  std::string response_schema;
  std::string request_hash;
  std::string response_hash;
  bool cache_hit{false};
  std::int64_t duration_us{0};
  std::string status;
};

[[nodiscard]] cbor::Value to_cbor(const QueryTrace& trace);
[[nodiscard]] std::string_view to_string(OpticalQueryCardinality value) noexcept;
[[nodiscard]] std::string_view to_string(OpticalQueryMerge value) noexcept;
[[nodiscard]] std::string_view to_string(OpticalQueryCache value) noexcept;

class OpticalContext;

// Optional side interface: ILens stays ABI-stable. A Lens opts into one or more
// methods and never receives a writable host object from query().
class IOpticalLensExtension {
 public:
  virtual ~IOpticalLensExtension() = default;
  [[nodiscard]] virtual bool supports_derive() const noexcept;
  [[nodiscard]] virtual bool supports_coordinate() const noexcept;
  [[nodiscard]] virtual bool supports_query() const noexcept;
  virtual Result<cbor::Value> derive(const PhotonWindow& photons);
  virtual Result<void> coordinate(const PhotonWindow& photons,
                                  const OpticalContext& optical,
                                  SurfaceBuilder& surface);
  virtual Result<cbor::Value> optical_query(
      const FrozenLensState& state, std::string_view capability,
      const cbor::Value& parameters, const QueryBudget& budget) const;
};

struct BeatMetadata {
  BeatId beat;
  RayId ray;
  MountEpoch epoch{0};
  std::string path_hash;
  std::string input_prefix_hash;
};

class BeatBoardSnapshot;

class BeatBoardBuilder {
 public:
  explicit BeatBoardBuilder(BeatMetadata metadata);
  ~BeatBoardBuilder();
  BeatBoardBuilder(BeatBoardBuilder&&) noexcept;
  BeatBoardBuilder& operator=(BeatBoardBuilder&&) noexcept;
  BeatBoardBuilder(const BeatBoardBuilder&) = delete;
  BeatBoardBuilder& operator=(const BeatBoardBuilder&) = delete;

  Result<void> publish(LensId lens, std::string artifact_hash,
                       GenerationId generation, std::size_t path_index,
                       std::vector<OpticalQueryCapability> capabilities,
                       std::shared_ptr<IOpticalLensExtension> extension,
                       cbor::Value state);
  [[nodiscard]] Result<std::shared_ptr<const BeatBoardSnapshot>> freeze(
      const SurfaceSnapshot& surface) &&;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class BeatBoardSnapshot {
 public:
  ~BeatBoardSnapshot();
  [[nodiscard]] const BeatMetadata& metadata() const noexcept;
  [[nodiscard]] std::vector<QueryTrace> traces() const;

 private:
  friend class BeatBoardBuilder;
  friend class OpticalContext;
  BeatBoardSnapshot();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class OpticalContext {
 public:
  using GetCallback = std::function<Result<cbor::Value>(
      std::string_view channel, std::string_view key)>;
  using GetAllCallback = std::function<Result<std::vector<cbor::Value>>(
      std::string_view channel)>;
  using QueryCallback = std::function<Result<cbor::Value>(OpticalQueryRequest)>;

  OpticalContext(std::shared_ptr<const BeatBoardSnapshot> board, LensId consumer,
                 GenerationId generation,
                 std::vector<OpticalQueryConsumption> consumptions);
  [[nodiscard]] static OpticalContext from_callbacks(
      GetCallback get, GetAllCallback get_all, QueryCallback query);

  [[nodiscard]] Result<cbor::Value> get(
      std::string_view channel, std::string_view key) const;
  [[nodiscard]] Result<std::vector<cbor::Value>> get_all(
      std::string_view channel) const;
  [[nodiscard]] Result<cbor::Value> query(OpticalQueryRequest request) const;
  [[nodiscard]] std::vector<QueryTrace> query_traces() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace tokmon
