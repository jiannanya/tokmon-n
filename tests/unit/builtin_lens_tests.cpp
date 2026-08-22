#include <filesystem>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tokmon/tokmon.hpp"

namespace {

std::filesystem::path lens_temporary_directory(const std::string_view name) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("tokmon-lens-" + std::string(name) + "-" + tokmon::make_id("test"));
  std::filesystem::create_directories(directory);
  return directory;
}

class RecordingHost final : public tokmon::OpticalHost {
 public:
  tokmon::Result<tokmon::Photon> emit(tokmon::PhotonDraft draft) override {
    drafts.push_back(draft);
    return tokmon::Photon{.sequence = next_sequence++,
        .id = tokmon::make_id("photon"), .ray = draft.ray, .parent = draft.parent,
        .kind = draft.kind, .schema = draft.schema, .payload = draft.payload,
        .epoch = draft.epoch, .hash = std::string(64, 'a'),
        .caused_by_act = draft.caused_by_act};
  }
  void log(std::string_view, std::string_view, const tokmon::LensId&) override {}

  std::uint64_t next_sequence{1};
  std::vector<tokmon::PhotonDraft> drafts;
};

tokmon::Result<tokmon::RefractionResult> refract(
    const std::shared_ptr<tokmon::ILens>& lens, std::string kind, std::string schema,
    tokmon::cbor::Value parameters, RecordingHost& host,
    const tokmon::PhotonWindow& window = {}) {
  tokmon::Act act{.id = "act-scenario", .ray = "ray-scenario", .kind = std::move(kind),
      .schema = std::move(schema), .parameters = std::move(parameters),
      .target = lens->manifest().id, .epoch = 7, .generation = 7001,
      .approved = true, .idempotency_key = "scenario-idempotency",
      .timeout = std::chrono::seconds(5)};
  std::stop_source stop;
  tokmon::RefractionBeam beam(host, act, stop.get_token(),
                              std::chrono::steady_clock::now() + act.timeout);
  return lens->refract(window, act, beam);
}

}  // namespace

TEST_CASE("official LightPath contains nineteen unique business Lenses") {
  const auto order = tokmon::official_lens_order();
  REQUIRE(order.size() == 19);
  REQUIRE(std::set<std::string>(order.begin(), order.end()).size() == order.size());
  REQUIRE(std::find(order.begin(), order.end(), "calculator") == order.end());
  for (const auto& id : order) REQUIRE(tokmon::make_builtin_lens(id));
}

TEST_CASE("all twenty built Lens libraries expose their independent C ABI identity") {
  auto ids = tokmon::official_lens_order();
  ids.push_back("calculator");
#if defined(_WIN32)
  constexpr auto extension = ".dll";
#elif defined(__APPLE__)
  constexpr auto extension = ".dylib";
#else
  constexpr auto extension = ".so";
#endif
  for (const auto& id : ids) {
    DYNAMIC_SECTION(id) {
      const auto path = std::filesystem::path(TOKMON_TEST_LENS_DIR) /
          ("tokmon-lens-" + id + extension);
      auto loaded = tokmon::CAbiLens::load(path);
      REQUIRE(loaded);
      REQUIRE((*loaded)->manifest().id == "org.tokmon.lens." + id);
      tokmon::SurfaceBuilder surface((*loaded)->manifest().id);
      REQUIRE((*loaded)->view(tokmon::PhotonWindow{}, surface));
    }
  }
}

