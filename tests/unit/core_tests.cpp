#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
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

class ManifestLens final : public tokmon::ILens {
 public:
  explicit ManifestLens(tokmon::LensManifest manifest) : manifest_(std::move(manifest)) {}
  const tokmon::LensManifest& manifest() const noexcept override { return manifest_; }
  tokmon::Result<void> view(const tokmon::PhotonWindow&,
                            tokmon::SurfaceBuilder&) override { return {}; }
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
  REQUIRE_FALSE(tokmon::json::parse("{broken"));
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
  char* error = nullptr;
  REQUIRE(sqlite3_exec(connection, "UPDATE photons SET kind='tampered' WHERE seq=1",
                       nullptr, nullptr, &error) != SQLITE_OK);
  sqlite3_free(error); error = nullptr;
  REQUIRE(sqlite3_exec(connection, "DELETE FROM photons WHERE seq=1",
                       nullptr, nullptr, &error) != SQLITE_OK);
  sqlite3_free(error);
  sqlite3_close(connection);
  REQUIRE(store.verify());
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

TEST_CASE("C ABI Lens loads and passes a dark-lane view") {
  auto lens = tokmon::CAbiLens::load(TOKMON_TEST_LENS_PATH);
  REQUIRE(lens);
  REQUIRE((*lens)->manifest().id == "org.tokmon.lens.calculator");
  tokmon::SurfaceBuilder surface((*lens)->manifest().id);
  REQUIRE((*lens)->view(tokmon::PhotonWindow{}, surface));
  REQUIRE_FALSE(surface.contributions().empty());
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
  REQUIRE(runtime.reconcile());
  REQUIRE(runtime.light_path()->epoch == first_epoch + 1);
  auto all = runtime.history_all();
  REQUIRE(all);
  const auto system_has_kind = [&all](const std::string_view kind) {
    return std::any_of(all->begin(), all->end(),
        [kind](const tokmon::Photon& photon) { return photon.kind == kind; });
  };
  REQUIRE(system_has_kind("config.light-path-observed"));
  REQUIRE(system_has_kind("lens.candidate-verified"));
  REQUIRE(system_has_kind("mount.epoch-committed"));
  REQUIRE(system_has_kind("lens.afterglow-started"));
  REQUIRE(system_has_kind("lens.afterglow-completed"));
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
  output << "api: tokmon.lens/v1\n"
      "id: org.example.rich\n"
      "display_name: Rich lens\n"
      "version: 2.1.0\n"
      "abi: { major: 1, minor: 0 }\n"
      "runtime: { kind: node, version: 24.0.0, entry: main.mjs }\n"
      "observes: [{ kind: user.input, schema: '*' }]\n"
      "view_channels: [model.tools]\n"
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
        .view_channels = {"test.channel"}, .refracts = {{"test.run", "*"}},
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
      << "security:\n  require_signatures: true\n";
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
      "abi: { major: 1, minor: 0 }\n"
      "runtime: { kind: in_process, entry: " << library_name << " }\n"
      "observes: [{ kind: user.input, schema: tokmon.user.input.v1 }]\n"
      "view_channels: [model.tools]\n"
      "refracts: [{ kind: tool.calculate, schema: tokmon.math.calculate.v1 }]\n"
      "light_permissions: [photon.emit, log.write]\n";
  std::ofstream(project / "light-path.yaml")
      << "version: 1\nlenses:\n"
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
        "abi: { major: 1, minor: 0 }\n"
        "runtime: { kind: in_process, entry: " << library_name << " }\n"
        "trust: t1\nstateless: true\n"
        "observes:\n  - { kind: user.input, schema: tokmon.user.input.v1 }\n"
        "view_channels: [model.tools]\n"
        "refracts:\n  - { kind: tool.calculate, schema: tokmon.math.calculate.v1 }\n"
        "light_permissions: [photon.emit, log.write]\n";
  }
  {
    std::ofstream path(project / "light-path.yaml");
    path << "version: 1\nlenses:\n"
        "  - id: org.tokmon.lens.calculator\n"
        "    artifact: calculator-artifact\n"
        "    enabled: true\n"
        "    runtime: in_process\n";
  }

  tokmon::TokmonRuntime runtime;
  REQUIRE(runtime.open(workspace, "tokmon-tests"));
  const auto first = runtime.light_path();
  REQUIRE(first->lenses.size() == 20);
  REQUIRE(dynamic_cast<tokmon::CAbiLens*>(
      runtime.light_path()->lenses.back().lens.get()) != nullptr);
  const auto first_epoch = first->epoch;
  const auto first_hash = first->lenses.back().artifact_hash;

  {
    std::ofstream path(project / "light-path.yaml", std::ios::trunc);
    path << "version: 1\nlenses:\n"
        "  - id: org.tokmon.lens.calculator\n"
        "    artifact: builtin:calculator\n"
        "    enabled: true\n"
        "    runtime: in_process\n";
  }
  REQUIRE(runtime.reconcile());
  const auto second = runtime.light_path();
  REQUIRE(second->epoch == first_epoch + 1);
  REQUIRE(second->lenses.size() == 20);
  REQUIRE(second->lenses.back().artifact_hash != first_hash);
  REQUIRE(dynamic_cast<tokmon::CAbiLens*>(second->lenses.back().lens.get()) == nullptr);
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
            "      auth: bearer\n";
  }
  {
    std::ofstream project(workspace / ".tokmon" / "config.yaml");
    project << "models:\n"
               "  providers:\n"
               "    private-cloud:\n"
               "      model: project-model\n"
               "      thinking: true\n";
  }
  auto config = tokmon::load_config(workspace);
  REQUIRE(config);
  REQUIRE(config->default_model_provider == "private-cloud");
  const auto& provider = config->model_providers.at("private-cloud");
  REQUIRE(provider.protocol == "openai-compatible");
  REQUIRE(provider.model == "project-model");
  REQUIRE(provider.endpoint == "https://models.example.test/v1/chat/completions");
  REQUIRE(provider.secret_ref == "model-provider/private-cloud");
  REQUIRE(provider.thinking);
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
  REQUIRE(plaintext.error().code == tokmon::ErrorCode::schema_mismatch);
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
