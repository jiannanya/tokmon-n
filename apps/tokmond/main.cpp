#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <spdlog/spdlog.h>

#include "tokmon/tokmon.hpp"
#include "tokmon/secret_store.hpp"

namespace {
std::atomic_bool running{true};
void stop_signal(int) { running.store(false, std::memory_order_release); }

class DaemonInstanceLock final {
 public:
  explicit DaemonInstanceLock(const std::filesystem::path& endpoint) {
#if defined(_WIN32)
    const auto key = tokmon::sha256_hex(endpoint.generic_string()).substr(0, 20);
    const auto name = std::wstring(L"Local\\TokmonDaemon-") +
        std::wstring(key.begin(), key.end());
    handle_ = CreateMutexW(nullptr, TRUE, name.c_str());
    acquired_ = handle_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
#else
    const auto file = endpoint.parent_path() /
        ("tokmond-" + tokmon::sha256_hex(endpoint.generic_string()).substr(0, 16) + ".lock");
    descriptor_ = ::open(file.c_str(), O_CREAT | O_RDWR, 0600);
    acquired_ = descriptor_ >= 0 && ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0;
#endif
  }
  ~DaemonInstanceLock() {
#if defined(_WIN32)
    if (handle_) { if (acquired_) ReleaseMutex(handle_); CloseHandle(handle_); }
#else
    if (descriptor_ >= 0) { if (acquired_) (void)::flock(descriptor_, LOCK_UN); (void)::close(descriptor_); }
#endif
  }
  [[nodiscard]] bool acquired() const noexcept { return acquired_; }
 private:
  bool acquired_{false};
#if defined(_WIN32)
  HANDLE handle_{nullptr};
#else
  int descriptor_{-1};
#endif
};

tokmon::cbor::Value photon_array(const std::vector<tokmon::Photon>& photons) {
  tokmon::cbor::Value::Array values;
  values.reserve(photons.size());
  for (const auto& photon : photons) values.push_back(tokmon::to_cbor(photon));
  return values;
}

tokmon::SnowMessage snow_error(const tokmon::SnowMessage& request,
                               const tokmon::Error& error) {
  return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::error,
      .request_id = request.request_id, .cursor = request.cursor,
      .payload = tokmon::cbor::object({
          {"code", std::string(tokmon::to_string(error.code))},
          {"message", error.describe()}})};
}

tokmon::Result<void> update_project_light_path(const std::filesystem::path& file,
                                                const tokmon::cbor::Value& payload,
                                                const std::string_view action) {
  const auto* id_field = tokmon::cbor::find(payload, "id");
  if (!id_field || id_field->as_string().empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                             "Lens id is required"));
  const auto id = std::string(id_field->as_string());
  try {
    YAML::Node root;
    if (std::filesystem::exists(file)) root = YAML::LoadFile(file.string());
    if (!root || !root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
    root["version"] = 1;
    auto lenses = root["lenses"];
    if (!lenses || !lenses.IsSequence()) lenses = YAML::Node(YAML::NodeType::Sequence);
    std::size_t selected = lenses.size();
    for (std::size_t index = 0; index < lenses.size(); ++index)
      if (lenses[index]["id"] && lenses[index]["id"].as<std::string>() == id) {
        selected = index; break;
      }
    YAML::Node entry = selected < lenses.size() ? lenses[selected] :
                                                 YAML::Node(YAML::NodeType::Map);
    entry["id"] = id;
    if (action == "lens.unmount") {
      entry["enabled"] = false;
    } else {
      const auto* artifact = tokmon::cbor::find(payload, "artifact");
      const auto* runtime = tokmon::cbor::find(payload, "runtime");
      if (!artifact || artifact->as_string().empty())
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                 "Lens artifact is required"));
      entry["artifact"] = std::string(artifact->as_string());
      entry["runtime"] = runtime && !runtime->as_string().empty()
          ? std::string(runtime->as_string()) : "in_process";
      entry["enabled"] = true;
    }
    if (selected < lenses.size()) lenses[selected] = entry;
    else lenses.push_back(entry);
    root["lenses"] = lenses;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
          "cannot create project .tokmon directory: " + error.message()));
    const auto temporary = file.string() + ".new";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                                 "cannot write LightPath candidate"));
      output << root;
      output.flush();
      if (!output)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                                 "cannot flush LightPath candidate"));
    }