TEST_CASE("every built-in Lens executes a declared refraction scenario") {
  const auto root = lens_temporary_directory("scenarios");
  struct Scenario {
    std::string id;
    std::string kind;
    std::string schema;
    tokmon::cbor::Value parameters;
    std::string emitted_kind;
  };
  const std::vector<Scenario> scenarios = {
      {"ignis", "lens.verify", "tokmon.lens.verify.v1", tokmon::cbor::object({}),
       "lens.verification-requested"},
      {"lemon", "waveguide.send-frame", "tokmon.waveguide.frame.v1",
       tokmon::cbor::object({{"frame_kind", "delta"}, {"payload_bytes", 32}}),
       "waveguide.frame-sent"},
      {"iris", "external.disconnect", "tokmon.external.disconnect.v1",
       tokmon::cbor::object({{"connection_ref", "connection:test"}}),
       "external.connection-closed"},
      {"rhea", "model.call", "tokmon.model.call.v1",
       tokmon::cbor::object({{"model", "local-deterministic"}, {"prompt", "hello"}}),
       "model.requested"},
      {"janus", "ray.cancel", "tokmon.ray.cancel.v1", tokmon::cbor::object({}),
       "ray.cancel-requested"},
      {"clotho", "workflow.cancel", "tokmon.workflow.cancel.v1", tokmon::cbor::object({}),
       "workflow.cancelled"},
      {"aya", "child.cancel", "tokmon.child.cancel.v1",
       tokmon::cbor::object({{"child_ray", "ray-child"}}), "child.cancelled"},
      {"textus", "text.compact", "tokmon.text.compact.v1", tokmon::cbor::object({}),
       "summary.created"},
      {"enso", "rag.reindex", "tokmon.rag.reindex.v1", tokmon::cbor::object({}),
       "rag.index-rebuilt"},
      {"techor", "tool.decode", "tokmon.tool.decode.v1", tokmon::cbor::object({}),
       "tool.decoded"},
      {"styx", "process.cancel", "tokmon.process.cancel.v1",
       tokmon::cbor::object({{"process_ref", "process:test"}}),
       "process.cancel-requested"},
      {"fallen", "policy.evaluate", "tokmon.policy.evaluate.v1",
       tokmon::cbor::object({{"act_hash", std::string(64, 'a')}, {"decision", "allow"}}),
       "policy.evaluated"},
      {"cista", "redaction.apply", "tokmon.redaction.apply.v1",
       tokmon::cbor::object({{"content", "token=secret-value"}}), "redaction.applied"},
      {"chora", "archive.seal", "tokmon.archive.seal.v1",
       tokmon::cbor::object({{"storage_root", root.generic_string()}}), "archive.sealed"},
      {"tracket", "integrity.verify", "tokmon.integrity.verify.v1", tokmon::cbor::object({}),
       "integrity.verified"},
      {"nota", "diagnostic.bundle", "tokmon.diagnostic.bundle.v1", tokmon::cbor::object({}),
       "diagnostic.bundle-created"},
      {"cove", "artifact.create", "tokmon.artifact.create.v1",
       tokmon::cbor::object({{"workspace_root", root.generic_string()}, {"content", "artifact"}}),
       "artifact.created"},
      {"snow", "snow.cancel", "tokmon.snow.cancel.v1", tokmon::cbor::object({}),
       "snow.cancel-observed"},
      {"termon", "ui.intent", "tokmon.ui.intent.v1", tokmon::cbor::object({}),
       "ui.intent-forwarded"},
      {"calculator", "tool.calculate", "tokmon.math.calculate.v1",
       tokmon::cbor::object({{"expression", "7 * 6"}}), "tool.result"},
  };
  for (const auto& scenario : scenarios) {
    DYNAMIC_SECTION(scenario.id) {
      const auto lens = tokmon::make_builtin_lens(scenario.id);
      REQUIRE(lens);
      RecordingHost host;
      auto result = refract(lens, scenario.kind, scenario.schema,
                            scenario.parameters, host);
      REQUIRE(result);
      REQUIRE(result->status == tokmon::RefractionStatus::completed);
      REQUIRE_FALSE(host.drafts.empty());
      REQUIRE(host.drafts.front().kind == scenario.emitted_kind);
      for (const auto& draft : host.drafts) {
        REQUIRE(draft.ray == "ray-scenario");
        REQUIRE(draft.caused_by_act == "act-scenario");
      }
    }
  }
}

