#include <atomic>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "tests/support/test_framework.hpp"
#include "tests/support/optical_harness.hpp"
#include "lenses/common/process_runner.hpp"
#include <sqlite3.h>

#include "tokmon/tokmon.hpp"

namespace {

std::filesystem::path temporary_directory(const std::string_view name) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("tokmon-" + std::string(name) + "-" + tokmon::make_id("test"));
  std::filesystem::create_directories(directory);
  return directory;
}

struct UserProfileGuard {
  explicit UserProfileGuard(const std::filesystem::path& replacement) {
    if (const auto* value = std::getenv("USERPROFILE")) previous = value;
#if defined(_WIN32)
    _putenv_s("USERPROFILE", replacement.string().c_str());
#else
    setenv("HOME", replacement.string().c_str(), 1);
#endif
  }
  ~UserProfileGuard() {
#if defined(_WIN32)
    _putenv_s("USERPROFILE", previous.c_str());
#else
    setenv("HOME", previous.c_str(), 1);
#endif
  }
  std::string previous;
};

void write_runtime_profile(const std::filesystem::path& root,
                           const std::string_view profile) {
  const auto directory = root / "home" / ".tokmon";
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "config.yaml") << "profile: " << profile << '\n';
}

class ManifestLens final : public tokmon::ILens {
 public:
  explicit ManifestLens(tokmon::LensManifest manifest) : manifest_(std::move(manifest)) {}
  const tokmon::LensManifest& manifest() const noexcept override { return manifest_; }
  tokmon::Result<void> view(const tokmon::OpticalInput&,
                            tokmon::WavefrontBuilder&) override { return {}; }
  tokmon::Result<tokmon::RefractionResult> refract(
      const tokmon::PhotonWindow&, const tokmon::Act&,
      tokmon::RefractionBeam&) override {
    return tokmon::RefractionResult{.status = tokmon::RefractionStatus::passed};
  }
  void request_stop() noexcept override {}

 private:
  tokmon::LensManifest manifest_;
};

tokmon::OpticalPortSpec optical_port(
    std::string name, std::string band,
    const bool surface = false,
    const tokmon::MergeLaw merge = tokmon::MergeLaw::set_union) {
  return tokmon::OpticalPortSpec{.name = std::move(name), .band = std::move(band),
      .schema = "tokmon.test.field.v1", .merge = merge, .surface = surface};
}

class EmittingLens final : public tokmon::ILens {
 public:
  struct Emission {
    std::string port;
    std::string key;
    tokmon::cbor::Value value;
    std::int32_t priority{0};
  };

  EmittingLens(tokmon::LensId id, std::vector<tokmon::OpticalPortSpec> outputs,
               std::vector<Emission> emissions)
      : manifest_{.id = std::move(id), .display_name = "Test Emitter",
            .version = "1.0.0", .abi_major = 2,
            .runtime = tokmon::RuntimeKind::in_process,
            .trust = tokmon::TrustLevel::t1, .outputs = std::move(outputs),
            .monotone = true},
        emissions_(std::move(emissions)) {}

  const tokmon::LensManifest& manifest() const noexcept override { return manifest_; }
  tokmon::Result<void> view(const tokmon::OpticalInput&,
                            tokmon::WavefrontBuilder& outgoing) override {
    for (const auto& emission : emissions_) {
      auto result = outgoing.emit(emission.port, emission.key, emission.value,
                                  {}, emission.priority);
      if (!result) return tl::unexpected(result.error());
    }
    return {};
  }
  tokmon::Result<tokmon::RefractionResult> refract(
      const tokmon::PhotonWindow&, const tokmon::Act&,
      tokmon::RefractionBeam&) override {
    return tokmon::RefractionResult{.status = tokmon::RefractionStatus::passed};
  }
  void request_stop() noexcept override {}

 private:
  tokmon::LensManifest manifest_;
  std::vector<Emission> emissions_;
};

class CapturingOpticalHost final : public tokmon::OpticalHost {
 public:
  tokmon::Result<tokmon::Photon> emit(tokmon::PhotonDraft draft) override {
    tokmon::Photon photon{.sequence = ++sequence_,
        .id = tokmon::make_id("photon"), .ray = draft.ray,
        .parent = std::move(draft.parent), .kind = std::move(draft.kind),
        .schema = std::move(draft.schema), .payload = std::move(draft.payload),
        .epoch = draft.epoch, .caused_by_act = std::move(draft.caused_by_act)};
    emitted.push_back(photon);
    return photon;
  }
  void log(std::string_view, std::string_view, const tokmon::LensId&) override {}

  std::vector<tokmon::Photon> emitted;

 private:
  std::uint64_t sequence_{0};
};

class ResonatorActLens final : public tokmon::ILens {
 public:
  ResonatorActLens()
      : manifest_{.id = "test.resonator-act", .display_name = "Invalid resonator",
            .abi_major = 2, .runtime = tokmon::RuntimeKind::in_process,
            .inputs = {optical_port("in", "test.resonator-act")},
            .outputs = {optical_port("out", "test.resonator-act")},
            .trigger = tokmon::TriggerPolicy::on_delta, .monotone = true,
            .refracts = {{"test.effect", "test.effect.v1"}}} {}
  const tokmon::LensManifest& manifest() const noexcept override { return manifest_; }
  tokmon::Result<void> view(const tokmon::OpticalInput& input,
                            tokmon::WavefrontBuilder& outgoing) override {
    return outgoing.propose(tokmon::Act{.id = "test-effect", .ray = input.beat().key.ray,
        .kind = "test.effect", .schema = "test.effect.v1",
        .target = manifest_.id, .epoch = input.beat().key.epoch});
  }
  tokmon::Result<tokmon::RefractionResult> refract(
      const tokmon::PhotonWindow&, const tokmon::Act&,
      tokmon::RefractionBeam&) override {
    return tokmon::RefractionResult{.status = tokmon::RefractionStatus::passed};
  }
  void request_stop() noexcept override {}

 private:
  tokmon::LensManifest manifest_;
};

class KeyJoinLens final : public tokmon::ILens {
 public:
  KeyJoinLens() {
    auto left = optical_port("left", "test.join");
    auto right = optical_port("right", "test.join");
    left.requirement = tokmon::PortRequirement::required;
    right.requirement = tokmon::PortRequirement::required;
    manifest_ = tokmon::LensManifest{.id = "test.key-join",
        .display_name = "Key Join", .abi_major = 2,
        .runtime = tokmon::RuntimeKind::in_process,
        .inputs = {left, right},
        .outputs = {optical_port("out", "test.joined", true)},
        .trigger = tokmon::TriggerPolicy::per_key_join, .monotone = true};
  }
  const tokmon::LensManifest& manifest() const noexcept override { return manifest_; }
  tokmon::Result<void> view(const tokmon::OpticalInput& input,
                            tokmon::WavefrontBuilder& outgoing) override {
    const auto* left = input.incident().one("left");
    const auto* right = input.incident().one("right");
    if (!left || !right || left->key != right->key)
      return tl::unexpected(tokmon::make_error(
          tokmon::ErrorCode::invalid_state, "per-key input was not joined"));
    const std::array<tokmon::FieldCellId, 2> causes{left->id, right->id};
    auto emitted = outgoing.emit("out", left->key,
        left->value.as_integer() + right->value.as_integer(), causes);
    if (!emitted) return tl::unexpected(emitted.error());
    return {};
  }
  tokmon::Result<tokmon::RefractionResult> refract(
      const tokmon::PhotonWindow&, const tokmon::Act&,
      tokmon::RefractionBeam&) override {
    return tokmon::RefractionResult{.status = tokmon::RefractionStatus::passed};
  }
  void request_stop() noexcept override {}

 private:
  tokmon::LensManifest manifest_;
};

}  // namespace

TEST_CASE("JSON bridge preserves protocol objects") {
  const auto value = tokmon::cbor::object({
      {"method", "tools/call"}, {"id", 7},
      {"params", tokmon::cbor::object({
          {"enabled", true},
          {"items", tokmon::cbor::Value::Array{"a", "b"}}})}});
  const auto text = tokmon::json::stringify(value);
  auto parsed = tokmon::json::parse(text);
  REQUIRE(parsed);
  REQUIRE(tokmon::cbor::encode(*parsed) == tokmon::cbor::encode(value));
  REQUIRE(tokmon::json::stringify(tokmon::cbor::Value::Bytes{1, 2, 255}) ==
          "{\"bytes\":[1,2,255],\"subtype\":null}");
  REQUIRE_FALSE(tokmon::json::parse("{broken"));
}

