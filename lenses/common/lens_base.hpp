#pragma once

#include <atomic>
#include <initializer_list>
#include <string_view>

#include "tokmon/lens.hpp"

namespace tokmon::builtin {

class LensBase : public ILens {
 public:
  explicit LensBase(LensManifest manifest);
  [[nodiscard]] const LensManifest& manifest() const noexcept final;
  void request_stop() noexcept override;

 protected:
  [[nodiscard]] Result<void> ready() const;
  [[nodiscard]] bool accepts(const Act& act) const noexcept;
  [[nodiscard]] Result<void> identify(SurfaceBuilder& surface,
                                      std::string channel,
                                      cbor::Value detail) const;
  [[nodiscard]] Result<RefractionResult> emit(RefractionBeam& beam,
      std::string kind, std::string schema, cbor::Value payload,
      std::string detail = "completed") const;
  [[nodiscard]] static Act propose(const Photon& source, std::string kind,
      std::string schema, std::string target, cbor::Value parameters,
      RiskClass risk = RiskClass::observe);
  [[nodiscard]] static std::string text(const Photon& photon,
                                        std::string_view field = "text");
  [[nodiscard]] static LensManifest make_manifest(std::string_view short_id,
      std::string display_name, std::vector<std::string> channels,
      std::vector<PhotonPattern> observes, std::vector<ActPattern> refracts,
      std::vector<std::string> permissions = {"photon.emit", "log.write"},
      RuntimeKind runtime = RuntimeKind::in_process);

 private:
  LensManifest manifest_;
  std::atomic_bool stopping_{false};
};

}  // namespace tokmon::builtin
