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
#include "tokmon/wavefront.hpp"

namespace tokmon {

enum class RuntimeKind : std::uint8_t { in_process, native_worker, node, cpython, wasm, desktop };
enum class TrustLevel : std::uint8_t { t0, t1, t2, t3 };

struct LensDependency {
  LensId id;
  std::string version;
};

struct LensResourceLimits {
  std::size_t memory_mb{256};
  std::size_t output_bytes{1024u * 1024u};
  std::chrono::milliseconds deadline{30'000};
};

struct LensManifest {
  LensId id;
  std::string display_name;
  std::string version{"0.1.0"};
  std::uint32_t abi_major{2};
  std::uint32_t abi_minor{0};
  RuntimeKind runtime{RuntimeKind::in_process};
  std::string runtime_version;
  std::string runtime_entry;
  TrustLevel trust{TrustLevel::t1};
  std::vector<PhotonPattern> observes;
  std::vector<OpticalPortSpec> inputs;
  std::vector<OpticalPortSpec> outputs;
  TriggerPolicy trigger{TriggerPolicy::once_when_ready};
  bool monotone{false};
  std::vector<ActPattern> refracts;
  std::vector<std::string> light_permissions;
  bool stateless{true};
  std::vector<LensDependency> dependencies;
  std::vector<LensId> conflicts;
  std::vector<LensId> optical_before;
  std::vector<LensId> optical_after;
  LensResourceLimits resources;
  std::string replacement{"R1"};
  std::string schema_bundle;
  std::string sbom;
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
  Result<Photon> emit_to(const RayId& ray, std::string kind, std::string schema,
                         cbor::Value payload, PhotonId parent = {});
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
  virtual Result<void> view(const OpticalInput& input,
                            WavefrontBuilder& outgoing) = 0;
  virtual Result<RefractionResult> refract(const PhotonWindow& photons,
                                           const Act& act,
                                           RefractionBeam& beam) = 0;
  virtual void request_stop() noexcept = 0;
};

struct MountedLens {
  std::shared_ptr<ILens> lens;
  GenerationId generation{0};
  std::string artifact_hash;
};

[[nodiscard]] std::string_view to_string(RuntimeKind kind) noexcept;
[[nodiscard]] std::string_view to_string(RefractionStatus status) noexcept;

}  // namespace tokmon