TEST_CASE("YAML bridge preserves scalar types aliases and quoted nulls") {
  auto parsed = tokmon::yaml::parse(
      "text: \"null\"\nempty: \"\"\nnull_value: null\nboolean: true\n"
      "integer: 42\nnumber: 1.25\nanchor: &shared value\nalias: *shared\n");
  REQUIRE(parsed);
  REQUIRE(tokmon::cbor::find(*parsed, "text")->as_string() == "null");
  REQUIRE(tokmon::cbor::find(*parsed, "empty")->as_string().empty());
  REQUIRE(tokmon::cbor::find(*parsed, "null_value")->is_null());
  REQUIRE(tokmon::cbor::find(*parsed, "boolean")->as_bool());
  REQUIRE(tokmon::cbor::find(*parsed, "integer")->as_integer() == 42);
  REQUIRE(std::get<double>(tokmon::cbor::find(*parsed, "number")->data) == 1.25);
  REQUIRE(tokmon::cbor::find(*parsed, "alias")->as_string() == "value");

  auto emitted = tokmon::yaml::stringify(*parsed);
  REQUIRE(emitted);
  auto reparsed = tokmon::yaml::parse(*emitted);
  REQUIRE(reparsed);
  REQUIRE(tokmon::cbor::find(*reparsed, "text")->as_string() == "null");
  REQUIRE(tokmon::cbor::find(*reparsed, "empty")->as_string().empty());
  REQUIRE(tokmon::cbor::find(*reparsed, "null_value")->is_null());
  REQUIRE(tokmon::cbor::find(*reparsed, "boolean")->as_bool());
  REQUIRE(tokmon::cbor::find(*reparsed, "integer")->as_integer() == 42);
  REQUIRE(std::get<double>(tokmon::cbor::find(*reparsed, "number")->data) == 1.25);
  REQUIRE(tokmon::cbor::find(*reparsed, "alias")->as_string() == "value");
  REQUIRE(tokmon::cbor::encode(*reparsed) == tokmon::cbor::encode(*parsed));
  REQUIRE_FALSE(tokmon::yaml::parse("value: 1\nvalue: 2\n"));
}

TEST_CASE("slash command catalog parses aliases quotes and desktop matches") {
  const auto& catalog = tokmon::slash_command_catalog();
  REQUIRE(catalog.size() >= 25);
  REQUIRE(std::ranges::none_of(catalog, [](const auto& command) {
    return command.name == "billing" || command.name == "login" ||
           command.name == "upgrade";
  }));

  auto parsed = tokmon::parse_slash_command("/subtask \"review src/core\"");
  REQUIRE(parsed);
  REQUIRE(parsed->descriptor->name == "fork");
  REQUIRE(parsed->invoked_name == "subtask");
  REQUIRE(parsed->arguments == std::vector<std::string>{"review src/core"});

  const auto matches = tokmon::match_slash_commands("/sec");
  REQUIRE_FALSE(matches.empty());
  REQUIRE(matches.front()->name == "security-review");
  REQUIRE_FALSE(tokmon::parse_slash_command("/billing"));
  REQUIRE_FALSE(tokmon::is_slash_command("explain /status"));
}

TEST_CASE("canonical CBOR is deterministic and rejects trailing bytes") {
  const auto value = tokmon::cbor::object({{"z", 1}, {"aa", 2}, {"a", 3}});
  const auto first = tokmon::cbor::encode(value);
  const auto second = tokmon::cbor::encode(value);
  REQUIRE(first == second);
  auto decoded = tokmon::cbor::decode(first);
  REQUIRE(decoded);
  REQUIRE(tokmon::cbor::diagnostic(*decoded) == "{\"a\":3,\"aa\":2,\"z\":1}");
  auto malformed = first; malformed.push_back(0);
  REQUIRE_FALSE(tokmon::cbor::decode(malformed));
}

TEST_CASE("Wavefront optical assembly composes merge aperture and projection") {
  auto raw_a = optical_port("out", "test.raw");
  auto raw_b = optical_port("out", "test.raw");
  auto left = optical_port("left", "test.raw");
  auto right = optical_port("right", "test.raw");
  left.requirement = tokmon::PortRequirement::required;
  right.requirement = tokmon::PortRequirement::required;
  auto merged = optical_port("merged", "test.merged");
  auto aperture_in = optical_port("in", "test.merged");
  aperture_in.requirement = tokmon::PortRequirement::required;
  auto selected = optical_port("selected", "test.selected");
  auto projection_in = optical_port("in", "test.selected");
  projection_in.requirement = tokmon::PortRequirement::required;
  auto surface = optical_port("surface", "test.surface", true,
                              tokmon::MergeLaw::stable_concat);

  auto source_a = std::make_shared<EmittingLens>("test.source-a", std::vector{raw_a},
      std::vector<EmittingLens::Emission>{{"out", "a-low", 1, 10},
                                          {"out", "a-high", 3, 30}});
  auto source_b = std::make_shared<EmittingLens>("test.source-b", std::vector{raw_b},
      std::vector<EmittingLens::Emission>{{"out", "b-mid", 2, 20}});
  auto merge = std::make_shared<tokmon::MergeLens>("test.merge",
      std::vector{left, right}, merged);
  auto aperture = std::make_shared<tokmon::ApertureLens>(
      "test.aperture", aperture_in, selected, 2);
  auto projection = std::make_shared<tokmon::ProjectionLens>(
      "test.projection", projection_in, surface);
  const std::vector<tokmon::MountedLens> lenses{
      {source_a, 1, "a"}, {source_b, 1, "b"}, {merge, 1, "merge"},
      {aperture, 1, "aperture"}, {projection, 1, "projection"}};
  tokmon::OpticalAssemblySpec spec;
  spec.id = "test.assembly.composition";
  spec.autowire_unique = false;
  spec.connections = {
      {{"test.source-a", "out"}, {"test.merge", "left"}},
      {{"test.source-b", "out"}, {"test.merge", "right"}},
      {{"test.merge", "merged"}, {"test.aperture", "in"}},
      {{"test.aperture", "selected"}, {"test.projection", "in"}}};
  auto assembly = tokmon::compile_optical_assembly(7, lenses, spec);
  REQUIRE(assembly);
  tokmon::OpticalPropagator propagator;
  auto first = propagator.propagate("ray-optical", tokmon::PhotonWindow{},
                                    lenses, **assembly);
  auto second = propagator.propagate("ray-optical", tokmon::PhotonWindow{},
                                     lenses, **assembly);
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first->surface.contributions.size() == 2);
  REQUIRE(first->surface.contributions[0].key == "a-high");
  REQUIRE(first->surface.contributions[1].key == "b-mid");
  REQUIRE(first->wavefront.band("diagnostic.optical").size() == 1);
  REQUIRE(tokmon::cbor::find(first->wavefront.band("diagnostic.optical").front().value,
                             "dropped_count")->as_integer() == 1);
  REQUIRE(first->surface.assembly_hash == (*assembly)->hash);
  REQUIRE(first->surface.wavefront_hash == second->surface.wavefront_hash);
  REQUIRE_FALSE(first->surface.contributions.front().field_cell.empty());
  REQUIRE_FALSE(first->surface.contributions.front().input_cells.empty());
}

TEST_CASE("Optical assembly rejects bare cycles and converges guarded resonators") {
  auto input_a = optical_port("in", "test.loop");
  auto output_a = optical_port("out", "test.loop");
  auto input_b = optical_port("in", "test.loop");
  auto output_b = optical_port("out", "test.loop");
  auto lens_a = std::make_shared<tokmon::IdentityLens>(
      "test.loop-a", input_a, output_a);
  auto lens_b = std::make_shared<tokmon::IdentityLens>(
      "test.loop-b", input_b, output_b);
  const std::vector<tokmon::MountedLens> lenses{
      {lens_a, 1, "a"}, {lens_b, 1, "b"}};
  tokmon::OpticalAssemblySpec spec;
  spec.id = "test.assembly.loop";
  spec.autowire_unique = false;
  spec.connections = {{{"test.loop-a", "out"}, {"test.loop-b", "in"}},
                      {{"test.loop-b", "out"}, {"test.loop-a", "in"}}};
  auto bare = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE_FALSE(bare);
  REQUIRE(bare.error().code == tokmon::ErrorCode::invalid_state);

  spec.resonators.push_back(tokmon::ResonatorSpec{
      .id = "test.resonator", .lenses = {"test.loop-a", "test.loop-b"},
      .budget = {.max_rounds = 4}});
  auto guarded = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE(guarded);
  auto result = tokmon::OpticalPropagator{}.propagate(
      "ray-loop", tokmon::PhotonWindow{}, lenses, **guarded);
  REQUIRE(result);
  REQUIRE(result->surface.propagation_rounds == 1);

  auto invalid = std::make_shared<ResonatorActLens>();
  const std::vector<tokmon::MountedLens> invalid_lenses{{invalid, 1, "invalid"}};
  tokmon::OpticalAssemblySpec invalid_spec;
  invalid_spec.id = "test.resonator-act.assembly";
  invalid_spec.autowire_unique = false;
  invalid_spec.connections = {{{"test.resonator-act", "out"},
                               {"test.resonator-act", "in"}}};
  invalid_spec.resonators = {{.id = "test.resonator-act.region",
      .lenses = {"test.resonator-act"}, .budget = {.max_rounds = 2}}};
  auto invalid_assembly = tokmon::compile_optical_assembly(
      1, invalid_lenses, invalid_spec);
  REQUIRE(invalid_assembly);
  auto forbidden = tokmon::OpticalPropagator{}.propagate(
      "ray-resonator-act", tokmon::PhotonWindow{}, invalid_lenses,
      **invalid_assembly);
  REQUIRE_FALSE(forbidden);
  REQUIRE(forbidden.error().code == tokmon::ErrorCode::permission_denied);
}

