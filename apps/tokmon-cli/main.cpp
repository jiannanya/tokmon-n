#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#include "tokmon/tokmon.hpp"

namespace {

enum class OutputFormat { human, jsonl, cbor };

void print_error(const tokmon::Error& error) { std::cerr << error.describe() << '\n'; }

void write_value(std::ostream& output, const tokmon::cbor::Value& value,
                 const OutputFormat format) {
  if (format == OutputFormat::jsonl) {
    output << tokmon::json::stringify(value) << '\n';
  } else if (format == OutputFormat::cbor) {
    const auto encoded = tokmon::cbor::encode(value);
    const std::uint32_t size = static_cast<std::uint32_t>(encoded.size());
    const std::array<char, 4> header{
        static_cast<char>(size >> 24u), static_cast<char>(size >> 16u),
        static_cast<char>(size >> 8u), static_cast<char>(size)};
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
  } else {
    output << tokmon::cbor::diagnostic(value) << '\n';
  }
  output.flush();
}

void print_photon(std::ostream& output, const tokmon::Photon& photon,
                  const OutputFormat format) {
  if (format == OutputFormat::human)
    output << photon.sequence << "  " << photon.kind << "  "
           << tokmon::cbor::diagnostic(photon.payload) << '\n';
  else write_value(output, tokmon::to_cbor(photon), format);
}

std::string join(const std::vector<std::string>& arguments, const std::size_t start) {
  std::string result;
  for (std::size_t index = start; index < arguments.size(); ++index) {
    if (!result.empty()) result.push_back(' ');
    result.append(arguments[index]);
  }
  return result;
}

void help() {
  std::cout << R"HELP(Tokmon — A Lens to Them All

Usage:
  tokmon run <message>           Submit one headless causal ray through tokmond
  tokmon chat [message]          Interactive conversation
  tokmon history [ray-id]        Print committed photons
  tokmon stdio                   Concurrent JSON Lines stdio server
  tokmon doctor                  Verify storage and active light path
  tokmon lens list               List mounted lens generations
  tokmon lens verify             Verify storage and every mounted generation
  tokmon lens mount <id> <artifact> [runtime]
  tokmon lens replace <id> <artifact> [runtime]
  tokmon lens unmount <id>
  tokmon lens reconcile          Re-read .tokmon YAML and atomically swap
  tokmon daemon status           Check this workspace background service
  tokmon daemon start            Start it now (normally automatic)
  tokmon daemon stop             Gracefully stop it after active work drains
  tokmon config paths            Print resolved .tokmon directories

Options:
  --workspace <path>             Select project-level .tokmon directory
  --format human|jsonl|cbor      Stable human or machine output
  --output <file>                Write command output to a file
  --deadline-ms <milliseconds>   Attach a request deadline
  --no-color                     Disable decoration (accepted for CI)

All stateful commands attach to or automatically start the workspace tokmond.
The shared daemon remains alive when a CLI or desktop client exits.
)HELP";
}

std::filesystem::path sibling_daemon(const char* argv0) {
  std::error_code error;
#if defined(_WIN32)
  std::wstring module(32'768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, module.data(),
                                      static_cast<DWORD>(module.size()));
  if (size > 0 && size < module.size()) {
    module.resize(size);
    return std::filesystem::path(module).parent_path() / "tokmond.exe";
  }
#endif
  auto executable = std::filesystem::absolute(argv0, error);
  if (error) executable = std::filesystem::current_path() / argv0;
#if defined(_WIN32)
  return executable.parent_path() / "tokmond.exe";
#else
  return executable.parent_path() / "tokmond";
#endif
}

