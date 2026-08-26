#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "tokmon/tokmon.hpp"
#include "apps/entrypoints.hpp"

namespace {

class CollectHost final : public tokmon::OpticalHost {
 public:
  tokmon::Result<tokmon::Photon> emit(tokmon::PhotonDraft draft) override {
    drafts.push_back(draft);
    tokmon::Photon photon;
    photon.id = tokmon::make_id("worker-photon"); photon.ray = draft.ray;
    photon.kind = draft.kind; photon.schema = draft.schema; photon.payload = draft.payload;
    photon.epoch = draft.epoch; photon.caused_by_act = draft.caused_by_act;
    return photon;
  }
  void log(std::string_view level, std::string_view message,
           const tokmon::LensId& lens) override {
    logs.push_back(tokmon::cbor::object({{"level", std::string(level)},
        {"message", std::string(message)}, {"lens", lens}}));
  }
  std::vector<tokmon::PhotonDraft> drafts;
  tokmon::cbor::Value::Array logs;
};

tokmon::cbor::Value error_payload(const tokmon::Error& error) {
  return tokmon::cbor::object({{"code", std::string(tokmon::to_string(error.code))},
                               {"message", error.message}, {"retryable", error.retryable}});
}

tokmon::cbor::Value draft_payload(const std::vector<tokmon::PhotonDraft>& drafts) {
  tokmon::cbor::Value::Array result;
  for (const auto& draft : drafts) result.push_back(tokmon::cbor::object({
      {"ray", draft.ray}, {"kind", draft.kind}, {"schema", draft.schema},
      {"payload", draft.payload}, {"epoch", static_cast<std::int64_t>(draft.epoch)},
      {"caused_by_act", draft.caused_by_act}}));
  return result;
}

int serve_lens(std::shared_ptr<tokmon::ILens> lens, const std::string_view runtime_name) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (!lens) return 2;
  std::stop_source stop;
  const auto host_optical = [](const std::uint64_t request_id,
                               tokmon::cbor::Value payload)
      -> tokmon::Result<tokmon::cbor::Value> {
    if (auto written = tokmon::write_frame(std::cout, tokmon::WorkerFrame{
        .type = "host.optical.request", .request_id = request_id,
        .payload = std::move(payload)}); !written)
      return tl::unexpected(written.error());
    auto frame = tokmon::read_frame(std::cin);
    if (!frame) return tl::unexpected(frame.error());
    if (frame->type != "host.optical.result" || frame->request_id != request_id)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                               "unexpected optical host response"));
    const auto* ok = tokmon::cbor::find(frame->payload, "ok");
    if (!ok || !ok->as_bool()) {
      const auto* encoded = tokmon::cbor::find(frame->payload, "error");
      const auto* message = encoded ? tokmon::cbor::find(*encoded, "message") : nullptr;
      const auto* code = encoded ? tokmon::cbor::find(*encoded, "code") : nullptr;
      const auto* retryable = encoded ? tokmon::cbor::find(*encoded, "retryable") : nullptr;
      return tl::unexpected(tokmon::make_error(
          code ? tokmon::error_code_from_string(code->as_string(),
                                                tokmon::ErrorCode::provider_failed) :
                 tokmon::ErrorCode::provider_failed,
          message ? std::string(message->as_string()) : "optical host operation failed",
          retryable && retryable->as_bool()));
    }
    const auto* value = tokmon::cbor::find(frame->payload, "value");
    return value ? *value : tokmon::cbor::Value{};
  };
  for (;;) {
    auto frame = tokmon::read_frame(std::cin);
    if (!frame) return std::cin.eof() ? 0 : 2;
    tokmon::WorkerFrame response;
    response.request_id = frame->request_id;
    if (frame->type == "worker.hello") {
      const auto* extension = dynamic_cast<const tokmon::IOpticalLensExtension*>(lens.get());
      response.type = "worker.ready";
      response.payload = tokmon::cbor::object({
          {"protocol_major", static_cast<std::int64_t>(tokmon::worker_protocol_major)},
          {"protocol_minor", static_cast<std::int64_t>(tokmon::worker_protocol_minor)},
          {"lens_id", lens->manifest().id}, {"runtime", std::string(runtime_name)},
          {"version", lens->manifest().version},
          {"features", tokmon::cbor::object({
              {"derive", extension && extension->supports_derive()},
              {"coordinate", extension && extension->supports_coordinate()},
              {"query", extension && extension->supports_query()}})}});
    } else if (frame->type == "lens.view.request") {
      const auto* value = tokmon::cbor::find(frame->payload, "window");
      auto window = value ? tokmon::photon_window_from_cbor(*value)
                          : tokmon::Result<tokmon::PhotonWindow>(tl::unexpected(
                                tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                   "view request has no window")));
      if (!window) {
        response.type = "lens.view.result";
        response.payload = tokmon::cbor::object({{"ok", false},
                                                 {"error", error_payload(window.error())}});
      } else {
        tokmon::SurfaceBuilder builder(lens->manifest().id);
        auto result = lens->view(*window, builder);
        if (!result) response.payload = tokmon::cbor::object(
            {{"ok", false}, {"error", error_payload(result.error())}});
        else response.payload = tokmon::cbor::object({{"ok", true},
            {"surface", tokmon::to_cbor(tokmon::SurfaceSnapshot{
                .contributions = builder.contributions(), .proposals = builder.proposals()})}});
        response.type = "lens.view.result";
      }
    } else if (frame->type == "lens.derive.request") {
      response.type = "lens.derive.result";
      const auto* value = tokmon::cbor::find(frame->payload, "window");
      auto window = value ? tokmon::photon_window_from_cbor(*value)
                          : tokmon::Result<tokmon::PhotonWindow>(tl::unexpected(
                              tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                 "derive request has no window")));
      auto* extension = dynamic_cast<tokmon::IOpticalLensExtension*>(lens.get());
      if (!window || !extension || !extension->supports_derive()) {
        const auto failure = !window ? window.error() : tokmon::make_error(
            tokmon::ErrorCode::unsupported, "worker Lens has no derive extension");
        response.payload = tokmon::cbor::object({{"ok", false},
                                                 {"error", error_payload(failure)}});
      } else {
        auto result = extension->derive(*window);
        response.payload = result ? tokmon::cbor::object({{"ok", true}, {"state", *result}}) :
            tokmon::cbor::object({{"ok", false}, {"error", error_payload(result.error())}});
      }
    } else if (frame->type == "lens.coordinate.request") {
      response.type = "lens.coordinate.result";
      const auto* value = tokmon::cbor::find(frame->payload, "window");
      auto window = value ? tokmon::photon_window_from_cbor(*value)
                          : tokmon::Result<tokmon::PhotonWindow>(tl::unexpected(
                              tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                 "coordinate request has no window")));
      auto* extension = dynamic_cast<tokmon::IOpticalLensExtension*>(lens.get());
      if (!window || !extension || !extension->supports_coordinate()) {
        const auto failure = !window ? window.error() : tokmon::make_error(
            tokmon::ErrorCode::unsupported, "worker Lens has no coordinate extension");
        response.payload = tokmon::cbor::object({{"ok", false},
                                                 {"error", error_payload(failure)}});
      } else {
        auto optical = tokmon::OpticalContext::from_callbacks(
            [&](const std::string_view channel, const std::string_view key) {
              return host_optical(frame->request_id, tokmon::cbor::object({
                  {"operation", "get"}, {"channel", std::string(channel)},
                  {"key", std::string(key)}}));
            },
            [&](const std::string_view channel)
                -> tokmon::Result<std::vector<tokmon::cbor::Value>> {
              auto result = host_optical(frame->request_id, tokmon::cbor::object({
                  {"operation", "get_all"}, {"channel", std::string(channel)}}));
              if (!result) return tl::unexpected(result.error());
              if (!result->as_array())
                return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::protocol_error,
                    "get_all host result must be an array"));
              return *result->as_array();
            },
            [&](tokmon::OpticalQueryRequest request) {
              return host_optical(frame->request_id, tokmon::cbor::object({
                  {"operation", "query"}, {"capability", request.capability},
                  {"parameters", request.parameters},
                  {"request_schema", request.request_schema},
                  {"response_schema", request.response_schema},
                  {"timeout_ms", static_cast<std::int64_t>(request.timeout.count())},
                  {"max_response_bytes", static_cast<std::int64_t>(request.max_response_bytes)}}));
            });
        tokmon::SurfaceBuilder surface(lens->manifest().id);
        auto result = extension->coordinate(*window, optical, surface);
        response.payload = result ? tokmon::cbor::object({{"ok", true},
            {"surface", tokmon::to_cbor(tokmon::SurfaceSnapshot{
                .contributions = surface.contributions(), .proposals = surface.proposals()})}}) :
            tokmon::cbor::object({{"ok", false}, {"error", error_payload(result.error())}});
      }
    } else if (frame->type == "lens.query.request") {
      response.type = "lens.query.result";
      auto* extension = dynamic_cast<tokmon::IOpticalLensExtension*>(lens.get());
      const auto* encoded_state = tokmon::cbor::find(frame->payload, "state");
      const auto* capability = tokmon::cbor::find(frame->payload, "capability");
      const auto* parameters = tokmon::cbor::find(frame->payload, "parameters");
      const auto* encoded_budget = tokmon::cbor::find(frame->payload, "budget");
      if (!extension || !extension->supports_query() || !encoded_state || !capability ||
          !parameters || !encoded_budget) {
        response.payload = tokmon::cbor::object({{"ok", false}, {"error", error_payload(
            tokmon::make_error(tokmon::ErrorCode::unsupported,
                               "worker query request or extension is incomplete"))}});
      } else {
        tokmon::FrozenLensState state;
        if (const auto* part = tokmon::cbor::find(*encoded_state, "lens")) state.lens = part->as_string();
        if (const auto* part = tokmon::cbor::find(*encoded_state, "artifact_hash")) state.artifact_hash = part->as_string();
        if (const auto* part = tokmon::cbor::find(*encoded_state, "epoch")) state.epoch = static_cast<tokmon::MountEpoch>(part->as_integer());
        if (const auto* part = tokmon::cbor::find(*encoded_state, "generation")) state.generation = static_cast<tokmon::GenerationId>(part->as_integer());
        if (const auto* part = tokmon::cbor::find(*encoded_state, "path_index")) state.path_index = static_cast<std::size_t>(part->as_integer());
        state.value = std::make_shared<const tokmon::cbor::Value>(
            tokmon::cbor::find(*encoded_state, "value") ?
                *tokmon::cbor::find(*encoded_state, "value") : tokmon::cbor::Value{});
        const auto timeout = tokmon::cbor::find(*encoded_budget, "timeout_ms") ?
            tokmon::cbor::find(*encoded_budget, "timeout_ms")->as_integer(1) : 1;
        tokmon::QueryBudget budget{
            .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout),
            .max_request_bytes = static_cast<std::size_t>(tokmon::cbor::find(*encoded_budget, "max_request_bytes") ? tokmon::cbor::find(*encoded_budget, "max_request_bytes")->as_integer() : 0),
            .max_response_bytes = static_cast<std::size_t>(tokmon::cbor::find(*encoded_budget, "max_response_bytes") ? tokmon::cbor::find(*encoded_budget, "max_response_bytes")->as_integer() : 0),
            .call_index = static_cast<std::size_t>(tokmon::cbor::find(*encoded_budget, "call_index") ? tokmon::cbor::find(*encoded_budget, "call_index")->as_integer() : 0)};
        auto result = extension->optical_query(state, capability->as_string(), *parameters, budget);
        response.payload = result ? tokmon::cbor::object({{"ok", true}, {"value", *result}}) :
            tokmon::cbor::object({{"ok", false}, {"error", error_payload(result.error())}});
      }
    } else if (frame->type == "lens.refract.request") {
      const auto* window_value = tokmon::cbor::find(frame->payload, "window");
      const auto* act_value = tokmon::cbor::find(frame->payload, "act");
      auto window = window_value ? tokmon::photon_window_from_cbor(*window_value)
                                 : tokmon::Result<tokmon::PhotonWindow>(tl::unexpected(
                                      tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                         "refract request has no window")));
      auto act = act_value ? tokmon::act_from_cbor(*act_value)
                           : tokmon::Result<tokmon::Act>(tl::unexpected(
                                 tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                    "refract request has no act")));
      response.type = "lens.refract.result";
      if (!window || !act) {
        const auto error = !window ? window.error() : act.error();
        response.payload = tokmon::cbor::object({{"ok", false},
                                                 {"error", error_payload(error)}});
      } else {
        CollectHost host;
        tokmon::RefractionBeam beam(host, *act, stop.get_token(),
                                    std::chrono::steady_clock::now() + act->timeout);
        auto result = lens->refract(*window, *act, beam);
        if (!result) response.payload = tokmon::cbor::object(
            {{"ok", false}, {"error", error_payload(result.error())}});
        else response.payload = tokmon::cbor::object({{"ok", true},
            {"status", std::string(tokmon::to_string(result->status))},
            {"detail", result->detail}, {"drafts", draft_payload(host.drafts)},
            {"logs", std::move(host.logs)}});
      }
    } else if (frame->type == "beam.cancel") {
      stop.request_stop(); response.type = "beam.cancelled";
      response.payload = tokmon::cbor::object({{"ok", true}});
    } else if (frame->type == "worker.shutdown") {
      lens->request_stop(); response.type = "worker.stopped";
      response.payload = tokmon::cbor::object({{"ok", true}});
      if (auto written = tokmon::write_frame(std::cout, response); !written) return 2;
      return 0;
    } else {
      response.type = "worker.error";
      response.payload = tokmon::cbor::object({{"ok", false}, {"error",
          error_payload(tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                           "unknown worker frame type"))}});
    }
    if (auto written = tokmon::write_frame(std::cout, response); !written) return 2;
  }
}