TEST_CASE("Optical per-key join invokes a Lens once for each ready key") {
  auto left = std::make_shared<EmittingLens>("test.join-left",
      std::vector{optical_port("out", "test.join")},
      std::vector<EmittingLens::Emission>{{"out", "x", 1, 0},
                                          {"out", "y", 2, 0}});
  auto right = std::make_shared<EmittingLens>("test.join-right",
      std::vector{optical_port("out", "test.join")},
      std::vector<EmittingLens::Emission>{{"out", "x", 1, 0},
                                          {"out", "y", 20, 0},
                                          {"out", "unmatched", 30, 0}});
  auto join = std::make_shared<KeyJoinLens>();
  const std::vector<tokmon::MountedLens> lenses{
      {left, 1, "left"}, {right, 1, "right"}, {join, 1, "join"}};
  tokmon::OpticalAssemblySpec spec;
  spec.id = "test.join.assembly";
  spec.autowire_unique = false;
  spec.connections = {{{"test.join-left", "out"}, {"test.key-join", "left"}},
                      {{"test.join-right", "out"}, {"test.key-join", "right"}}};
  auto assembly = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE(assembly);
  auto result = tokmon::OpticalPropagator{}.propagate(
      "ray-join", tokmon::PhotonWindow{}, lenses, **assembly);
  REQUIRE(result);
  REQUIRE(result->surface.contributions.size() == 2);
  REQUIRE(result->surface.contributions[0].key == "x");
  REQUIRE(result->surface.contributions[0].value.as_integer() == 2);
  REQUIRE(result->surface.contributions[1].key == "y");
  REQUIRE(result->surface.contributions[1].value.as_integer() == 22);
  REQUIRE(std::ranges::count_if(result->trace, [](const auto& entry) {
    return entry.lens == "test.key-join" && entry.status == "completed";
  }) == 2);
}

TEST_CASE("Optical assembly Lens preserves explicit nested boundaries and provenance") {
  auto inner_input = optical_port("in", "test.nested.in");
  inner_input.requirement = tokmon::PortRequirement::required;
  auto inner_output = optical_port("out", "test.nested.out");
  auto identity = std::make_shared<tokmon::IdentityLens>(
      "test.nested.identity", inner_input, inner_output);
  std::vector<tokmon::MountedLens> internal{{identity, 1, "identity"}};
  tokmon::OpticalAssemblySpec inner_spec;
  inner_spec.id = "test.nested.internal";
  inner_spec.autowire_unique = false;
  inner_spec.inputs = {{"in", {"test.nested.identity", "in"}}};
  inner_spec.outputs = {{{"test.nested.identity", "out"}, "out"}};
  tokmon::LensManifest boundary{.id = "test.nested.facade",
      .display_name = "Nested facade", .abi_major = 2,
      .inputs = {optical_port("in", "test.nested.in")},
      .outputs = {optical_port("out", "test.nested.out", true)}};
  auto facade = tokmon::OpticalAssemblyLens::create(boundary, internal, inner_spec);
  REQUIRE(facade);
  auto source = std::make_shared<EmittingLens>("test.nested.source",
      std::vector{optical_port("out", "test.nested.in")},
      std::vector<EmittingLens::Emission>{{"out", "nested", "value", 4}});
  const std::vector<tokmon::MountedLens> lenses{
      {source, 1, "source"}, {*facade, 1, "facade"}};
  tokmon::OpticalAssemblySpec outer;
  outer.id = "test.nested.outer";
  outer.autowire_unique = false;
  outer.connections = {{{"test.nested.source", "out"},
                        {"test.nested.facade", "in"}}};
  auto compiled = tokmon::compile_optical_assembly(3, lenses, outer);
  REQUIRE(compiled);
  auto result = tokmon::OpticalPropagator{}.propagate(
      "ray-nested", tokmon::PhotonWindow{}, lenses, **compiled);
  REQUIRE(result);
  REQUIRE(result->surface.contributions.size() == 1);
  REQUIRE(result->surface.contributions.front().lens == "test.nested.facade");
  REQUIRE(result->surface.contributions.front().key == "nested");
  REQUIRE(result->surface.contributions.front().input_cells.size() == 1);
}

TEST_CASE("Causal delay Lens crosses a committed Photon boundary between beats") {
  auto source = std::make_shared<EmittingLens>("test.delay.source",
      std::vector{optical_port("out", "test.delay.in")},
      std::vector<EmittingLens::Emission>{{"out", "delayed", 42, 9}});
  auto delay_input = optical_port("in", "test.delay.in");
  delay_input.requirement = tokmon::PortRequirement::required;
  auto delay = std::make_shared<tokmon::CausalDelayLens>(
      "test.delay", delay_input, optical_port("out", "test.delay.out", true));
  const std::vector<tokmon::MountedLens> lenses{
      {source, 1, "source"}, {delay, 1, "delay"}};
  tokmon::OpticalAssemblySpec spec;
  spec.id = "test.delay.assembly";
  spec.autowire_unique = false;
  spec.connections = {{{"test.delay.source", "out"}, {"test.delay", "in"}}};
  auto assembly = tokmon::compile_optical_assembly(5, lenses, spec);
  REQUIRE(assembly);
  tokmon::OpticalPropagator propagator;
  auto first = propagator.propagate("ray-delay", tokmon::PhotonWindow{},
                                    lenses, **assembly);
  REQUIRE(first);
  REQUIRE(first->surface.proposals.size() == 1);
  REQUIRE(first->surface.contributions.empty());

  CapturingOpticalHost host;
  std::stop_source stop;
  tokmon::RefractionBeam beam(host, first->surface.proposals.front(),
      stop.get_token(), std::chrono::steady_clock::now() + std::chrono::seconds(1));
  auto refracted = delay->refract(tokmon::PhotonWindow{},
                                  first->surface.proposals.front(), beam);
  REQUIRE(refracted);
  REQUIRE(refracted->status == tokmon::RefractionStatus::completed);
  REQUIRE(host.emitted.size() == 1);

  auto second = propagator.propagate("ray-delay", tokmon::PhotonWindow(host.emitted),
                                     lenses, **assembly);
  REQUIRE(second);
  REQUIRE(second->surface.proposals.empty());
  REQUIRE(second->surface.contributions.size() == 1);
  REQUIRE(second->surface.contributions.front().key == "delayed");
  REQUIRE(second->surface.contributions.front().value.as_integer() == 42);
}

TEST_CASE("Optical assembly enforces sensitivity audience and process boundaries") {
  auto sensitive = optical_port("out", "test.secure");
  sensitive.sensitivity = tokmon::FieldSensitivity::sensitive;
  auto source = std::make_shared<EmittingLens>("test.secure.source",
      std::vector{sensitive}, std::vector<EmittingLens::Emission>{});
  auto normal_input = optical_port("in", "test.secure");
  auto consumer_manifest = tokmon::LensManifest{.id = "test.secure.consumer",
      .display_name = "Consumer", .abi_major = 2,
      .inputs = std::vector{normal_input}};
  auto consumer = std::make_shared<ManifestLens>(consumer_manifest);
  std::vector<tokmon::MountedLens> lenses{{source, 1, "source"},
                                          {consumer, 1, "consumer"}};
  tokmon::OpticalAssemblySpec spec;
  spec.id = "test.secure.assembly";
  spec.autowire_unique = false;
  spec.connections = {{{"test.secure.source", "out"},
                       {"test.secure.consumer", "in"}}};
  auto lowered = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE_FALSE(lowered);
  REQUIRE(lowered.error().code == tokmon::ErrorCode::permission_denied);

  sensitive.sensitivity = tokmon::FieldSensitivity::normal;
  sensitive.allowed_audiences = {"test.someone-else"};
  source = std::make_shared<EmittingLens>("test.secure.source",
      std::vector{sensitive}, std::vector<EmittingLens::Emission>{});
  lenses = {{source, 1, "source"}, {consumer, 1, "consumer"}};
  auto audience = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE_FALSE(audience);
  REQUIRE(audience.error().code == tokmon::ErrorCode::permission_denied);

  sensitive.allowed_audiences.clear();
  sensitive.exportable = false;
  source = std::make_shared<EmittingLens>("test.secure.source",
      std::vector{sensitive}, std::vector<EmittingLens::Emission>{});
  consumer_manifest.inputs.front().sensitivity = tokmon::FieldSensitivity::normal;
  consumer_manifest.runtime = tokmon::RuntimeKind::native_worker;
  consumer = std::make_shared<ManifestLens>(consumer_manifest);
  lenses = {{source, 1, "source"}, {consumer, 1, "consumer"}};
  auto escaped = tokmon::compile_optical_assembly(1, lenses, spec);
  REQUIRE_FALSE(escaped);
  REQUIRE(escaped.error().code == tokmon::ErrorCode::permission_denied);
}

