#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <yaml-cpp/yaml.h>

#include "tokmon/tokmon.hpp"

namespace tokmon::tests {

inline std::vector<std::string> yaml_strings(const YAML::Node& sequence) {
  std::vector<std::string> values;
  for (const auto& item : sequence) values.push_back(item.as<std::string>());
  return values;
}

inline void verify_pattern_sequence(const YAML::Node& sequence,
                                    const std::vector<PhotonPattern>& patterns) {
  REQUIRE(sequence.IsSequence());
  REQUIRE(sequence.size() == patterns.size());
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    REQUIRE(sequence[index]["kind"].as<std::string>() == patterns[index].kind);
    REQUIRE(sequence[index]["schema"].as<std::string>() == patterns[index].schema);
  }
}

inline void verify_pattern_sequence(const YAML::Node& sequence,
                                    const std::vector<ActPattern>& patterns) {
  REQUIRE(sequence.IsSequence());
  REQUIRE(sequence.size() == patterns.size());
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    REQUIRE(sequence[index]["kind"].as<std::string>() == patterns[index].kind);
    REQUIRE(sequence[index]["schema"].as<std::string>() == patterns[index].schema);
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
  const auto yaml = YAML::LoadFile(manifest_path.string());
  REQUIRE(yaml["id"].as<std::string>() == manifest.id);
  REQUIRE(yaml["display_name"].as<std::string>() == manifest.display_name);
  REQUIRE(yaml["version"].as<std::string>() == manifest.version);
  REQUIRE(yaml["abi"]["major"].as<std::uint32_t>() == manifest.abi_major);
  REQUIRE(yaml["abi"]["minor"].as<std::uint32_t>() == manifest.abi_minor);
  REQUIRE(yaml["runtime"]["kind"].as<std::string>() == to_string(manifest.runtime));
  REQUIRE(yaml["trust"].as<std::string>() == "t1");
  REQUIRE(yaml["stateless"].as<bool>() == manifest.stateless);
  REQUIRE(yaml_strings(yaml["view_channels"]) == manifest.view_channels);
  REQUIRE(yaml_strings(yaml["light_permissions"]) == manifest.light_permissions);
  verify_pattern_sequence(yaml["observes"], manifest.observes);
  verify_pattern_sequence(yaml["refracts"], manifest.refracts);

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