TEST_CASE("Styx executes argv without a shell and captures bounded output") {
  const auto root = lens_temporary_directory("styx");
  const auto lens = tokmon::make_builtin_lens("styx");
  RecordingHost host;
  tokmon::cbor::Value::Array argv{TOKMON_CMAKE_COMMAND, "-E", "echo", "styx-ok"};
  auto result = refract(lens, "process.exec", "tokmon.process.exec.v1",
      tokmon::cbor::object({{"argv", std::move(argv)}, {"cwd", root.generic_string()},
                            {"max_output_bytes", 4096},
                            {"require_strength", "process-tree"}}), host);
  REQUIRE(result);
  const auto stdout_photon = std::find_if(host.drafts.begin(), host.drafts.end(),
      [](const auto& draft) { return draft.kind == "process.stdout"; });
  REQUIRE(stdout_photon != host.drafts.end());
  REQUIRE(tokmon::cbor::find(stdout_photon->payload, "text")->as_string().find("styx-ok") !=
          std::string_view::npos);
  REQUIRE(host.drafts.back().kind == "process.exited");
  REQUIRE(tokmon::cbor::find(host.drafts.back().payload, "exit_code")->as_integer() == 0);
}

TEST_CASE("Cove performs guarded write read move and delete") {
  const auto root = lens_temporary_directory("cove");
  const auto lens = tokmon::make_builtin_lens("cove");
  RecordingHost host;
  auto write = refract(lens, "fs.write", "tokmon.fs.write.v1",
      tokmon::cbor::object({{"workspace_root", root.generic_string()},
                            {"path", "a/file.txt"}, {"content", "lens-data"},
                            {"precondition_sha256", ""}}), host);
  REQUIRE(write);
  REQUIRE(std::filesystem::exists(root / "a" / "file.txt"));
  auto read = refract(lens, "fs.read", "tokmon.fs.read.v1",
      tokmon::cbor::object({{"workspace_root", root.generic_string()},
                            {"path", "a/file.txt"}}), host);
  REQUIRE(read);
  REQUIRE(tokmon::cbor::find(host.drafts.back().payload, "content")->as_string() == "lens-data");
  auto move = refract(lens, "fs.move", "tokmon.fs.move.v1",
      tokmon::cbor::object({{"workspace_root", root.generic_string()},
                            {"path", "a/file.txt"}, {"destination", "b/file.txt"}}), host);
  REQUIRE(move);
  REQUIRE(std::filesystem::exists(root / "b" / "file.txt"));
  auto remove = refract(lens, "fs.delete", "tokmon.fs.delete.v1",
      tokmon::cbor::object({{"workspace_root", root.generic_string()},
                            {"path", "b/file.txt"}}), host);
  REQUIRE(remove);
  REQUIRE_FALSE(std::filesystem::exists(root / "b" / "file.txt"));
}

TEST_CASE("Chora stores immutable content addressed blobs") {
  const auto root = lens_temporary_directory("chora");
  const auto lens = tokmon::make_builtin_lens("chora");
  RecordingHost host;
  auto stored = refract(lens, "blob.put", "tokmon.blob.put.v1",
      tokmon::cbor::object({{"storage_root", root.generic_string()},
                            {"content", "immutable-data"}}), host);
  REQUIRE(stored);
  const auto* path = tokmon::cbor::find(host.drafts.back().payload, "path");
  REQUIRE(path);
  REQUIRE(std::filesystem::exists(std::filesystem::path(path->as_string())));
  REQUIRE(tokmon::cbor::find(host.drafts.back().payload, "immutable")->as_bool());
}

TEST_CASE("Clotho validates a DAG and proposes the first ready node deterministically") {
  const auto lens = tokmon::make_builtin_lens("clotho");
  tokmon::cbor::Value::Array nodes{
      tokmon::cbor::object({{"id", "build"},
                            {"depends_on", tokmon::cbor::Value::Array{"prepare"}}}),
      tokmon::cbor::object({{"id", "prepare"},
                            {"depends_on", tokmon::cbor::Value::Array{}}})};
  tokmon::Photon definition{.sequence = 1, .id = "photon-definition", .ray = "ray-workflow",
      .kind = "workflow.defined", .schema = "tokmon.workflow.definition.v1",
      .payload = tokmon::cbor::object({{"nodes", std::move(nodes)}}), .epoch = 3,
      .hash = std::string(64, 'b')};
  tokmon::SurfaceBuilder surface(lens->manifest().id);
  REQUIRE(lens->view(tokmon::PhotonWindow({definition}), surface));
  REQUIRE(surface.proposals().size() == 1);
  REQUIRE(tokmon::cbor::find(surface.proposals().front().parameters, "node_id")->as_string() ==
          "prepare");
}