TEST_CASE("Photon store is hash chained and physically append-only") {
  const auto root = temporary_directory("append-only");
  const auto database = root / "photons.sqlite3";
  tokmon::PhotonStore store;
  REQUIRE(store.open(database));
  const auto ray = tokmon::make_id("ray");
  auto first = store.append(tokmon::PhotonDraft{.ray = ray, .kind = "test.first",
      .schema = "tokmon.test.v1", .payload = tokmon::cbor::object({{"value", 1}}),
      .epoch = 1});
  REQUIRE(first);
  auto second = store.append(tokmon::PhotonDraft{.ray = ray, .kind = "test.second",
      .schema = "tokmon.test.v1", .payload = tokmon::cbor::object({{"value", 2}}),
      .epoch = 1});
  REQUIRE(second);
  REQUIRE(second->sequence == first->sequence + 1);
  REQUIRE(second->previous_hash == first->hash);
  REQUIRE(store.verify());

  sqlite3* connection = nullptr;
  REQUIRE(sqlite3_open(database.string().c_str(), &connection) == SQLITE_OK);
  sqlite3_stmt* checkpoint = nullptr;
  REQUIRE(sqlite3_prepare_v2(connection,
      "SELECT sequence FROM photon_verification_state WHERE singleton=1",
      -1, &checkpoint, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(checkpoint) == SQLITE_ROW);
  REQUIRE(sqlite3_column_int64(checkpoint, 0) == 2);
  sqlite3_finalize(checkpoint);
  char* error = nullptr;
  REQUIRE(sqlite3_exec(connection, "UPDATE photons SET kind='tampered' WHERE sequence=1",
                       nullptr, nullptr, &error) != SQLITE_OK);
  sqlite3_free(error); error = nullptr;
  REQUIRE(sqlite3_exec(connection, "DELETE FROM photons WHERE sequence=1",
                       nullptr, nullptr, &error) != SQLITE_OK);
  sqlite3_free(error);
  sqlite3_close(connection);
  auto third = store.append(tokmon::PhotonDraft{.ray = ray, .kind = "test.third",
      .schema = "tokmon.test.v1", .payload = tokmon::cbor::object({{"value", 3}}),
      .epoch = 1});
  REQUIRE(third);
  REQUIRE(store.verify());
  REQUIRE(sqlite3_open(database.string().c_str(), &connection) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(connection,
      "SELECT sequence FROM photon_verification_state WHERE singleton=1",
      -1, &checkpoint, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(checkpoint) == SQLITE_ROW);
  REQUIRE(sqlite3_column_int64(checkpoint, 0) == 3);
  sqlite3_finalize(checkpoint);
  sqlite3_close(connection);
}

TEST_CASE("LightPath publication only exposes complete immutable epochs") {
  tokmon::LightPath path;
  auto one = std::make_shared<tokmon::LightPathSnapshot>();
  one->epoch = 1; one->hash = "epoch-one";
  one->lenses.push_back(tokmon::MountedLens{
      tokmon::make_builtin_lens("ignis"), 1000, "a"});
  auto two = std::make_shared<tokmon::LightPathSnapshot>();
  two->epoch = 2; two->hash = "epoch-two";
  two->lenses.push_back(tokmon::MountedLens{
      tokmon::make_builtin_lens("rhea"), 2000, "b"});
  REQUIRE(path.publish(one));
  std::atomic_bool invalid{false};
  std::jthread reader([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      const auto snapshot = path.snapshot();
      if (!((snapshot->hash == "epoch-one" &&
             snapshot->lenses.front().generation == 1000) ||
            (snapshot->hash == "epoch-two" &&
             snapshot->lenses.front().generation == 2000))) invalid.store(true);
    }
  });
  for (int index = 0; index < 1000; ++index) {
    auto candidate = std::make_shared<tokmon::LightPathSnapshot>();
    candidate->epoch = static_cast<tokmon::MountEpoch>(index + 2);
    const bool first_shape = (index % 2) == 0;
    candidate->hash = first_shape ? "epoch-one" : "epoch-two";
    candidate->lenses.push_back(tokmon::MountedLens{
        tokmon::make_builtin_lens(first_shape ? "ignis" : "rhea"),
        first_shape ? 1000u : 2000u, first_shape ? "a" : "b"});
    REQUIRE(path.publish(std::move(candidate)));
  }
  reader.request_stop(); reader.join();
  REQUIRE_FALSE(invalid.load());
}

TEST_CASE("closed Lens generations reject every later Beam acquisition") {
  tokmon::BeamRegistry beams;
  auto active = beams.acquire("org.example.lens", 42, "ray-active",
                              std::chrono::seconds(1));
  REQUIRE(active);
  REQUIRE(beams.stop_generation("org.example.lens", 42) == 1);
  REQUIRE((*active)->stop.stop_requested());
  auto stale = beams.acquire("org.example.lens", 42, "ray-stale",
                             std::chrono::seconds(1));
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code == tokmon::ErrorCode::invalid_state);
  beams.release((*active)->id);
}

TEST_CASE("C ABI Lens loads and passes a dark-lane view") {
  auto lens = tokmon::CAbiLens::load(TOKMON_TEST_LENS_PATH);
  REQUIRE(lens);
  REQUIRE((*lens)->manifest().id == "org.tokmon.lens.calculator");
  auto result = tokmon::tests::view_lens_once(*lens);
  REQUIRE(result);
  REQUIRE_FALSE(result->surface.contributions.empty());
}

TEST_CASE("Snow framing carries canonical requests and responses") {
  const auto root = temporary_directory("snow");
  const auto endpoint = tokmon::default_snow_endpoint(root);
  tokmon::SnowServer server;
  REQUIRE(server.start(endpoint, [](const tokmon::SnowMessage& request) {
    return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::pong,
        .request_id = request.request_id, .cursor = request.cursor,
        .payload = tokmon::cbor::object({{"healthy", true}})};
  }));
  tokmon::SnowClient client(endpoint);
  auto response = client.request(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::ping,
      .request_id = 42, .cursor = 7});
  REQUIRE(response);
  REQUIRE(response->kind == tokmon::SnowMessageKind::pong);
  REQUIRE(response->request_id == 42);
  REQUIRE(response->cursor == 7);
  REQUIRE(tokmon::cbor::find(response->payload, "healthy")->as_bool());
  server.stop();
}

TEST_CASE("Snow local transport serves independent clients concurrently") {
  const auto root = temporary_directory("snow-concurrent");
  const auto endpoint = tokmon::default_snow_endpoint(root);
  std::atomic_int active{0};
  std::atomic_int maximum{0};
  tokmon::SnowServer server;
  REQUIRE(server.start(endpoint, [&active, &maximum](const tokmon::SnowMessage& request) {
    const auto current = active.fetch_add(1) + 1;
    auto observed = maximum.load();
    while (current > observed && !maximum.compare_exchange_weak(observed, current)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    active.fetch_sub(1);
    return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::pong,
        .request_id = request.request_id};
  }));
  std::vector<std::jthread> clients;
  std::atomic_int succeeded{0};
  for (std::uint64_t index = 0; index < 8; ++index) {
    clients.emplace_back([&, index] {
      tokmon::SnowClient client(endpoint);
      auto result = client.request(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::ping,
          .request_id = index + 1});
      if (result && result->request_id == index + 1) succeeded.fetch_add(1);
    });
  }
  for (auto& client : clients) client.join();
  REQUIRE(succeeded.load() == 8);
  REQUIRE(maximum.load() > 1);
  server.stop();
}

