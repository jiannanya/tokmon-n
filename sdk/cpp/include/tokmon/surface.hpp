#pragma once

#include <string>
#include <vector>

#include "tokmon/act.hpp"
#include "tokmon/cbor.hpp"
#include "tokmon/ids.hpp"
#include "tokmon/optical.hpp"

namespace tokmon {

struct SurfaceContribution {
  LensId lens;
  std::string channel;
  std::string key;
  cbor::Value value;
  std::int32_t priority{0};
};

class SurfaceBuilder {
 public:
  explicit SurfaceBuilder(LensId source);

  Result<void> add(std::string channel, std::string key, cbor::Value value,
                   std::int32_t priority = 0);
  Result<void> propose(Act act);
  [[nodiscard]] const std::vector<SurfaceContribution>& contributions() const noexcept;
  [[nodiscard]] const std::vector<Act>& proposals() const noexcept;

 private:
  LensId source_;
  std::vector<SurfaceContribution> contributions_;
  std::vector<Act> proposals_;
};

struct SurfaceSnapshot {
  MountEpoch epoch{0};
  BeatId beat;
  std::string path_hash;
  std::string input_prefix_hash;
  std::vector<SurfaceContribution> contributions;
  std::vector<Act> proposals;
  std::vector<QueryTrace> query_traces;
};

[[nodiscard]] cbor::Value to_cbor(const SurfaceSnapshot& surface);
[[nodiscard]] Result<SurfaceSnapshot> surface_from_cbor(const cbor::Value& value);

}  // namespace tokmon