TEST_CASE("redaction removes assignment bearer and URL secrets") {
  const auto redacted = tokmon::redact(
      "Authorization: Bearer abc.def token=plain https://x.test/?api_key=url-secret");
  REQUIRE(redacted.find("abc.def") == std::string::npos);
  REQUIRE(redacted.find("plain") == std::string::npos);
  REQUIRE(redacted.find("url-secret") == std::string::npos);
}

#if defined(TOKMON_NODE_EXECUTABLE)
TEST_CASE("Node.js Lens runs through WorkerLensProxy and emits through host Beam") {
  tokmon::LensManifest manifest{.id = "org.tokmon.lens.adder-node",
      .display_name = "Node adder", .runtime = tokmon::RuntimeKind::node,
      .runtime_version = TOKMON_NODE_VERSION,
      .observes = {{"user.input", "*"}}, .view_channels = {"model.tools"},
      .refracts = {{"tool.add", "tokmon.math.add.v1"}},
      .light_permissions = {"photon.emit", "log.write"}};
  auto lens = tokmon::WorkerLensProxy::launch(tokmon::WorkerLensOptions{
      .manifest = std::move(manifest), .supervisor = TOKMON_WORKER_EXECUTABLE,
      .runtime_executable = TOKMON_NODE_EXECUTABLE,
      .adapter = std::filesystem::path(TOKMON_SOURCE_DIR) / "sdk/typescript/worker.mjs",
      .entry = std::filesystem::path(TOKMON_SOURCE_DIR) / "sdk/typescript/examples/adder.mjs"});
  REQUIRE(lens);
  tokmon::SurfaceBuilder surface((*lens)->manifest().id);
  REQUIRE((*lens)->view(tokmon::PhotonWindow{}, surface));
  REQUIRE(surface.contributions().size() == 1);
  RecordingHost host;
  auto result = refract(*lens, "tool.add", "tokmon.math.add.v1",
      tokmon::cbor::object({{"left", 19}, {"right", 23}}), host);
  REQUIRE(result);
  REQUIRE(host.drafts.size() == 1);
  REQUIRE(tokmon::cbor::find(host.drafts.front().payload, "result")->as_integer() == 42);
  (*lens)->request_stop();
}
#endif

#if defined(TOKMON_PYTHON_EXECUTABLE)
TEST_CASE("CPython Lens runs through WorkerLensProxy and emits through host Beam") {
  tokmon::LensManifest manifest{.id = "org.tokmon.lens.adder-python",
      .display_name = "Python adder", .runtime = tokmon::RuntimeKind::cpython,
      .runtime_version = TOKMON_PYTHON_VERSION,
      .observes = {{"user.input", "*"}}, .view_channels = {"model.tools"},
      .refracts = {{"tool.add", "tokmon.math.add.v1"}},
      .light_permissions = {"photon.emit", "log.write"}};
  auto lens = tokmon::WorkerLensProxy::launch(tokmon::WorkerLensOptions{
      .manifest = std::move(manifest), .supervisor = TOKMON_WORKER_EXECUTABLE,
      .runtime_executable = TOKMON_PYTHON_EXECUTABLE,
      .adapter = std::filesystem::path(TOKMON_SOURCE_DIR) /
          "sdk/python/tokmon_lens_sdk/worker.py",
      .entry = std::filesystem::path(TOKMON_SOURCE_DIR) / "sdk/python/examples/adder.py"});
  REQUIRE(lens);
  tokmon::SurfaceBuilder surface((*lens)->manifest().id);
  REQUIRE((*lens)->view(tokmon::PhotonWindow{}, surface));
  REQUIRE(surface.contributions().size() == 1);
  RecordingHost host;
  auto result = refract(*lens, "tool.add", "tokmon.math.add.v1",
      tokmon::cbor::object({{"left", 20}, {"right", 22}}), host);
  REQUIRE(result);
  REQUIRE(host.drafts.size() == 1);
  REQUIRE(tokmon::cbor::find(host.drafts.front().payload, "result")->as_integer() == 42);
  (*lens)->request_stop();
}
#endif