#if defined(_WIN32)
    const auto source = std::filesystem::path(temporary).wstring();
    const auto destination = file.wstring();
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                               "cannot atomically publish LightPath YAML"));
#else
    std::filesystem::rename(temporary, file, error);
    if (error)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                               "cannot atomically publish LightPath YAML"));
#endif
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
        "cannot update project LightPath: " + std::string(exception.what())));
  }
}

YAML::Node yaml_from_cbor(const tokmon::cbor::Value& value) {
  if (const auto* map = value.as_map()) {
    YAML::Node result(YAML::NodeType::Map);
    for (const auto& [key, child] : *map) result[key] = yaml_from_cbor(child);
    return result;
  }
  if (const auto* array = value.as_array()) {
    YAML::Node result(YAML::NodeType::Sequence);
    for (const auto& child : *array) result.push_back(yaml_from_cbor(child));
    return result;
  }
  if (const auto* text = std::get_if<std::string>(&value.data)) return YAML::Node(*text);
  if (const auto* number = std::get_if<std::int64_t>(&value.data)) return YAML::Node(*number);
  if (const auto* number = std::get_if<double>(&value.data)) return YAML::Node(*number);
  if (const auto* boolean = std::get_if<bool>(&value.data)) return YAML::Node(*boolean);
  return YAML::Node();
}

tokmon::cbor::Value cbor_from_yaml(const YAML::Node& value) {
  if (!value || value.IsNull()) return nullptr;
  if (value.IsMap()) {
    tokmon::cbor::Value::Map result;
    for (const auto& entry : value)
      result[entry.first.as<std::string>()] = cbor_from_yaml(entry.second);
    return result;
  }
  if (value.IsSequence()) {
    tokmon::cbor::Value::Array result;
    for (const auto& child : value) result.push_back(cbor_from_yaml(child));
    return result;
  }
  const auto text = value.Scalar();
  if (text == "true") return true;
  if (text == "false") return false;
  try {
    std::size_t parsed = 0;
    const auto integer = std::stoll(text, &parsed);
    if (parsed == text.size()) return static_cast<std::int64_t>(integer);
  } catch (...) {}
  return text;
}

tokmon::Result<void> update_project_settings(const std::filesystem::path& file,
                                             const tokmon::cbor::Value& payload) {
  const auto* values = tokmon::cbor::find(payload, "values");
  if (!values || !values->as_map())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                             "settings values must be a map"));
  try {
    YAML::Node root;
    if (std::filesystem::exists(file)) root = YAML::LoadFile(file.string());
    if (!root || !root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
    root["ui"] = yaml_from_cbor(*values);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
          "cannot create project .tokmon directory: " + error.message()));
    const auto temporary = std::filesystem::path(file.string() + ".new");
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                                 "cannot write UI settings candidate"));
      output << root;
      output.flush();
      if (!output)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                                 "cannot flush UI settings candidate"));
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.wstring().c_str(), file.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                               "cannot atomically publish UI settings YAML"));
#else
    std::filesystem::rename(temporary, file, error);
    if (error)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                               "cannot atomically publish UI settings YAML"));
#endif
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
        "cannot update project UI settings: " + std::string(exception.what())));
  }
}

tokmon::Result<tokmon::cbor::Value> read_project_settings(
    const std::filesystem::path& file) {
  try {
    if (!std::filesystem::exists(file)) return tokmon::cbor::Value::Map{};
    const auto root = YAML::LoadFile(file.string());
    if (!root || !root.IsMap() || !root["ui"]) return tokmon::cbor::Value::Map{};
    const auto value = cbor_from_yaml(root["ui"]);
    if (!value.as_map())
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                               "config ui must be a map"));
    return value;
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
        "cannot read project UI settings: " + std::string(exception.what())));
  }
}

bool loopback_endpoint(const std::string_view endpoint) {
  return endpoint.starts_with("http://127.0.0.1") ||
      endpoint.starts_with("http://localhost") || endpoint.starts_with("http://[::1]");
}