TEST_CASE("Fallen policy keeps deny precedence and project policy cannot expand root authority") {
  tokmon::RuntimeConfig config;
  config.user_policy.configured = true;
  config.user_policy.default_effect = tokmon::PolicyEffect::ask;
  config.user_policy.approval_risks.clear();
  config.user_policy.rules.push_back(tokmon::PolicyRule{
      .effect = tokmon::PolicyEffect::deny,
      .acts = {"process.exec"}, .argv0 = {"powershell", "cmd"}});
  config.user_policy.rules.push_back(tokmon::PolicyRule{
      .effect = tokmon::PolicyEffect::allow,
      .acts = {"fs.read"}, .paths = {"${workspace}/**"}});
  config.project_policy.configured = true;
  config.project_policy.default_effect = tokmon::PolicyEffect::allow;
  config.project_policy.approval_risks.clear();
  config.project_policy.rules.push_back(tokmon::PolicyRule{
      .effect = tokmon::PolicyEffect::allow, .acts = {"process.exec"}});

  tokmon::Act process{.kind = "process.exec", .schema = "tokmon.process.exec.v1",
      .parameters = tokmon::cbor::object({
          {"argv", tokmon::cbor::Value::Array{"powershell", "-NoProfile"}}}),
      .target = "org.tokmon.lens.styx", .risk = tokmon::RiskClass::external};
  REQUIRE(tokmon::evaluate_policy(config, process, tokmon::TrustLevel::t1,
                                  "C:/workspace") == tokmon::PolicyEffect::deny);
  tokmon::Act read{.kind = "fs.read", .schema = "tokmon.fs.read.v1",
      .parameters = tokmon::cbor::object({{"path", "C:/workspace/src/main.cpp"}}),
      .target = "org.tokmon.lens.cove", .risk = tokmon::RiskClass::observe};
  REQUIRE(tokmon::evaluate_policy(config, read, tokmon::TrustLevel::t1,
                                  "C:/workspace") == tokmon::PolicyEffect::allow);
  read.parameters = tokmon::cbor::object({{"path", "C:/outside/secret.txt"}});
  REQUIRE(tokmon::evaluate_policy(config, read, tokmon::TrustLevel::t1,
                                  "C:/workspace") == tokmon::PolicyEffect::ask);
}

TEST_CASE("runtime profiles keep Calculator out of production") {
  const auto root = temporary_directory("runtime-profiles");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";

  auto production = tokmon::load_config(workspace);
  REQUIRE(production);
  REQUIRE(production->profile == tokmon::RuntimeProfile::production);
  REQUIRE(std::ranges::none_of(production->light_path, [](const auto& lens) {
    return lens.enabled && lens.id == "org.tokmon.lens.calculator";
  }));

  write_runtime_profile(root, "development");
  auto development = tokmon::load_config(workspace);
  REQUIRE(development);
  REQUIRE(development->profile == tokmon::RuntimeProfile::development);
  REQUIRE(std::ranges::count_if(development->light_path, [](const auto& lens) {
    return lens.enabled && lens.id == "org.tokmon.lens.calculator";
  }) == 1);

  write_runtime_profile(root, "production");
  std::filesystem::create_directories(workspace / ".tokmon");
  std::ofstream(workspace / ".tokmon" / "light-path.yaml")
      << "api: tokmon.light-path/wavefront\nlenses:\n"
         "  - { id: org.tokmon.lens.calculator, artifact: builtin:calculator, enabled: true, runtime: in_process }\n";
  auto forbidden = tokmon::load_config(workspace);
  REQUIRE_FALSE(forbidden);
  REQUIRE(forbidden.error().code == tokmon::ErrorCode::permission_denied);
}

TEST_CASE("Lens YAML management emits the strict loadable schema") {
  const auto root = temporary_directory("light-path-writer");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  const auto project = workspace / ".tokmon";
  std::filesystem::create_directories(project);

  auto unmounted = tokmon::update_light_path_document(
      tokmon::cbor::object({{"version", 1}}), "org.tokmon.lens.nota",
      std::nullopt, std::nullopt, false);
  REQUIRE(unmounted);
  REQUIRE(tokmon::cbor::find(*unmounted, "version") == nullptr);
  REQUIRE(tokmon::cbor::find(*unmounted, "api")->as_string() ==
          "tokmon.light-path/wavefront");
  auto yaml = tokmon::yaml::stringify(*unmounted);
  REQUIRE(yaml);
  std::ofstream(project / "light-path.yaml") << *yaml;
  auto disabled = tokmon::load_config(workspace);
  REQUIRE(disabled);
  REQUIRE(std::ranges::none_of(disabled->light_path, [](const auto& lens) {
    return lens.enabled && lens.id == "org.tokmon.lens.nota";
  }));

  auto mounted = tokmon::update_light_path_document(std::move(*unmounted),
      "org.tokmon.lens.nota", "builtin:nota", "in_process", true);
  REQUIRE(mounted);
  yaml = tokmon::yaml::stringify(*mounted);
  REQUIRE(yaml);
  std::ofstream(project / "light-path.yaml", std::ios::trunc) << *yaml;
  auto enabled = tokmon::load_config(workspace);
  REQUIRE(enabled);
  REQUIRE(std::ranges::any_of(enabled->light_path, [](const auto& lens) {
    return lens.enabled && lens.id == "org.tokmon.lens.nota" &&
        lens.artifact == "builtin:nota";
  }));
}

TEST_CASE("Act approved boolean cannot bypass the common admission decision") {
  auto snapshot = std::make_shared<tokmon::LightPathSnapshot>();
  snapshot->epoch = 9;
  snapshot->lenses.push_back(tokmon::MountedLens{
      tokmon::make_builtin_lens("cove"), 901, "builtin"});
  tokmon::Act write{.id = "act-policy", .ray = "ray-policy", .kind = "fs.write",
      .schema = "tokmon.fs.write.v1", .parameters = tokmon::cbor::object({}),
      .epoch = 9, .risk = tokmon::RiskClass::external_irreversible, .approved = true};
  tokmon::ActPipeline asks([](const tokmon::Act&) {
    return tokmon::AdmissionDecision::ask;
  });
  auto rejected = asks.admit(write, *snapshot);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == tokmon::ErrorCode::approval_required);
  tokmon::ActPipeline allows([](const tokmon::Act&) {
    return tokmon::AdmissionDecision::allow;
  });
  REQUIRE(allows.admit(std::move(write), *snapshot));
}

TEST_CASE("calculator executes the complete Fact Lens Act photon loop") {
  const auto root = temporary_directory("runtime");
  UserProfileGuard profile(root / "home");
  write_runtime_profile(root, "test");
  tokmon::TokmonRuntime runtime;
  REQUIRE(runtime.open(root / "workspace", "tokmon-tests"));
  auto ray = runtime.submit("请计算 128 * 4", tokmon::cbor::object({
      {"provider", "local"}, {"protocol", "local"},
      {"model", "local-deterministic"}, {"access_mode", "受限访问"},
      {"effort", "最高"}}));
  REQUIRE(ray);
  auto beats = runtime.advance(*ray);
  REQUIRE(beats);
  REQUIRE(*beats == 3);
  auto photons = runtime.history(*ray);
  REQUIRE(photons);
  const auto has_kind = [&photons](const std::string_view kind) {
    return std::any_of(photons->begin(), photons->end(),
        [kind](const tokmon::Photon& photon) { return photon.kind == kind; });
  };
  REQUIRE(has_kind("model.tool-call"));
  REQUIRE(has_kind("tool.result"));
  REQUIRE(has_kind("assistant.message"));
  const auto input = std::ranges::find_if(*photons, [](const tokmon::Photon& photon) {
    return photon.kind == "user.input";
  });
  REQUIRE(input != photons->end());
  REQUIRE(tokmon::cbor::find(input->payload, "model")->as_string() ==
          "local-deterministic");
  REQUIRE(has_kind("model.reasoning-chunk"));
  REQUIRE(photons->back().kind == "ray.darkened");
  const auto first_tail = photons->back().sequence;
  auto continued = runtime.submit_to(*ray, "3 + 7");
  REQUIRE(continued);
  REQUIRE(*continued == *ray);
  auto continued_beats = runtime.advance(*ray);
  REQUIRE(continued_beats);
  auto continued_photons = runtime.history(*ray);
  REQUIRE(continued_photons);
  REQUIRE(std::ranges::count_if(*continued_photons, [](const tokmon::Photon& photon) {
    return photon.kind == "user.input";
  }) == 2);
  REQUIRE(continued_photons->back().sequence > first_tail);
  REQUIRE_FALSE(runtime.submit_to("ray-does-not-exist", "orphan input"));
  const auto first_epoch = runtime.light_path()->epoch;
  auto before_reconcile = runtime.history_all();
  REQUIRE(before_reconcile);
  REQUIRE(runtime.reconcile());
  REQUIRE(runtime.light_path()->epoch == first_epoch);
  auto all = runtime.history_all();
  REQUIRE(all);
  REQUIRE(all->size() == before_reconcile->size());
  const auto system_has_kind = [&all](const std::string_view kind) {
    return std::any_of(all->begin(), all->end(),
        [kind](const tokmon::Photon& photon) { return photon.kind == kind; });
  };
  REQUIRE(system_has_kind("config.light-path-observed"));
  REQUIRE(system_has_kind("lens.candidate-verified"));
  REQUIRE(system_has_kind("mount.epoch-committed"));
  REQUIRE(runtime.verify());
}