int launch_language(const std::string& runtime, const std::filesystem::path& executable,
                    const std::filesystem::path& adapter,
                    const std::filesystem::path& entry) {
#if defined(_WIN32)
  std::wstring command = L"\"" + executable.wstring() + L"\" ";
  if (runtime == "cpython") command += L"-I ";
  command += L"\"" + adapter.wstring() + L"\" --entry \"" + entry.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  auto job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    if (job) CloseHandle(job); return 2;
  }
  if (job) AssignProcessToJobObject(job, process.hProcess);
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 2; GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess); if (job) CloseHandle(job);
  return static_cast<int>(code);
#else
  const auto child = fork();
  if (child == 0) {
    const auto executable_text = executable.string(); const auto adapter_text = adapter.string();
    const auto entry_text = entry.string();
    if (runtime == "cpython")
      execl(executable_text.c_str(), executable_text.c_str(), "-I", adapter_text.c_str(),
            "--entry", entry_text.c_str(), static_cast<char*>(nullptr));
    else
      execl(executable_text.c_str(), executable_text.c_str(), adapter_text.c_str(),
            "--entry", entry_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  if (child < 0) return 2;
  int status = 0; waitpid(child, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
#endif
}

}  // namespace

int tokmon::app::lens_worker_main(int argc, char** argv) {
  std::string runtime = "native"; std::string lens = "rhea";
  std::filesystem::path executable; std::filesystem::path adapter; std::filesystem::path entry;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--runtime" && index + 1 < argc) runtime = argv[++index];
    else if (argument == "--lens" && index + 1 < argc) lens = argv[++index];
    else if (argument == "--runtime-executable" && index + 1 < argc) executable = argv[++index];
    else if (argument == "--adapter" && index + 1 < argc) adapter = argv[++index];
    else if (argument == "--entry" && index + 1 < argc) entry = argv[++index];
  }
  if (runtime == "native") return serve_lens(tokmon::make_builtin_lens(lens), "native");
  if (runtime == "native_worker" && !entry.empty()) {
    auto loaded = tokmon::CAbiLens::load(entry);
    if (!loaded) { std::cerr << loaded.error().describe() << '\n'; return 2; }
    return serve_lens(std::static_pointer_cast<tokmon::ILens>(*loaded), "native_worker");
  }
  if ((runtime == "node" || runtime == "cpython") && !executable.empty() &&
      !adapter.empty() && !entry.empty())
    return launch_language(runtime, executable, adapter, entry);
  std::cerr << "invalid internal worker arguments\n";
  return 2;
}
