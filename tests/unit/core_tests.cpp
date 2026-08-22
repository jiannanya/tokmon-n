#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

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

}  // namespace

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

TEST_CASE("calculator executes the complete Fact Lens Act photon loop") {
  const auto root = temporary_directory("runtime");
  UserProfileGuard profile(root / "home");
  tokmon::TokmonRuntime runtime;
  REQUIRE(runtime.open(root / "workspace", "tokmon-tests"));
  auto ray = runtime.submit("128 * 4");
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
  REQUIRE(photons->back().kind == "ray.darkened");
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
