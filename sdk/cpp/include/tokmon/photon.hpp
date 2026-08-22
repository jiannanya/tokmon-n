#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tokmon/cbor.hpp"
#include "tokmon/ids.hpp"

namespace tokmon {

struct PhotonDraft {
  RayId ray;
  std::optional<PhotonId> parent;
  std::string kind;
  std::string schema;
  cbor::Value payload{cbor::Value::Map{}};
  MountEpoch epoch{0};
  std::string caused_by_act;
};

struct Photon {
  std::uint64_t sequence{0};
  PhotonId id;
  RayId ray;
  std::optional<PhotonId> parent;
  std::string kind;
  std::string schema;
  cbor::Value payload;
  MountEpoch epoch{0};
  std::int64_t committed_at_ms{0};
  std::string previous_hash;
  std::string hash;
  std::string caused_by_act;
};

struct PhotonPattern {
  std::string kind;
  std::string schema;

  [[nodiscard]] bool matches(const Photon& photon) const noexcept;
};

class PhotonWindow {
 public:
  PhotonWindow() = default;
  explicit PhotonWindow(std::vector<Photon> photons);

  [[nodiscard]] const std::vector<Photon>& photons() const noexcept;
  [[nodiscard]] const Photon* latest() const noexcept;
  [[nodiscard]] const Photon* latest(std::string_view kind) const noexcept;
  [[nodiscard]] bool contains_after(std::string_view kind,
                                    std::uint64_t sequence) const noexcept;

 private:
  std::vector<Photon> photons_;
};

[[nodiscard]] cbor::Value to_cbor(const Photon& photon);
[[nodiscard]] Result<Photon> photon_from_cbor(const cbor::Value& value);
[[nodiscard]] cbor::Value to_cbor(const PhotonWindow& window);
[[nodiscard]] Result<PhotonWindow> photon_window_from_cbor(const cbor::Value& value);

}  // namespace tokmon

