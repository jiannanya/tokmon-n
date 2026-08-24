#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "tests/support/test_framework.hpp"

#include "tokmon/tokmon.hpp"
#include "tokmon/yaml.hpp"

namespace tokmon::tests {

inline std::vector<std::string> yaml_strings(const cbor::Value* sequence) {
  std::vector<std::string> values;
  if (!sequence || !sequence->as_array()) return values;
  for (const auto& item : *sequence->as_array())
    values.emplace_back(item.as_string());
  return values;
}

inline void verify_pattern_sequence(const cbor::Value* sequence,
                                    const std::vector<PhotonPattern>& patterns) {
  REQUIRE(sequence != nullptr);
  REQUIRE(sequence->as_array() != nullptr);
  REQUIRE(sequence->as_array()->size() == patterns.size());
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    const auto& item = (*sequence->as_array())[index];
    REQUIRE(cbor::find(item, "kind")->as_string() == patterns[index].kind);
    REQUIRE(cbor::find(item, "schema")->as_string() == patterns[index].schema);
  }
}

inline void verify_pattern_sequence(const cbor::Value* sequence,
                                    const std::vector<ActPattern>& patterns) {
  REQUIRE(sequence != nullptr);
  REQUIRE(sequence->as_array() != nullptr);
  REQUIRE(sequence->as_array()->size() == patterns.size());
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    const auto& item = (*sequence->as_array())[index];
    REQUIRE(cbor::find(item, "kind")->as_string() == patterns[index].kind);
    REQUIRE(cbor::find(item, "schema")->as_string() == patterns[index].schema);
  }
}

inline void verify_lens_contract(const std::string_view short_id,
                                 const bool expected_stateless = true) {
  INFO("Lens: " << short_id);
  auto lens = make_builtin_lens(short_id);
  REQUIRE(lens);
  const auto& manifest = lens->manifest();
  REQUIRE(manifest.id == "org.tokmon.lens." + std::string(short_id));
  REQUIRE_FALSE(manifest.display_name.empty());
  REQUIRE(manifest.version == "0.1.0");
  REQUIRE(manifest.abi_major == 1);
  REQUIRE(manifest.abi_minor == 0);
  REQUIRE(manifest.trust == TrustLevel::t1);
  REQUIRE(manifest.stateless == expected_stateless);
  REQUIRE_FALSE(manifest.view_channels.empty());
  REQUIRE_FALSE(manifest.observes.empty());
  REQUIRE_FALSE(manifest.refracts.empty());

  const auto manifest_path = std::filesystem::path(TOKMON_SOURCE_DIR) /
      "lenses" / short_id / "lens.yaml";
  auto encoded = yaml::load(manifest_path);
  REQUIRE(encoded.has_value());
  REQUIRE(cbor::find(*encoded, "id")->as_string() == manifest.id);
  REQUIRE(cbor::find(*encoded, "display_name")->as_string() == manifest.display_name);
  REQUIRE(cbor::find(*encoded, "version")->as_string() == manifest.version);
  const auto* abi = cbor::find(*encoded, "abi");
  const auto* runtime = cbor::find(*encoded, "runtime");
  REQUIRE(cbor::find(*abi, "major")->as_integer() == manifest.abi_major);
  REQUIRE(cbor::find(*abi, "minor")->as_integer() == manifest.abi_minor);
  REQUIRE(cbor::find(*runtime, "kind")->as_string() == to_string(manifest.runtime));
  REQUIRE(cbor::find(*encoded, "trust")->as_string() == "t1");
  REQUIRE(cbor::find(*encoded, "stateless")->as_bool() == manifest.stateless);
  REQUIRE(yaml_strings(cbor::find(*encoded, "view_channels")) == manifest.view_channels);
  REQUIRE(yaml_strings(cbor::find(*encoded, "light_permissions")) ==
          manifest.light_permissions);
  verify_pattern_sequence(cbor::find(*encoded, "observes"), manifest.observes);
  verify_pattern_sequence(cbor::find(*encoded, "refracts"), manifest.refracts);

  SurfaceBuilder first_builder(manifest.id);
  SurfaceBuilder second_builder(manifest.id);
  REQUIRE(lens->view(PhotonWindow{}, first_builder));
  REQUIRE(lens->view(PhotonWindow{}, second_builder));
  const SurfaceSnapshot first{.epoch = 1,
      .contributions = first_builder.contributions(),
      .proposals = first_builder.proposals()};
  const SurfaceSnapshot second{.epoch = 1,
      .contributions = second_builder.contributions(),
      .proposals = second_builder.proposals()};
  REQUIRE(cbor::encode(to_cbor(first)) == cbor::encode(to_cbor(second)));
  for (const auto& contribution : first.contributions) {
    REQUIRE(contribution.lens == manifest.id);
    REQUIRE(std::find(manifest.view_channels.begin(), manifest.view_channels.end(),
                      contribution.channel) != manifest.view_channels.end());
  }

  struct RejectingHost final : OpticalHost {
    Result<Photon> emit(PhotonDraft) override {
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "unexpected emit for unmatched Act"));
    }
    void log(std::string_view, std::string_view, const LensId&) override {}
  } host;
  Act unknown{.id = "act-contract", .ray = "ray-contract", .kind = "unknown.act",
      .schema = "tokmon.unknown.v1", .target = manifest.id, .epoch = 1,
      .idempotency_key = "contract"};
  std::stop_source stop;
  RefractionBeam beam(host, unknown, stop.get_token(),
                      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  auto unmatched = lens->refract(PhotonWindow{}, unknown, beam);
  REQUIRE(unmatched);
  REQUIRE(unmatched->status == RefractionStatus::passed);

  lens->request_stop();
  SurfaceBuilder stopped_builder(manifest.id);
  auto stopped = lens->view(PhotonWindow{}, stopped_builder);
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == ErrorCode::cancelled);
}

}  // namespace tokmon::tests