tokmon::Result<void> publish_yaml(const std::filesystem::path& file,
                                  const YAML::Node& root,
                                  const std::string_view description) {
  std::error_code error;
  std::filesystem::create_directories(file.parent_path(), error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
        "cannot create project .tokmon directory: " + error.message()));
  const auto temporary = std::filesystem::path(file.string() + ".new");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
          "cannot write " + std::string(description) + " candidate"));
    output << root;
    output.flush();
    if (!output)
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
          "cannot flush " + std::string(description) + " candidate"));
  }
#if defined(_WIN32)
  if (!MoveFileExW(temporary.wstring().c_str(), file.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
        "cannot atomically publish " + std::string(description) + " YAML"));
#else
  std::filesystem::rename(temporary, file, error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
        "cannot atomically publish " + std::string(description) + " YAML"));
#endif
  return {};
}

std::string provider_secret_ref(const std::string_view id) {
  return "model-provider/" + std::string(id);
}

tokmon::Result<void> update_project_model_provider(const std::filesystem::path& file,
                                                    const tokmon::cbor::Value& payload) {
  const auto read = [&payload](const char* key, const std::string_view fallback = {}) {
    const auto* field = tokmon::cbor::find(payload, key);
    return field ? std::string(field->as_string(fallback)) : std::string(fallback);
  };
  const auto id = read("id");
  const auto protocol = read("protocol", "openai-compatible");
  const auto endpoint = read("endpoint");
  const auto model = read("model");
  const auto auth = read("auth", "protocol-default");
  static const std::regex id_pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
  static const std::set<std::string> protocols{
      "openai-compatible", "anthropic", "gemini"};
  static const std::set<std::string> auth_modes{
      "protocol-default", "bearer", "x-api-key", "x-goog-api-key", "none"};
  const bool allow_anonymous = tokmon::cbor::find(payload, "allow_anonymous") &&
      tokmon::cbor::find(payload, "allow_anonymous")->as_bool();
  const auto max_output_tokens = tokmon::cbor::find(payload, "max_output_tokens")
      ? tokmon::cbor::find(payload, "max_output_tokens")->as_integer(4096) : 4096;
  const auto max_attempts = tokmon::cbor::find(payload, "max_attempts")
      ? tokmon::cbor::find(payload, "max_attempts")->as_integer(2) : 2;
  const auto retry_backoff_ms = tokmon::cbor::find(payload, "retry_backoff_ms")
      ? tokmon::cbor::find(payload, "retry_backoff_ms")->as_integer(250) : 250;
  if (!std::regex_match(id, id_pattern) || id == "local" || !protocols.contains(protocol) ||
      !auth_modes.contains(auth) || endpoint.empty() || model.empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
        "provider requires a valid id, protocol, HTTPS endpoint, model and auth mode"));
  if (!endpoint.starts_with("https://") && !loopback_endpoint(endpoint))
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::permission_denied,
        "provider endpoint must use HTTPS or loopback HTTP"));
  if (allow_anonymous && !loopback_endpoint(endpoint) && auth != "none")
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::permission_denied,
        "anonymous remote providers must explicitly use auth: none"));
  if (max_output_tokens <= 0 || max_output_tokens > 1'000'000 ||
      max_attempts <= 0 || max_attempts > 10 ||
      retry_backoff_ms < 0 || retry_backoff_ms > 60'000)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
        "provider retry/token limits are outside the allowed range"));
  try {
    YAML::Node root;
    if (std::filesystem::exists(file)) root = YAML::LoadFile(file.string());
    if (!root || !root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
    auto providers = root["models"]["providers"];
    if (!providers || !providers.IsMap()) providers = YAML::Node(YAML::NodeType::Map);
    auto entry = providers[id];
    if (!entry || !entry.IsMap()) entry = YAML::Node(YAML::NodeType::Map);
    entry["protocol"] = protocol;
    entry["endpoint"] = endpoint;
    entry["model"] = model;
    entry["secret_ref"] = provider_secret_ref(id);
    entry["auth"] = auth;
    entry["enabled"] = !tokmon::cbor::find(payload, "enabled") ||
        tokmon::cbor::find(payload, "enabled")->as_bool();
    entry["allow_anonymous"] = allow_anonymous;
    entry["thinking"] = tokmon::cbor::find(payload, "thinking") &&
        tokmon::cbor::find(payload, "thinking")->as_bool();
    entry["reasoning_effort"] = read("reasoning_effort");
    entry["max_output_tokens"] = max_output_tokens;
    entry["max_attempts"] = max_attempts;
    entry["retry_backoff_ms"] = retry_backoff_ms;
    providers[id] = entry;
    root["models"]["providers"] = providers;
    if (tokmon::cbor::find(payload, "default") &&
        tokmon::cbor::find(payload, "default")->as_bool()) root["models"]["default"] = id;
    return publish_yaml(file, root, "model provider");
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
        "cannot update project model provider: " + std::string(exception.what())));
  }
}