int run_stdio(const tokmon::SnowClient& client) {
  std::mutex output_mutex;
  std::vector<std::jthread> requests;
  const auto forward = [&client, &output_mutex](tokmon::SnowMessage request) {
    auto response = client.request_stream(request, [&](const tokmon::SnowMessage& event) {
      std::scoped_lock lock(output_mutex);
      std::cout << tokmon::json::stringify(tokmon::to_cbor(event)) << '\n';
      std::cout.flush();
      return tokmon::Result<void>{};
    });
    std::scoped_lock lock(output_mutex);
    if (response) std::cout << tokmon::json::stringify(tokmon::to_cbor(*response)) << '\n';
    else {
      const tokmon::SnowMessage error{.kind = tokmon::SnowMessageKind::error,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"code", std::string(tokmon::to_string(response.error().code))},
              {"message", response.error().describe()}})};
      std::cout << tokmon::json::stringify(tokmon::to_cbor(error)) << '\n';
    }
    std::cout.flush();
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    auto parsed = tokmon::json::parse(line);
    if (!parsed) {
      std::scoped_lock lock(output_mutex);
      const tokmon::SnowMessage error{.kind = tokmon::SnowMessageKind::error,
          .payload = tokmon::cbor::object({{"code", "protocol_error"},
                                           {"message", parsed.error().describe()}})};
      std::cout << tokmon::json::stringify(tokmon::to_cbor(error)) << '\n';
      continue;
    }
    auto request = tokmon::snow_message_from_cbor(*parsed);
    if (!request) {
      std::scoped_lock lock(output_mutex);
      const tokmon::SnowMessage error{.kind = tokmon::SnowMessageKind::error,
          .payload = tokmon::cbor::object({{"code", "protocol_error"},
                                           {"message", request.error().describe()}})};
      std::cout << tokmon::json::stringify(tokmon::to_cbor(error)) << '\n';
      continue;
    }
    const bool closing = request->kind == tokmon::SnowMessageKind::close;
    if (closing) {
      for (auto& pending : requests) if (pending.joinable()) pending.join();
      requests.clear();
      forward(std::move(*request));
      break;
    }
    requests.emplace_back([&forward, request = std::move(*request)]() mutable {
      forward(std::move(request));
    });
  }
  for (auto& request : requests) if (request.joinable()) request.join();
  return 0;
}

tokmon::Result<tokmon::SnowMessage> intent(const tokmon::SnowClient& client,
                                            tokmon::cbor::Value payload,
                                            const std::uint64_t cursor = 0) {
  tokmon::SnowMessage message{.kind = tokmon::SnowMessageKind::intent,
      .request_id = tokmon::next_snow_request_id(), .cursor = cursor,
      .payload = std::move(payload)};
  auto response = client.request(message);
  if (!response) return tl::unexpected(response.error());
  if (response->kind == tokmon::SnowMessageKind::error) {
    const auto* field = tokmon::cbor::find(response->payload, "message");
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::protocol_error,
        field ? std::string(field->as_string()) : "tokmond rejected the intent"));
  }
  return response;
}

tokmon::Result<std::vector<tokmon::Photon>> response_photons(
    const tokmon::SnowMessage& response) {
  const auto* field = tokmon::cbor::find(response.payload, "photons");
  if (field == nullptr || field->as_array() == nullptr)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                             "Snow response has no photon array"));
  std::vector<tokmon::Photon> photons;
  for (const auto& value : *field->as_array()) {
    auto photon = tokmon::photon_from_cbor(value);
    if (!photon) return tl::unexpected(photon.error());
    photons.push_back(std::move(*photon));
  }
  return photons;
}

void print_lenses(const tokmon::SnowMessage& response) {
  const auto* field = tokmon::cbor::find(response.payload, "lenses");
  if (field == nullptr || field->as_array() == nullptr) return;
  for (const auto& lens : *field->as_array()) {
    const auto value = [&lens](const char* key) -> std::string {
      const auto* item = tokmon::cbor::find(lens, key);
      return item ? std::string(item->as_string()) : std::string{};
    };
    const auto* generation = tokmon::cbor::find(lens, "generation");
    std::cout << value("id") << '\t'
              << (generation ? generation->as_integer() : 0) << '\t'
              << value("runtime") << '\t' << value("artifact_hash") << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)argc;
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  std::vector<std::string> arguments;
#if defined(_WIN32)
  int wide_count = 0;
  auto** wide_arguments = CommandLineToArgvW(GetCommandLineW(), &wide_count);
  if (!wide_arguments) return 2;
  for (int index = 1; index < wide_count; ++index) {
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1,
                                           nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) { LocalFree(wide_arguments); return 2; }
    std::string value(static_cast<std::size_t>(bytes), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1,
                              value.data(), bytes, nullptr, nullptr);
    value.resize(static_cast<std::size_t>(bytes - 1));
    arguments.push_back(std::move(value));
  }
  LocalFree(wide_arguments);