TEST_CASE("Nyxia recovery marks unterminated in-flight Act outcome unknown") {
  const auto root = temporary_directory("recovery");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  const auto ray = "ray-recovery";
  {
    tokmon::TokmonRuntime runtime;
    REQUIRE(runtime.open(workspace, "tokmon-tests-recovery-one"));
    auto started = runtime.store().append(tokmon::PhotonDraft{.ray = ray,
        .kind = "act.started", .schema = "tokmon.act.audit.v1",
        .payload = tokmon::cbor::object({{"act_hash", std::string(64, 'f')}}),
        .epoch = runtime.light_path()->epoch, .caused_by_act = "act-interrupted"});
    REQUIRE(started);
  }
  tokmon::TokmonRuntime recovered;
  REQUIRE(recovered.open(workspace, "tokmon-tests-recovery-two"));
  auto photons = recovered.history(ray);
  REQUIRE(photons);
  REQUIRE(std::any_of(photons->begin(), photons->end(), [](const tokmon::Photon& photon) {
    return photon.kind == "act.outcome-unknown" &&
           photon.caused_by_act == "act-interrupted";
  }));
  REQUIRE(recovered.verify());
}

TEST_CASE("mount epochs and generations remain monotonic across runtime restarts") {
  const auto root = temporary_directory("mount-identity-recovery");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  tokmon::MountEpoch first_epoch = 0;
  tokmon::GenerationId first_max_generation = 0;
  {
    tokmon::TokmonRuntime first;
    REQUIRE(first.open(workspace, "tokmon-mount-identity-one"));
    first_epoch = first.light_path()->epoch;
    for (const auto& mounted : first.light_path()->lenses)
      first_max_generation = std::max(first_max_generation, mounted.generation);
  }
  {
    tokmon::TokmonRuntime second;
    REQUIRE(second.open(workspace, "tokmon-mount-identity-two"));
    REQUIRE(second.light_path()->epoch > first_epoch);
    REQUIRE(std::ranges::all_of(second.light_path()->lenses,
        [&](const tokmon::MountedLens& mounted) {
          return mounted.generation > first_max_generation;
        }));
  }
}

TEST_CASE("language Lens manifests carry an exact runtime entry") {
  const auto source = std::filesystem::path(TOKMON_SOURCE_DIR);
  auto node = tokmon::load_lens_manifest(source / "sdk/typescript/examples/lens.yaml");
  REQUIRE(node);
  REQUIRE(node->runtime == tokmon::RuntimeKind::node);
  REQUIRE(node->runtime_entry == "adder.mjs");
  auto python = tokmon::load_lens_manifest(source / "sdk/python/examples/lens.yaml");
  REQUIRE(python);
  REQUIRE(python->runtime == tokmon::RuntimeKind::cpython);
  REQUIRE(python->runtime_entry == "adder.py");
}

