#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "tokmon/tokmon.hpp"

namespace {

std::atomic_uint64_t next_request{1};

void print_error(const tokmon::Error& error) { std::cerr << error.describe() << '\n'; }

void print_photon(const tokmon::Photon& photon) {
  std::cout << photon.sequence << "  " << photon.kind << "  "
            << tokmon::cbor::diagnostic(photon.payload) << '\n';
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
  tokmon chat <message>          Submit one complete causal ray through tokmond
  tokmon run                     Interactive conversation
  tokmon history [ray-id]        Print committed photons
  tokmon doctor                  Verify storage and active light path
  tokmon lens list               List mounted lens generations
  tokmon lens reconcile          Re-read .tokmon YAML and atomically swap
  tokmon config paths            Print resolved .tokmon directories

Options:
  --workspace <path>             Select project-level .tokmon directory

tokmond must be running for commands that read or change causal state.
)HELP";
}

tokmon::Result<tokmon::SnowMessage> intent(const tokmon::SnowClient& client,
                                            tokmon::cbor::Value payload,
                                            const std::uint64_t cursor = 0) {
  tokmon::SnowMessage message{.kind = tokmon::SnowMessageKind::intent,
      .request_id = next_request.fetch_add(1), .cursor = cursor,
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
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  std::optional<std::filesystem::path> workspace;
  for (auto iterator = arguments.begin(); iterator != arguments.end();) {
    if (*iterator == "--workspace" && std::next(iterator) != arguments.end()) {
      workspace = *std::next(iterator);
      iterator = arguments.erase(iterator, std::next(iterator, 2));
    } else ++iterator;
  }
  if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "help") {
    help(); return 0;
  }

  auto paths = tokmon::resolve_paths(workspace);
  if (!paths) { print_error(paths.error()); return 2; }
  if (arguments[0] == "config" && arguments.size() > 1 && arguments[1] == "paths") {
    std::cout << "user=" << paths->user.string() << "\nproject=" << paths->project.string()
              << "\ndatabase=" << paths->database.string() << "\nruntimes="
              << paths->runtimes.string() << '\n';
    return 0;
  }
  tokmon::SnowClient client(tokmon::default_snow_endpoint(paths->run));

  if (arguments[0] == "chat") {
    const auto message = join(arguments, 1);
    if (message.empty()) { std::cerr << "chat requires a message\n"; return 2; }
    auto response = intent(client, tokmon::cbor::object({{"action", "chat"},
                                                        {"text", message}}));
    if (!response) { print_error(response.error()); return 1; }
    auto photons = response_photons(*response);
    if (!photons) { print_error(photons.error()); return 1; }
    for (const auto& photon : *photons) print_photon(photon);
    return 0;
  }
  if (arguments[0] == "run") {
    std::cout << "Tokmon interactive mode. Type /quit to leave.\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line) && line != "/quit") {
      if (line.empty()) continue;
      auto response = intent(client, tokmon::cbor::object({{"action", "chat"},
                                                          {"text", line}}));
      if (!response) { print_error(response.error()); continue; }
      auto photons = response_photons(*response);
      if (!photons) { print_error(photons.error()); continue; }
      for (const auto& photon : *photons)
        if (photon.kind == "assistant.message" || photon.kind == "tool.result")
          print_photon(photon);
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
    for (const auto& photon : *photons) print_photon(photon);
    return 0;
  }
  if (arguments[0] == "doctor") {
    auto response = intent(client, tokmon::cbor::object({{"action", "doctor"}}));
    if (!response) { print_error(response.error()); return 1; }
    const auto* epoch = tokmon::cbor::find(response->payload, "epoch");
    const auto* hash = tokmon::cbor::find(response->payload, "hash");
    std::cout << "storage: verified\nlight-path: epoch="
              << (epoch ? epoch->as_integer() : 0) << " hash="
              << (hash ? hash->as_string() : std::string_view{}) << '\n';
    print_lenses(*response);
    return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 1 && arguments[1] == "list") {
    auto response = intent(client, tokmon::cbor::object({{"action", "lens.list"}}));
    if (!response) { print_error(response.error()); return 1; }
    print_lenses(*response); return 0;
  }
  if (arguments[0] == "lens" && arguments.size() > 1 && arguments[1] == "reconcile") {
    auto response = intent(client, tokmon::cbor::object({{"action", "lens.reconcile"}}));
    if (!response) { print_error(response.error()); return 1; }
    const auto* epoch = tokmon::cbor::find(response->payload, "epoch");
    std::cout << "committed mount epoch " << (epoch ? epoch->as_integer() : 0) << '\n';
    return 0;
  }
  help();
  return 2;
}
