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
  for (;;) {
    auto frame = tokmon::read_frame(std::cin);
    if (!frame) return std::cin.eof() ? 0 : 2;
    tokmon::WorkerFrame response;
    response.request_id = frame->request_id;
    if (frame->type == "worker.hello") {
      response.type = "worker.ready";
      response.payload = tokmon::cbor::object({
          {"protocol_major", static_cast<std::int64_t>(tokmon::worker_protocol_major)},
          {"protocol_minor", static_cast<std::int64_t>(tokmon::worker_protocol_minor)},
          {"lens_id", lens->manifest().id}, {"runtime", std::string(runtime_name)},
          {"version", lens->manifest().version}});
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
  std::cerr << "usage: tokmon-lens-worker --runtime native --lens <id> | "
               "--runtime native_worker --entry <C-ABI-library> | "
               "--runtime node|cpython --runtime-executable <path> --adapter <path> --entry <path>\n";
  return 2;
}