TEST_CASE("Lens manifest parses dependency order resources and immutable evidence") {
  const auto root = temporary_directory("rich-manifest");
  const auto path = root / "lens.yaml";
  std::ofstream output(path);
  output << "api: tokmon.lens/wavefront\n"
      "id: org.example.rich\n"
      "display_name: Rich lens\n"
      "version: 2.1.0\n"
      "abi: { major: 2, minor: 0 }\n"
      "runtime: { kind: node, version: 24.0.0, entry: main.mjs }\n"
      "observes: [{ kind: user.input, schema: '*' }]\n"
      "inputs: []\n"
      "outputs: [{ port: model.tools, band: model.tools, schema: tokmon.surface.contribution.v1, merge: stable_concat, surface: true }]\n"
      "refracts: [{ kind: example.run, schema: example.run.v1 }]\n"
      "light_permissions: [photon.emit]\n"
      "dependencies: [{ id: org.tokmon.lens.techor, version: ^0.1.0 }]\n"
      "conflicts: [org.example.legacy]\n"
      "optical_order: { after: [org.tokmon.lens.techor], before: [org.tokmon.lens.rhea] }\n"
      "resources: { memory_mb: 512, output_bytes: 2097152, deadline_ms: 45000 }\n"
      "replacement: R2\n"
      "schema_bundle: schemas.cbor\n"
      "sbom: sbom.cdx.json\n";
  output.close();
  auto manifest = tokmon::load_lens_manifest(path);
  REQUIRE(manifest);
  REQUIRE(manifest->dependencies.size() == 1);
  REQUIRE(manifest->dependencies.front().id == "org.tokmon.lens.techor");
  REQUIRE(manifest->optical_after.front() == "org.tokmon.lens.techor");
  REQUIRE(manifest->resources.memory_mb == 512);
  REQUIRE(manifest->resources.deadline == std::chrono::milliseconds(45'000));
  REQUIRE(manifest->replacement == "R2");
  REQUIRE(manifest->schema_bundle == "schemas.cbor");
}

TEST_CASE("LightPath refuses missing dependencies conflicts and invalid optical order") {
  const auto basic = [](std::string id) {
    return tokmon::LensManifest{.id = std::move(id), .display_name = "test",
        .outputs = {{.name = "test.channel", .band = "test.channel",
                     .schema = "tokmon.test.field.v1", .surface = true}},
        .refracts = {{"test.run", "*"}},
        .light_permissions = {"photon.emit"}};
  };
  {
    tokmon::LightPath path;
    auto dependent = basic("org.example.dependent");
    dependent.dependencies.push_back({"org.example.required", "1.0.0"});
    auto candidate = std::make_shared<tokmon::LightPathSnapshot>();
    candidate->epoch = 1;
    candidate->lenses.push_back({std::make_shared<ManifestLens>(dependent), 1, "a"});
    auto published = path.publish(candidate);
    REQUIRE_FALSE(published);
    REQUIRE(published.error().code == tokmon::ErrorCode::not_found);
  }
  {
    tokmon::LightPath path;
    auto first = basic("org.example.first");
    first.conflicts.push_back("org.example.second");
    auto second = basic("org.example.second");
    second.refracts = {{"test.other", "*"}};
    auto candidate = std::make_shared<tokmon::LightPathSnapshot>();
    candidate->epoch = 1;
    candidate->lenses.push_back({std::make_shared<ManifestLens>(first), 1, "a"});
    candidate->lenses.push_back({std::make_shared<ManifestLens>(second), 2, "b"});
    REQUIRE_FALSE(path.publish(candidate));
  }
  {
    tokmon::LightPath path;
    auto first = basic("org.example.first");
    first.optical_after.push_back("org.example.second");
    auto second = basic("org.example.second");
    second.refracts = {{"test.other", "*"}};
    auto candidate = std::make_shared<tokmon::LightPathSnapshot>();
    candidate->epoch = 1;
    candidate->lenses.push_back({std::make_shared<ManifestLens>(first), 1, "a"});
    candidate->lenses.push_back({std::make_shared<ManifestLens>(second), 2, "b"});
    REQUIRE_FALSE(path.publish(candidate));
  }
}

TEST_CASE("signature-required runtime rejects an unlocked external Lens") {
  const auto root = temporary_directory("signature-required");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  const auto project = workspace / ".tokmon";
  const auto artifact = project / "calculator-artifact";
  std::filesystem::create_directories(artifact);
  std::filesystem::create_directories(root / "home" / ".tokmon");
  std::ofstream(root / "home" / ".tokmon" / "config.yaml")
      << "profile: test\nsecurity:\n  require_signatures: true\n";
#if defined(_WIN32)
  constexpr auto library_name = "calculator.dll";
#elif defined(__APPLE__)
  constexpr auto library_name = "libcalculator.dylib";
#else
  constexpr auto library_name = "libcalculator.so";
#endif
  std::filesystem::copy_file(TOKMON_TEST_LENS_PATH, artifact / library_name,
                             std::filesystem::copy_options::overwrite_existing);
  std::ofstream(artifact / "lens.yaml")
      << "id: org.tokmon.lens.calculator\n"
      "display_name: Calculator\nversion: 0.1.0\n"
      "abi: { major: 2, minor: 0 }\n"
      "runtime: { kind: in_process, entry: " << library_name << " }\n"
      "observes: [{ kind: user.input, schema: tokmon.user.input.v1 }]\n"
      "inputs: []\n"
      "outputs: [{ port: model.tools, band: model.tools, schema: tokmon.surface.contribution.v1, merge: stable_concat, surface: true }]\n"
      "refracts: [{ kind: tool.calculate, schema: tokmon.math.calculate.v1 }]\n"
      "light_permissions: [photon.emit, log.write]\n";
  std::ofstream(project / "light-path.yaml")
      << "api: tokmon.light-path/wavefront\nlenses:\n"
      "  - { id: org.tokmon.lens.calculator, artifact: calculator-artifact, enabled: true, runtime: in_process }\n";
  tokmon::TokmonRuntime runtime;
  auto opened = runtime.open(workspace, "tokmon-signature-test");
  REQUIRE_FALSE(opened);
  REQUIRE(opened.error().code == tokmon::ErrorCode::integrity_error);
}

TEST_CASE("runtime hot swaps a C ABI Lens generation through a higher epoch") {
  const auto root = temporary_directory("c-abi-hot-swap");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  const auto project = workspace / ".tokmon";
  const auto artifact = project / "calculator-artifact";
  std::filesystem::create_directories(artifact);
  write_runtime_profile(root, "test");
#if defined(_WIN32)
  constexpr auto library_name = "calculator.dll";
#elif defined(__APPLE__)
  constexpr auto library_name = "libcalculator.dylib";
#else
  constexpr auto library_name = "libcalculator.so";
#endif
  std::filesystem::copy_file(TOKMON_TEST_LENS_PATH, artifact / library_name,
                             std::filesystem::copy_options::overwrite_existing);
  {
    std::ofstream manifest(artifact / "lens.yaml");
    manifest << "id: org.tokmon.lens.calculator\n"
        "display_name: Calculator / 计算透镜\n"
        "version: 0.1.0\n"
        "abi: { major: 2, minor: 0 }\n"
        "runtime: { kind: in_process, entry: " << library_name << " }\n"
        "trust: t1\nstateless: true\n"
        "observes:\n  - { kind: user.input, schema: tokmon.user.input.v1 }\n"
        "inputs: []\n"
        "outputs: [{ port: model.tools, band: model.tools, schema: tokmon.surface.contribution.v1, merge: stable_concat, surface: true }]\n"
        "refracts:\n  - { kind: tool.calculate, schema: tokmon.math.calculate.v1 }\n"
        "light_permissions: [photon.emit, log.write]\n";
  }
  {
    std::ofstream path(project / "light-path.yaml");
    path << "api: tokmon.light-path/wavefront\nlenses:\n"
        "  - id: org.tokmon.lens.calculator\n"
        "    artifact: calculator-artifact\n"
        "    enabled: true\n"
        "    runtime: in_process\n";
  }

  tokmon::TokmonRuntime runtime;
  REQUIRE(runtime.open(workspace, "tokmon-tests"));
  const auto first = runtime.light_path();
  REQUIRE(first->lenses.size() == 20);
  const auto find_calculator = [](const auto& snapshot) {
    return std::ranges::find_if(snapshot->lenses, [](const auto& mounted) {
      return mounted.lens->manifest().id == "org.tokmon.lens.calculator";
    });
  };
  const auto first_calculator = find_calculator(first);
  REQUIRE(first_calculator != first->lenses.end());
  REQUIRE(dynamic_cast<tokmon::CAbiLens*>(first_calculator->lens.get()) != nullptr);
  const auto first_epoch = first->epoch;
  const auto first_hash = first_calculator->artifact_hash;

  // Changing bytes under the same configured artifact path must not be hidden
  // by the in-memory configuration hash. Only that Lens receives a new
  // generation; every unchanged mount is reused.
  std::ofstream(artifact / "revision.txt") << "revision two\n";
  REQUIRE(runtime.reconcile());
  const auto content_replaced = runtime.light_path();
  REQUIRE(content_replaced->epoch == first_epoch + 1);
  const auto content_calculator = find_calculator(content_replaced);
  REQUIRE(content_calculator != content_replaced->lenses.end());
  REQUIRE(content_calculator->artifact_hash != first_hash);
  REQUIRE(content_calculator->generation != first_calculator->generation);
  REQUIRE(dynamic_cast<tokmon::CAbiLens*>(content_calculator->lens.get()) != nullptr);
  for (const auto& old : first->lenses) {
    if (old.lens->manifest().id == "org.tokmon.lens.calculator") continue;
    const auto same = std::ranges::find_if(content_replaced->lenses,
        [&](const auto& mounted) {
          return mounted.lens->manifest().id == old.lens->manifest().id;
        });
    REQUIRE(same != content_replaced->lenses.end());
    REQUIRE(same->generation == old.generation);
    REQUIRE(same->lens == old.lens);
  }

  {
    std::ofstream path(project / "light-path.yaml", std::ios::trunc);
    path << "api: tokmon.light-path/wavefront\nlenses:\n"
        "  - id: org.tokmon.lens.calculator\n"
        "    artifact: builtin:calculator\n"
        "    enabled: true\n"
        "    runtime: in_process\n";
  }
  REQUIRE(runtime.reconcile());
  const auto builtin_replaced = runtime.light_path();
  REQUIRE(builtin_replaced->epoch == content_replaced->epoch + 1);
  REQUIRE(builtin_replaced->lenses.size() == 20);
  const auto builtin_calculator = find_calculator(builtin_replaced);
  REQUIRE(builtin_calculator != builtin_replaced->lenses.end());
  REQUIRE(builtin_calculator->artifact_hash != content_calculator->artifact_hash);
  REQUIRE(builtin_calculator->generation != content_calculator->generation);
  REQUIRE(dynamic_cast<tokmon::CAbiLens*>(builtin_calculator->lens.get()) == nullptr);
  for (const auto& old : content_replaced->lenses) {
    if (old.lens->manifest().id == "org.tokmon.lens.calculator") continue;
    const auto same = std::ranges::find_if(builtin_replaced->lenses,
        [&](const auto& mounted) {
          return mounted.lens->manifest().id == old.lens->manifest().id;
        });
    REQUIRE(same != builtin_replaced->lenses.end());
    REQUIRE(same->generation == old.generation);
  }
  REQUIRE(runtime.verify());
}

TEST_CASE("unknown YAML fields are rejected without publishing a path") {
  const auto root = temporary_directory("yaml");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace / ".tokmon");
  std::ofstream output(workspace / ".tokmon" / "config.yaml");
  output << "engine:\n  max_beats: 4\n  silent_unknown: true\n";
  output.close();
  auto config = tokmon::load_config(workspace);
  REQUIRE_FALSE(config);
  REQUIRE(config.error().code == tokmon::ErrorCode::schema_mismatch);

  {
    std::ofstream invalid_type(workspace / ".tokmon" / "config.yaml", std::ios::trunc);
    invalid_type << "security:\n  require_signatures: \"true\"\n";
  }
  config = tokmon::load_config(workspace);
  REQUIRE_FALSE(config);
  REQUIRE(config.error().code == tokmon::ErrorCode::schema_mismatch);
}

TEST_CASE("model platforms merge by id while credentials remain SecretRefs") {
  const auto root = temporary_directory("model-config");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  std::filesystem::create_directories(root / "home" / ".tokmon");
  std::filesystem::create_directories(workspace / ".tokmon");
  {
    std::ofstream user(root / "home" / ".tokmon" / "config.yaml");
    user << "models:\n"
            "  default: private-cloud\n"
            "  providers:\n"
            "    private-cloud:\n"
            "      protocol: openai-compatible\n"
            "      endpoint: https://models.example.test/v1/chat/completions\n"
            "      model: base-model\n"
            "      secret_ref: model-provider/private-cloud\n"
            "      secret_env: PRIVATE_CLOUD_API_KEY\n"
            "      auth: bearer\n"
            "      temperature: 0.25\n"
            "      stop:\n"
            "        - END\n"
            "        - STOP\n"
            "      request_parameters:\n"
            "        response_format:\n"
            "          type: json_object\n";
  }
  {
    std::ofstream project(workspace / ".tokmon" / "config.yaml");
    project << "models:\n"
               "  providers:\n"
               "    private-cloud:\n"
               "      model: project-model\n"
               "      thinking: true\n"
               "      top_p: 0.9\n";
  }
  auto config = tokmon::load_config(workspace);
  REQUIRE(config);
  REQUIRE(config->default_model_provider == "private-cloud");
  const auto& provider = config->model_providers.at("private-cloud");
  REQUIRE(provider.protocol == "openai-compatible");
  REQUIRE(provider.model == "project-model");
  REQUIRE(provider.endpoint == "https://models.example.test/v1/chat/completions");
  REQUIRE(provider.secret_ref == "model-provider/private-cloud");
  REQUIRE(provider.secret_env == "PRIVATE_CLOUD_API_KEY");
  REQUIRE(provider.thinking);
  const auto* request_parameters = provider.request_parameters.as_map();
  REQUIRE(request_parameters != nullptr);
  REQUIRE(std::get<double>(request_parameters->at("temperature").data) == 0.25);
  REQUIRE(std::get<double>(request_parameters->at("top_p").data) == 0.9);
  REQUIRE(request_parameters->at("stop").as_array() != nullptr);
  REQUIRE(request_parameters->at("stop").as_array()->size() == 2);
  const auto* response_format = request_parameters->at("response_format").as_map();
  REQUIRE(response_format != nullptr);
  REQUIRE(response_format->at("type").as_string() == "json_object");
}

TEST_CASE("model context keeps provider and configured model inseparable") {
  tokmon::RuntimeConfig config;
  config.paths.project = "E:/workspace/.tokmon";
  config.default_model_provider = "opencode";
  config.model_providers.emplace("opencode", tokmon::ModelProviderConfig{
      .id = "opencode", .protocol = "openai-compatible",
      .endpoint = "https://opencode.example/v1/chat/completions",
      .model = "x-preview-f-free", .secret_ref = "model-provider/opencode",
      .auth = "bearer"});
  config.model_providers.emplace("deepseek", tokmon::ModelProviderConfig{
      .id = "deepseek", .protocol = "openai-compatible",
      .endpoint = "https://api.deepseek.com/chat/completions",
      .model = "deepseek-v4-flash", .secret_ref = "model-provider/deepseek",
      .auth = "bearer", .thinking = true, .reasoning_effort = "high",
      .request_parameters = tokmon::cbor::object({{"temperature", 0.3}})});

  auto selected = tokmon::resolve_model_provider_context(config,
      tokmon::cbor::object({{"provider", "deepseek"},
                            {"model", "deepseek-v4-flash"},
                            {"effort", "高"}}));
  REQUIRE(selected);
  REQUIRE(tokmon::cbor::find(*selected, "provider")->as_string() == "deepseek");
  REQUIRE(tokmon::cbor::find(*selected, "model")->as_string() ==
          "deepseek-v4-flash");
  REQUIRE(tokmon::cbor::find(*selected, "endpoint")->as_string() ==
          "https://api.deepseek.com/chat/completions");
  const auto* selected_parameters =
      tokmon::cbor::find(*selected, "request_parameters");
  REQUIRE(selected_parameters != nullptr);
  REQUIRE(std::get<double>(
      tokmon::cbor::find(*selected_parameters, "temperature")->data) == 0.3);

  auto mismatch = tokmon::resolve_model_provider_context(config,
      tokmon::cbor::object({{"provider", "opencode"},
                            {"model", "deepseek-v4-flash"}}));
  REQUIRE_FALSE(mismatch);
  REQUIRE(mismatch.error().code == tokmon::ErrorCode::schema_mismatch);

  auto fallback = tokmon::resolve_model_provider_context(
      config, tokmon::cbor::Value::Map{});
  REQUIRE(fallback);
  REQUIRE(tokmon::cbor::find(*fallback, "provider")->as_string() == "opencode");
  REQUIRE(tokmon::cbor::find(*fallback, "model")->as_string() ==
          "x-preview-f-free");
}

TEST_CASE("model configuration rejects plaintext keys and insecure remote endpoints") {
  const auto root = temporary_directory("model-config-reject");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace / ".tokmon");
  const auto file = workspace / ".tokmon" / "config.yaml";
  {
    std::ofstream output(file);
    output << "models:\n  providers:\n    unsafe:\n"
              "      protocol: openai-compatible\n"
              "      endpoint: https://example.test/v1/chat/completions\n"
              "      model: test\n      api_key: plaintext-is-forbidden\n";
  }
  auto plaintext = tokmon::load_config(workspace);
  REQUIRE_FALSE(plaintext);
  REQUIRE(plaintext.error().code == tokmon::ErrorCode::permission_denied);
  {
    std::ofstream output(file, std::ios::trunc);
    output << "models:\n  providers:\n    unsafe:\n"
              "      protocol: openai-compatible\n"
              "      endpoint: http://models.example.test/v1/chat/completions\n"
              "      model: test\n      secret_ref: model-provider/unsafe\n";
  }
  auto insecure = tokmon::load_config(workspace);
  REQUIRE_FALSE(insecure);
  REQUIRE(insecure.error().code == tokmon::ErrorCode::permission_denied);
  {
    std::ofstream output(file, std::ios::trunc);
    output << "models:\n  providers:\n    unsafe:\n"
              "      protocol: openai-compatible\n"
              "      endpoint: https://models.example.test/v1/chat/completions\n"
              "      model: test\n      secret_ref: model-provider/unsafe\n"
              "      secret_env: unsafeApiKey\n";
  }
  auto invalid_environment = tokmon::load_config(workspace);
  REQUIRE_FALSE(invalid_environment);
  REQUIRE(invalid_environment.error().code == tokmon::ErrorCode::schema_mismatch);
}