#else
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
#endif
  std::optional<std::filesystem::path> workspace;
  std::optional<std::filesystem::path> output_path;
  OutputFormat output_format = OutputFormat::human;
  std::int64_t deadline_ms = 0;
  for (auto iterator = arguments.begin(); iterator != arguments.end();) {
    if (*iterator == "--workspace" && std::next(iterator) != arguments.end()) {
      workspace = *std::next(iterator);
      iterator = arguments.erase(iterator, std::next(iterator, 2));
    } else if (*iterator == "--output" && std::next(iterator) != arguments.end()) {
      output_path = *std::next(iterator);
      iterator = arguments.erase(iterator, std::next(iterator, 2));
    } else if (*iterator == "--format" && std::next(iterator) != arguments.end()) {
      const auto format = *std::next(iterator);
      if (format == "human") output_format = OutputFormat::human;
      else if (format == "jsonl") output_format = OutputFormat::jsonl;
      else if (format == "cbor") output_format = OutputFormat::cbor;
      else { std::cerr << "unknown output format\n"; return 2; }
      iterator = arguments.erase(iterator, std::next(iterator, 2));
    } else if (*iterator == "--deadline-ms" && std::next(iterator) != arguments.end()) {
      try { deadline_ms = std::stoll(*std::next(iterator)); }
      catch (...) { std::cerr << "invalid deadline\n"; return 2; }
      if (deadline_ms <= 0) { std::cerr << "deadline must be positive\n"; return 2; }
      iterator = arguments.erase(iterator, std::next(iterator, 2));
    } else if (*iterator == "--no-color") {
      iterator = arguments.erase(iterator);
    } else ++iterator;
  }
  if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "help") {
    help(); return 0;
  }

  auto paths = tokmon::resolve_paths(workspace);
  if (!paths) { print_error(paths.error()); return 2; }
  std::unique_ptr<std::ofstream> output_file;
  std::ostream* output = &std::cout;
  if (output_path) {
    output_file = std::make_unique<std::ofstream>(*output_path,
        std::ios::binary | std::ios::trunc);
    if (!*output_file) { std::cerr << "cannot open output file\n"; return 2; }
    output = output_file.get();
  }
  if (arguments[0] == "config" && arguments.size() > 1 && arguments[1] == "paths") {
    const auto value = tokmon::cbor::object({{"user", paths->user.string()},
        {"project", paths->project.string()}, {"database", paths->database.string()},
        {"runtimes", paths->runtimes.string()}});
    if (output_format == OutputFormat::human)
      *output << "user=" << paths->user.string() << "\nproject=" << paths->project.string()
              << "\ndatabase=" << paths->database.string() << "\nruntimes="
              << paths->runtimes.string() << '\n';
    else write_value(*output, value, output_format);
    return 0;
  }
  const auto endpoint = tokmon::workspace_snow_endpoint(
      paths->run, paths->project.parent_path());
  const auto daemon_command = arguments[0] == "daemon";
  if (daemon_command && arguments.size() > 1 && arguments[1] == "status") {
    auto available = tokmon::daemon_available(endpoint);
    if (!available) { print_error(available.error()); return 1; }
    if (output_format == OutputFormat::human)
      *output << (*available ? "running" : "stopped") << "\nendpoint="
              << endpoint.string() << '\n';
    else write_value(*output, tokmon::cbor::object({{"running", *available},
        {"endpoint", endpoint.string()},
        {"workspace", paths->project.parent_path().generic_string()}}), output_format);
    return *available ? 0 : 1;
  }
  if (daemon_command && arguments.size() > 1 && arguments[1] == "stop") {
    auto available = tokmon::daemon_available(endpoint);
    if (!available) { print_error(available.error()); return 1; }
    if (!*available) {
      if (output_format == OutputFormat::human) *output << "tokmond is already stopped\n";
      return 0;
    }
    auto stopped = tokmon::shutdown_daemon(endpoint);
    if (!stopped) { print_error(stopped.error()); return 1; }
    if (output_format == OutputFormat::human) *output << "tokmond stopped gracefully\n";
    else write_value(*output, tokmon::cbor::object({{"stopped", true}}), output_format);
    return 0;
  }

  auto connection = tokmon::ensure_daemon(tokmon::DaemonLaunchOptions{
      .endpoint = endpoint,
      .workspace = paths->project.parent_path(),
      .executable = sibling_daemon(argv[0])});
  if (!connection) { print_error(connection.error()); return 1; }
  if (daemon_command && arguments.size() > 1 && arguments[1] == "start") {
    if (output_format == OutputFormat::human)
      *output << (connection->started ? "tokmond started" : "tokmond already running")
              << "\nendpoint=" << endpoint.string() << '\n';
    else write_value(*output, tokmon::cbor::object({{"running", true},
        {"started", connection->started}, {"endpoint", endpoint.string()}}), output_format);
    return 0;
  }
  if (daemon_command) { help(); return 2; }

  tokmon::SnowClient client(endpoint);

  if (arguments[0] == "stdio") return run_stdio(client);
  if (arguments[0] == "run") {
    const auto message = join(arguments, 1);
    std::string input = message;
    if (input.empty()) {
      std::ostringstream buffer; buffer << std::cin.rdbuf(); input = buffer.str();
    }
    if (input.empty()) { std::cerr << "run requires a message or stdin\n"; return 2; }
    auto response = intent(client, tokmon::cbor::object({{"action", "chat"},
        {"text", input}, {"deadline_ms", deadline_ms}}));
    if (!response) { print_error(response.error()); return 1; }
    auto photons = response_photons(*response);
    if (!photons) { print_error(photons.error()); return 1; }
    for (const auto& photon : *photons) print_photon(*output, photon, output_format);
    return 0;
  }
  if (arguments[0] == "chat") {
    if (output_format != OutputFormat::human) {
      std::cerr << "chat requires human output; use run for machine output\n"; return 2;
    }
    *output << "Tokmon interactive mode. Type /new for a new ray or /quit to leave.\n";
    std::string line;
    tokmon::RayId active_ray;
    if (arguments.size() > 1) line = join(arguments, 1);
    while ((!line.empty() || (*output << "> " && std::getline(std::cin, line))) &&
           line != "/quit") {
      if (line.empty()) continue;
      if (line == "/new") {
        active_ray.clear();
        *output << "A new ray will be created by the next message.\n";
        line.clear();
        continue;
      }
      auto response = intent(client, tokmon::cbor::object({{"action", "chat"},
          {"text", line}, {"ray", active_ray}, {"deadline_ms", deadline_ms}}));
      if (!response) { print_error(response.error()); continue; }
      if (const auto* ray = tokmon::cbor::find(response->payload, "ray"))
        active_ray = std::string(ray->as_string());
      auto photons = response_photons(*response);
      if (!photons) { print_error(photons.error()); continue; }
      for (const auto& photon : *photons)
        if (photon.kind == "assistant.message" || photon.kind == "tool.result")
          print_photon(*output, photon, output_format);
      line.clear();
    }
    return 0;
  }
  if (arguments[0] == "history") {
    const auto ray = arguments.size() > 1 ? arguments[1] : std::string{};
    auto response = intent(client, tokmon::cbor::object({{"action", "history"},
                                                        {"ray", ray}}));
    if (!response) { print_error(response.error()); return 1; }
    auto photons = response_photons(*response);
    if (!photons) { print_error(photons.error()); return 1; }
    for (const auto& photon : *photons) print_photon(*output, photon, output_format);
    return 0;
  }
  if (arguments[0] == "doctor") {
    auto response = intent(client, tokmon::cbor::object({{"action", "doctor"}}));
    if (!response) { print_error(response.error()); return 1; }
    const auto* epoch = tokmon::cbor::find(response->payload, "epoch");
    const auto* hash = tokmon::cbor::find(response->payload, "hash");
    if (output_format != OutputFormat::human) {
      write_value(*output, response->payload, output_format); return 0;
    }
    *output << "storage: verified\nlight-path: epoch="
              << (epoch ? epoch->as_integer() : 0) << " hash="
              << (hash ? hash->as_string() : std::string_view{}) << '\n';
    print_lenses(*response);
    return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 1 && arguments[1] == "list") {
    auto response = intent(client, tokmon::cbor::object({{"action", "lens.list"}}));
    if (!response) { print_error(response.error()); return 1; }
    if (output_format == OutputFormat::human) print_lenses(*response);
    else write_value(*output, response->payload, output_format);
    return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 1 && arguments[1] == "verify") {
    auto response = intent(client, tokmon::cbor::object({{"action", "doctor"}}));
    if (!response) { print_error(response.error()); return 1; }
    if (output_format == OutputFormat::human) print_lenses(*response);
    else write_value(*output, response->payload, output_format);
    return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 2 &&
      (arguments[1] == "mount" || arguments[1] == "replace" ||
       arguments[1] == "unmount")) {
    const auto action = "lens." + arguments[1];
    tokmon::cbor::Value payload = tokmon::cbor::object({
        {"action", action}, {"id", arguments[2]}});
    if (arguments[1] != "unmount") {
      if (arguments.size() < 4) { std::cerr << arguments[1] << " requires artifact\n"; return 2; }
      (*payload.as_map())["artifact"] = arguments[3];
      (*payload.as_map())["runtime"] = arguments.size() > 4 ? arguments[4] : "in_process";
    }
    auto response = intent(client, std::move(payload));
    if (!response) { print_error(response.error()); return 1; }
    if (output_format == OutputFormat::human) {
      const auto* epoch = tokmon::cbor::find(response->payload, "epoch");
      *output << action << " committed at epoch " << (epoch ? epoch->as_integer() : 0) << '\n';
    } else write_value(*output, response->payload, output_format);
    return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 1 && arguments[1] == "reconcile") {
    auto response = intent(client, tokmon::cbor::object({{"action", "lens.reconcile"}}));
    if (!response) { print_error(response.error()); return 1; }
    const auto* epoch = tokmon::cbor::find(response->payload, "epoch");
    if (output_format == OutputFormat::human)
      *output << "committed mount epoch " << (epoch ? epoch->as_integer() : 0) << '\n';
    else write_value(*output, response->payload, output_format);
    return 0;
  }
  help();
  return 2;
}