tokmon::Result<void> select_project_model_provider(const std::filesystem::path& file,
                                                   const std::string_view id) {
  try {
    YAML::Node root;
    if (std::filesystem::exists(file)) root = YAML::LoadFile(file.string());
    if (!root || !root.IsMap()) root = YAML::Node(YAML::NodeType::Map);
    root["models"]["default"] = std::string(id);
    return publish_yaml(file, root, "default model provider");
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
        "cannot select project model provider: " + std::string(exception.what())));
  }
}

tokmon::cbor::Value provider_value(const tokmon::ModelProviderConfig& provider,
                                   const bool credential_present) {
  return tokmon::cbor::object({
      {"id", provider.id}, {"protocol", provider.protocol},
      {"endpoint", provider.endpoint}, {"model", provider.model},
      {"auth", provider.auth}, {"enabled", provider.enabled},
      {"allow_anonymous", provider.allow_anonymous}, {"thinking", provider.thinking},
      {"reasoning_effort", provider.reasoning_effort},
      {"max_output_tokens", provider.max_output_tokens},
      {"max_attempts", provider.max_attempts},
      {"retry_backoff_ms", provider.retry_backoff_ms},
      {"credential_present", credential_present}});
}

tokmon::Result<tokmon::cbor::Value> resolved_model_context(
    const tokmon::RuntimeConfig& config, const tokmon::cbor::Value& payload) {
  const auto* requested = tokmon::cbor::find(payload, "provider");
  const auto id = requested && !requested->as_string().empty()
      ? std::string(requested->as_string()) : config.default_model_provider;
  const auto found = config.model_providers.find(id);
  if (found == config.model_providers.end() || !found->second.enabled)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::not_found,
        "requested model provider is not configured or is disabled"));
  const auto& provider = found->second;
  auto context = tokmon::cbor::object({
      {"provider", provider.id}, {"protocol", provider.protocol},
      {"endpoint", provider.endpoint}, {"model", provider.model},
      {"auth", provider.auth}, {"allow_anonymous", provider.allow_anonymous},
      {"thinking", provider.thinking}, {"reasoning_effort", provider.reasoning_effort},
      {"max_output_tokens", provider.max_output_tokens},
      {"max_attempts", provider.max_attempts},
      {"retry_backoff_ms", provider.retry_backoff_ms},
      {"access_mode", tokmon::cbor::find(payload, "access_mode")
          ? std::string(tokmon::cbor::find(payload, "access_mode")->as_string())
          : std::string("完全访问")},
      {"effort", tokmon::cbor::find(payload, "effort")
          ? std::string(tokmon::cbor::find(payload, "effort")->as_string())
          : std::string("标准")}});
  if (!provider.secret_ref.empty()) (*context.as_map())["secret_ref"] = provider.secret_ref;
  return context;
}
}

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> workspace;
  std::optional<std::filesystem::path> endpoint_override;
  bool once = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--once") once = true;
    else if (argument == "--workspace" && index + 1 < argc) workspace = argv[++index];
    else if (argument == "--endpoint" && index + 1 < argc) endpoint_override = argv[++index];
  }
  std::signal(SIGINT, stop_signal);
  std::signal(SIGTERM, stop_signal);
  tokmon::TokmonRuntime runtime;
  if (auto opened = runtime.open(workspace, "tokmond"); !opened) {
    std::cerr << opened.error().describe() << '\n'; return 2;
  }
  if (once) return runtime.verify() ? 0 : 1;

  std::mutex runtime_mutex;
  std::mutex request_mutex;
  std::set<std::uint64_t> cancelled_requests;
  std::unordered_map<std::uint64_t, tokmon::SnowMessage> completed_requests;
  std::unordered_map<std::uint64_t, tokmon::RayId> active_requests;
  tokmon::SnowServer snow;
  const auto endpoint = endpoint_override.value_or(tokmon::workspace_snow_endpoint(
      runtime.config().paths.run, runtime.config().paths.project.parent_path()));
  DaemonInstanceLock instance_lock(endpoint);
  if (!instance_lock.acquired()) return 0;
  auto snow_started = snow.start(endpoint, [&runtime, &runtime_mutex, &request_mutex,
                                        &cancelled_requests, &completed_requests,
                                        &active_requests, &endpoint, &snow](
      const tokmon::SnowMessage& request) -> tokmon::SnowMessage {
    if (request.kind == tokmon::SnowMessageKind::cancel) {
      const auto* target = tokmon::cbor::find(request.payload, "request_id");
      std::optional<tokmon::RayId> active_ray;
      if (target) {
        std::scoped_lock state_lock(request_mutex);
        const auto target_id = static_cast<std::uint64_t>(target->as_integer());
        cancelled_requests.insert(target_id);
        if (const auto found = active_requests.find(target_id); found != active_requests.end())
          active_ray = found->second;
      }
      if (active_ray) runtime.cancel(*active_ray);
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"cancel_requested", true},
              {"active_ray", active_ray ? tokmon::cbor::Value(*active_ray)
                                         : tokmon::cbor::Value("")}})};
    }
    if (request.kind == tokmon::SnowMessageKind::close)
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::closed,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"ordered", true}})};
    {
      std::scoped_lock state_lock(request_mutex);
      if (request.request_id != 0) {
        if (const auto found = completed_requests.find(request.request_id);
            found != completed_requests.end()) return found->second;
        if (cancelled_requests.contains(request.request_id))
          return snow_error(request, tokmon::make_error(tokmon::ErrorCode::cancelled,
                                                        "request was cancelled"));
      }
    }
    std::scoped_lock lock(runtime_mutex);
    const auto remember = [&](tokmon::SnowMessage response) {
      if (request.request_id != 0) {
        std::scoped_lock state_lock(request_mutex);
        if (completed_requests.size() >= 1024u) completed_requests.erase(completed_requests.begin());
        completed_requests[request.request_id] = response;
      }
      return response;
    };
    if (request.kind == tokmon::SnowMessageKind::hello) {
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::hello,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"service", "tokmond"},
              {"protocol_major", tokmon::snow_protocol_major},
              {"protocol_minor", tokmon::snow_protocol_minor},
              {"workspace", runtime.config().paths.project.parent_path().generic_string()}})});
    }
    if (request.kind == tokmon::SnowMessageKind::ping) {
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::pong,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"healthy", true}})});
    }
    if (request.kind == tokmon::SnowMessageKind::snapshot_request) {
      auto complete_history = runtime.history_all(0);
      if (!complete_history) return snow_error(request, complete_history.error());
      const auto tail = complete_history->empty() ? 0u : complete_history->back().sequence;
      if (request.cursor > tail)
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::protocol_error,
            "Snow cursor is ahead of the committed causal tail; a fresh snapshot is required"));
      auto photons = runtime.history_all(request.cursor);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return remember(tokmon::SnowMessage{.kind = request.cursor == 0
              ? tokmon::SnowMessageKind::snapshot : tokmon::SnowMessageKind::delta,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"photons", photon_array(*photons)},
              {"mode", request.cursor == 0 ? "snapshot" : "delta"},
              {"from_cursor", static_cast<std::int64_t>(request.cursor)},
              {"to_cursor", static_cast<std::int64_t>(cursor)}, {"gap", false},
              {"light_path_epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"light_path_hash", runtime.light_path()->hash}})});
    }
    if (request.kind != tokmon::SnowMessageKind::intent)
      return snow_error(request, tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                    "unsupported Snow request"));
    const auto* action_field = tokmon::cbor::find(request.payload, "action");
    const auto action = action_field ? action_field->as_string() : std::string_view{};
    if (action == "daemon.shutdown") {
      running.store(false, std::memory_order_release);
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"stopping", true},
                                           {"ownership", "shared-background-service"}})});
    }
    if (action == "settings.get") {
      auto values = read_project_settings(runtime.config().paths.project / "config.yaml");
      if (!values) return snow_error(request, values.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"values", std::move(*values)},
                                           {"scope", "project"}})});
    }
    if (action == "settings.save") {
      auto saved = update_project_settings(runtime.config().paths.project / "config.yaml",
                                           request.payload);
      if (!saved) return snow_error(request, saved.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"saved", true}, {"scope", "project"},
              {"path", (runtime.config().paths.project / "config.yaml").generic_string()}})});
    }
    if (action == "model.providers") {
      std::set<std::string> credentials;
      auto stored = tokmon::builtin::keyring_list();
      if (stored)
        for (const auto& item : *stored) credentials.insert(item.id);
      tokmon::cbor::Value::Array providers;
      std::vector<std::string> ids;
      ids.reserve(runtime.config().model_providers.size());
      for (const auto& [id, provider] : runtime.config().model_providers) ids.push_back(id);
      std::ranges::sort(ids);
      for (const auto& id : ids) {
        const auto& provider = runtime.config().model_providers.at(id);
        providers.push_back(provider_value(provider,
            provider.auth == "none" || provider.allow_anonymous ||
            credentials.contains(provider.secret_ref)));
      }
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"default", runtime.config().default_model_provider},
              {"providers", std::move(providers)}, {"secrets", "redacted"}})});
    }
    if (action == "model.provider.configure") {
      const auto file = runtime.config().paths.project / "config.yaml";
      auto saved = update_project_model_provider(file, request.payload);
      if (!saved) return snow_error(request, saved.error());
      auto reconciled = runtime.reconcile();
      if (!reconciled) return snow_error(request, reconciled.error());
      const auto id = std::string(tokmon::cbor::find(request.payload, "id")->as_string());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"configured", true}, {"id", id},
              {"default", runtime.config().default_model_provider},
              {"path", file.generic_string()}, {"credential", "redacted"}})});
    }
    if (action == "model.provider.use") {
      const auto* id_field = tokmon::cbor::find(request.payload, "id");
      const auto id = id_field ? std::string(id_field->as_string()) : std::string{};
      const auto found = runtime.config().model_providers.find(id);
      if (found == runtime.config().model_providers.end() || !found->second.enabled)
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::not_found,
            "default model provider must be configured and enabled"));
      auto selected = select_project_model_provider(
          runtime.config().paths.project / "config.yaml", id);
      if (!selected) return snow_error(request, selected.error());
      auto reconciled = runtime.reconcile();
      if (!reconciled) return snow_error(request, reconciled.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"selected", id}})});
    }
    if (action == "model.provider.secret.set" ||
        action == "model.provider.secret.delete") {
      const auto* id_field = tokmon::cbor::find(request.payload, "id");
      const auto id = id_field ? std::string(id_field->as_string()) : std::string{};
      const auto found = runtime.config().model_providers.find(id);
      if (found == runtime.config().model_providers.end() || id == "local")
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::not_found,
                                                       "model provider is not configured"));
      tokmon::Result<void> changed;
      if (action == "model.provider.secret.set") {
        const auto* secret = tokmon::cbor::find(request.payload, "secret");
        if (!secret || secret->as_string().empty())
          return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                         "API secret is required"));
        changed = tokmon::builtin::keyring_write(found->second.secret_ref, "model-api",
                                                  secret->as_string());
      } else {
        changed = tokmon::builtin::keyring_delete(found->second.secret_ref);
      }
      if (!changed) return snow_error(request, changed.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"id", id}, {"credential_present", action.ends_with(".set")},
              {"storage", "operating-system-credential-manager"}})});
    }
    if (action == "chat" || action == "model.provider.test") {
      const auto* text_field = tokmon::cbor::find(request.payload, "text");
      const auto text = text_field && !text_field->as_string().empty()
          ? std::string(text_field->as_string())
          : action == "model.provider.test"
              ? std::string("Reply with exactly: TOKMON_PROVIDER_OK") : std::string{};
      if (text.empty())
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                      "chat text is required"));
      const auto deadline_ms = tokmon::cbor::find(request.payload, "deadline_ms")
          ? tokmon::cbor::find(request.payload, "deadline_ms")->as_integer() : 0;
      if (deadline_ms < 0 || deadline_ms > 86'400'000)
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
            "request deadline_ms must be within 0..86400000"));
      const auto* requested_ray = tokmon::cbor::find(request.payload, "ray");
      auto context = resolved_model_context(runtime.config(), request.payload);
      if (!context) return snow_error(request, context.error());
      auto ray = requested_ray && !requested_ray->as_string().empty()
          ? runtime.submit_to(std::string(requested_ray->as_string()), text, *context)
          : runtime.submit(text, *context);
      if (!ray) return snow_error(request, ray.error());
      {
        std::scoped_lock state_lock(request_mutex);
        active_requests[request.request_id] = *ray;
        if (cancelled_requests.contains(request.request_id)) runtime.cancel(*ray);
      }
      std::mutex deadline_mutex;
      std::condition_variable_any deadline_condition;
      std::atomic_bool deadline_expired{false};
      std::jthread deadline_watch;
      if (deadline_ms > 0) {
        deadline_watch = std::jthread([&runtime, &deadline_mutex, &deadline_condition,
                                      &deadline_expired, ray = *ray, deadline_ms](
                                         const std::stop_token stop) {
          std::unique_lock deadline_lock(deadline_mutex);
          const auto interrupted = deadline_condition.wait_for(deadline_lock, stop,
              std::chrono::milliseconds(deadline_ms), [] { return false; });
          if (!interrupted && !stop.stop_requested()) {
            deadline_expired.store(true, std::memory_order_release);
            runtime.cancel(ray);
          }
        });
      }
      auto advanced = runtime.advance(*ray);
      if (deadline_watch.joinable()) {
        deadline_watch.request_stop();
        deadline_condition.notify_all();
        deadline_watch.join();
      }
      {
        std::scoped_lock state_lock(request_mutex);
        active_requests.erase(request.request_id);
      }
      if (!advanced && deadline_expired.load(std::memory_order_acquire))
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::timeout,
            "Snow request deadline elapsed"));
      if (!advanced) return snow_error(request, advanced.error());
      auto photons = runtime.history(*ray);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"ray", *ray},
              {"beats", static_cast<std::int64_t>(*advanced)},
              {"photons", photon_array(*photons)},
              {"provider_test", action == "model.provider.test"}})});
    }
    if (action == "history") {
      const auto* ray_field = tokmon::cbor::find(request.payload, "ray");
      tokmon::Result<std::vector<tokmon::Photon>> photons =
          ray_field && !ray_field->as_string().empty()
              ? runtime.history(std::string(ray_field->as_string()))
              : runtime.history_all(request.cursor);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"photons", photon_array(*photons)}})});
    }
    if (action == "surface") {
      const auto* ray_field = tokmon::cbor::find(request.payload, "ray");
      if (!ray_field || ray_field->as_string().empty())
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                       "surface ray is required"));
      auto surface = runtime.surface(std::string(ray_field->as_string()));
      if (!surface) return snow_error(request, surface.error());
      auto photons = runtime.history(std::string(ray_field->as_string()));
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"ray", std::string(ray_field->as_string())},
              {"surface", tokmon::to_cbor(*surface)},
              {"source", "committed-photon-fold"}})});
    }
    if (action == "lens.reconcile") {
      auto result = runtime.reconcile();
      if (!result) return snow_error(request, result.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"hash", runtime.light_path()->hash}})});
    }
    if (action == "lens.mount" || action == "lens.replace" || action == "lens.unmount") {
      auto updated = update_project_light_path(runtime.config().paths.project / "light-path.yaml",
                                               request.payload, action);
      if (!updated) return snow_error(request, updated.error());
      auto result = runtime.reconcile();
      if (!result) return snow_error(request, result.error());
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"action", std::string(action)},
              {"epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"hash", runtime.light_path()->hash}})});
    }
    if (action == "lens.list" || action == "doctor") {
      if (action == "doctor") {
        auto verified = runtime.verify();
        if (!verified) return snow_error(request, verified.error());
      }
      tokmon::cbor::Value::Array lenses;
      for (const auto& mounted : runtime.light_path()->lenses) {
        lenses.push_back(tokmon::cbor::object({
            {"id", mounted.lens->manifest().id},
            {"version", mounted.lens->manifest().version},
            {"runtime", std::string(tokmon::to_string(mounted.lens->manifest().runtime))},
            {"generation", static_cast<std::int64_t>(mounted.generation)},
            {"artifact_hash", mounted.artifact_hash}}));
      }
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"verified", true},
              {"epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"hash", runtime.light_path()->hash}, {"lenses", std::move(lenses)},
              {"checks", tokmon::cbor::object({
                  {"config", tokmon::cbor::object({
                      {"user", runtime.config().paths.user.generic_string()},
                      {"project", runtime.config().paths.project.generic_string()},
                      {"yaml", true}})},
                  {"database", tokmon::cbor::object({
                      {"path", runtime.config().paths.database.generic_string()},
                      {"append_only_verified", true}})},
                  {"light_path", tokmon::cbor::object({{"verified", true},
                      {"mounted", static_cast<std::int64_t>(runtime.light_path()->lenses.size())}})},
                  {"artifacts", tokmon::cbor::object({{"content_hashes", true},
                      {"signature_policy", runtime.config().require_signatures}})},
                  {"runtimes", tokmon::cbor::object({{"cpp", true},
                      {"node", true}, {"cpython", true}})},
                  {"providers", tokmon::cbor::object({{"health_from_photons", true}})},
                  {"sandbox", tokmon::cbor::object({
#if defined(_WIN32)
                      {"backend", "windows-job-object/conpty"},
#else
                      {"backend", "posix-process-group/pty"},
#endif
                      {"controlled_degradation", true}})},
                  {"secret_backend", tokmon::cbor::object({
#if defined(_WIN32)
                      {"backend", "windows-credential-manager"}, {"available", true}
#elif defined(__APPLE__)
                      {"backend", "macos-keychain"}, {"available", true}
#else
                      {"backend", "libsecret"}, {"available", true}
#endif
                  })},
                  {"ui_connection", tokmon::cbor::object({
                      {"endpoint", endpoint.generic_string()}, {"snow", snow.running()}})}})}})});
    }
    return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                  "unknown intent action"));
  });
  if (!snow_started) {
    std::cerr << snow_started.error().describe() << '\n'; return 2;
  }
  spdlog::info("Snow endpoint listening at {}", endpoint.string());

  const std::vector<std::filesystem::path> watched_config{
      runtime.config().paths.user / "config.yaml",
      runtime.config().paths.project / "config.yaml",
      runtime.config().paths.user / "light-path.yaml",
      runtime.config().paths.project / "light-path.yaml"};
  std::vector<std::filesystem::file_time_type> watched_time(
      watched_config.size(), std::filesystem::file_time_type::min());
  while (running.load(std::memory_order_acquire)) {
    std::error_code error;
    bool changed = false;
    for (std::size_t index = 0; index < watched_config.size(); ++index) {
      const auto next = std::filesystem::exists(watched_config[index], error)
          ? std::filesystem::last_write_time(watched_config[index], error)
          : std::filesystem::file_time_type::min();
      if (watched_time[index] != std::filesystem::file_time_type::min() &&
          next != watched_time[index]) changed = true;
      watched_time[index] = next;
    }
    if (changed) {
      std::scoped_lock lock(runtime_mutex);
      if (auto result = runtime.reconcile(); !result)
        spdlog::error("configuration reconcile rejected: {}", result.error().describe());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  snow.stop();
  runtime.stop();
  return 0;
}