TEST_CASE("CLI reports the daemon authoritative configuration error") {
  const auto root = temporary_directory("cli-config-error");
  UserProfileGuard profile(root / "home");
  const auto workspace = root / "workspace";
  std::filesystem::create_directories(root / "home" / ".tokmon");
  std::filesystem::create_directories(workspace / ".tokmon");
  {
    std::ofstream output(workspace / ".tokmon" / "config.yaml");
    output << "engine:\n  max_beats: 4\n  misspelled_limit: 9\n";
  }
  auto invoked = tokmon::builtin::run_process(tokmon::builtin::ProcessRequest{
      .argv = {TOKMON_CLI_EXECUTABLE, "--workspace", workspace.string(),
               "model", "list"},
      .cwd = root,
      .timeout = std::chrono::seconds(5),
      .max_output_bytes = 16u * 1024u});
  REQUIRE(invoked);
  REQUIRE(invoked->exit_code != 0);
  REQUIRE_FALSE(invoked->timed_out);
  REQUIRE(invoked->stderr_text.find("unknown YAML field 'misspelled_limit'") !=
          std::string::npos);
  REQUIRE(invoked->stderr_text.find("did not become ready") == std::string::npos);
}

TEST_CASE("workspace Snow endpoints are isolated and daemon health is probeable") {
  const auto root = temporary_directory("workspace-daemon-endpoints");
  const auto first = tokmon::workspace_snow_endpoint(root / "run", root / "one");
  const auto second = tokmon::workspace_snow_endpoint(root / "run", root / "two");
  REQUIRE(first != second);

  tokmon::SnowServer server;
  REQUIRE(server.start(first, [](const tokmon::SnowMessage& request) {
    return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::pong,
        .request_id = request.request_id, .cursor = request.cursor,
        .payload = tokmon::cbor::object({{"healthy", true}})};
  }));
  tokmon::Result<bool> available = false;
  for (int attempt = 0; attempt < 20 && (!available || !*available); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    available = tokmon::daemon_available(first);
  }
  REQUIRE(available);
  REQUIRE(*available);
  auto other = tokmon::daemon_available(second);
  REQUIRE(other);
  REQUIRE_FALSE(*other);
  server.stop();
  available = tokmon::daemon_available(first);
  REQUIRE(available);
  REQUIRE_FALSE(*available);
}

TEST_CASE("daemon client leases attach, renew, detach, and explicit starts pin") {
  const auto root = temporary_directory("daemon-client-lease");
  const auto endpoint = tokmon::default_snow_endpoint(root);
  std::mutex actions_mutex;
  std::vector<std::string> actions;
  tokmon::SnowServer server;
  REQUIRE(server.start(endpoint, [&actions_mutex, &actions](
      const tokmon::SnowMessage& request) {
    const auto* action = tokmon::cbor::find(request.payload, "action");
    {
      std::scoped_lock lock(actions_mutex);
      actions.emplace_back(action ? action->as_string() : std::string_view{});
    }
    return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
        .request_id = request.request_id, .cursor = request.cursor,
        .payload = tokmon::cbor::object({{"accepted", true}})};
  }));

  auto lease = tokmon::DaemonClientLease::attach(tokmon::DaemonClientOptions{
      .endpoint = endpoint,
      .client_id = "desktop-test",
      .client_kind = "desktop",
      .shutdown_when_idle = true,
      .idle_timeout = std::chrono::milliseconds(0),
      .lease_ttl = std::chrono::seconds(2)});
  REQUIRE(lease);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  REQUIRE(lease->detach());
  REQUIRE(tokmon::pin_daemon(endpoint));
  server.stop();

  std::scoped_lock lock(actions_mutex);
  REQUIRE_FALSE(actions.empty());
  REQUIRE(actions.front() == "daemon.client.attach");
  REQUIRE(std::ranges::find(actions, "daemon.client.heartbeat") != actions.end());
  REQUIRE(std::ranges::find(actions, "daemon.client.detach") != actions.end());
  REQUIRE(actions.back() == "daemon.pin");
}
