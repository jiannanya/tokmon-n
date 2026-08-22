#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "tokmon/act.hpp"
#include "tokmon/error.hpp"
#include "tokmon/photon.hpp"
#include "tokmon/surface.hpp"

namespace tokmon {

enum class RuntimeKind : std::uint8_t { in_process, native_worker, node, cpython, wasm, desktop };
enum class TrustLevel : std::uint8_t { t0, t1, t2, t3 };

struct LensManifest {
  LensId id;
  std::string display_name;
  std::string version{"0.1.0"};
  std::uint32_t abi_major{1};
  std::uint32_t abi_minor{0};
  RuntimeKind runtime{RuntimeKind::in_process};
  std::string runtime_version;
  std::string runtime_entry;
  TrustLevel trust{TrustLevel::t1};
  std::vector<PhotonPattern> observes;
  std::vector<std::string> view_channels;
  std::vector<ActPattern> refracts;
  std::vector<std::string> light_permissions;
  bool stateless{true};
};

class OpticalHost {
 public:
  virtual ~OpticalHost() = default;
  virtual Result<Photon> emit(PhotonDraft draft) = 0;
  virtual void log(std::string_view level, std::string_view message,
                   const LensId& lens) = 0;
};

class RefractionBeam {
 public:
  RefractionBeam(OpticalHost& host, Act act, std::stop_token stop,
                 std::chrono::steady_clock::time_point deadline);

  [[nodiscard]] const Act& act() const noexcept;
  [[nodiscard]] std::stop_token stop_token() const noexcept;
  [[nodiscard]] bool stop_requested() const noexcept;
  [[nodiscard]] bool expired() const noexcept;
  Result<Photon> emit(std::string kind, std::string schema,
                      cbor::Value payload);
  void log(std::string_view level, std::string_view message) const;

 private:
  OpticalHost& host_;
  Act act_;
  std::stop_token stop_;
  std::chrono::steady_clock::time_point deadline_;
};

enum class RefractionStatus : std::uint8_t { passed, completed, rejected, failed };

struct RefractionResult {
  RefractionStatus status{RefractionStatus::passed};
  std::vector<PhotonId> emitted;
  std::string detail;
};

class ILens {
 public:
  virtual ~ILens() = default;
  [[nodiscard]] virtual const LensManifest& manifest() const noexcept = 0;
  virtual Result<void> view(const PhotonWindow& photons, SurfaceBuilder& surface) = 0;
  virtual Result<RefractionResult> refract(const PhotonWindow& photons,
                                           const Act& act,
                                           RefractionBeam& beam) = 0;
  virtual void request_stop() noexcept = 0;
};

[[nodiscard]] std::string_view to_string(RuntimeKind kind) noexcept;
[[nodiscard]] std::string_view to_string(RefractionStatus status) noexcept;

}  // namespace tokmon
