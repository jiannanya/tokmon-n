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
#include <sstream>
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

std::string photon_text(const tokmon::Photon& photon) {
  for (const auto* key : {"text", "summary", "stdout", "detail", "message"})
    if (const auto* value = tokmon::cbor::find(photon.payload, key);
        value && std::holds_alternative<std::string>(value->data))
      return std::string(value->as_string());
  return tokmon::cbor::diagnostic(photon.payload);
}

std::string photon_lines(const std::vector<tokmon::Photon>& photons,
                         const std::size_t limit = 80) {
  std::ostringstream output;
  const auto first = photons.size() > limit ? photons.size() - limit : 0;
  for (std::size_t index = first; index < photons.size(); ++index) {
    const auto& photon = photons[index];
    output << '#' << photon.sequence << " " << photon.kind;
    const auto detail = photon_text(photon);
    if (!detail.empty()) output << " — " << detail;
    output << '\n';
  }
  return output.str();
}

tokmon::Result<tokmon::RefractionResult> invoke_lens(
    tokmon::TokmonRuntime& runtime, const tokmon::RayId& ray,
    std::string kind, std::string schema, std::string target,
    tokmon::cbor::Value parameters,
    const tokmon::RiskClass risk = tokmon::RiskClass::observe) {
  return runtime.refract(tokmon::Act{.id = tokmon::make_id("act"), .ray = ray,
      .kind = std::move(kind), .schema = std::move(schema),
      .parameters = std::move(parameters), .target = std::move(target),
      .epoch = runtime.light_path()->epoch, .risk = risk,
      .idempotency_key = tokmon::make_id("command-binding")});
}

tokmon::Result<tokmon::cbor::Value> execute_slash_command(
    tokmon::TokmonRuntime& runtime, const tokmon::cbor::Value& request_payload) {
  const auto* text_field = tokmon::cbor::find(request_payload, "text");
  const auto text = text_field ? std::string(text_field->as_string()) : std::string{};
  auto parsed = tokmon::parse_slash_command(text);
  if (!parsed) return tl::unexpected(parsed.error());

  const auto* requested_ray = tokmon::cbor::find(request_payload, "ray");
  tokmon::RayId active_ray = requested_ray
      ? std::string(requested_ray->as_string()) : std::string{};
  if (parsed->descriptor->requires_ray && active_ray.empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_state,
        parsed->descriptor->usage + " requires an active session"));
  const auto* snow_request = tokmon::cbor::find(request_payload, "snow_request_id");
  const auto command_ray = active_ray.empty()
      ? "command-ray-" + (snow_request ? std::string(snow_request->as_string())
                                       : tokmon::make_id("request"))
      : active_ray;
  auto invoked = runtime.store().append(tokmon::PhotonDraft{.ray = command_ray,
      .kind = "command.invoked", .schema = "tokmon.command.invoked.v1",
      .payload = tokmon::cbor::object({
          {"name", parsed->descriptor->name}, {"invoked_name", parsed->invoked_name},
          {"arguments", parsed->raw_arguments}, {"surface", tokmon::cbor::find(request_payload, "surface")
              ? *tokmon::cbor::find(request_payload, "surface") : tokmon::cbor::Value("unknown")},
          {"modifies_committed_history", false}}), .epoch = runtime.light_path()->epoch});
  if (!invoked) return tl::unexpected(invoked.error());

  auto observed = invoke_lens(runtime, command_ray, "command.invoke",
      "tokmon.command.invoke.v1", "org.tokmon.lens.snow",
      tokmon::cbor::object({{"command", parsed->descriptor->name},
                            {"arguments", parsed->raw_arguments},
                            {"invocation_photon", invoked->id}}));
  if (!observed) return tl::unexpected(observed.error());

  tokmon::cbor::Value response = tokmon::cbor::object({
      {"command", parsed->descriptor->name}, {"ray", active_ray},
      {"display", ""}, {"close_client", false}, {"clear_session", false},
      {"open_settings", false}});
  const auto set = [&response](const char* key, tokmon::cbor::Value value) {
    (*response.as_map())[key] = std::move(value);
  };
  const auto argument = [&](const std::size_t index) -> std::string {
    return index < parsed->arguments.size() ? parsed->arguments[index] : std::string{};
  };
  const auto history_for = [&runtime](const tokmon::RayId& ray)
      -> tokmon::Result<std::vector<tokmon::Photon>> {
    return ray.empty() ? runtime.history_all(0) : runtime.history(ray);
  };
  const auto append_fact = [&runtime](const tokmon::RayId& ray, std::string kind,
                                      std::string schema, tokmon::cbor::Value payload)
      -> tokmon::Result<tokmon::Photon> {
    return runtime.store().append(tokmon::PhotonDraft{.ray = ray,
        .kind = std::move(kind), .schema = std::move(schema),
        .payload = std::move(payload), .epoch = runtime.light_path()->epoch});
  };

  const auto& name = parsed->descriptor->name;
  if (name == "help") {
    if (const auto target = argument(0); !target.empty()) {
      const auto* command = tokmon::find_slash_command(target);
      if (!command) return tl::unexpected(tokmon::make_error(
          tokmon::ErrorCode::not_found, "unknown slash command /" + target));
      set("display", tokmon::slash_command_help(*command));
    } else set("display", tokmon::slash_command_help());
  } else if (name == "clear") {
    set("clear_session", true);
    set("ray", "");
    set("display", "已开始新会话；原会话光子仍完整保留。\n下一条消息将创建一束新光线。");
  } else if (name == "exit") {
    set("close_client", true);
    set("display", "正在关闭当前客户端；tokmond 将按客户端租约规则安全退出。");
  } else if (name == "status") {
    auto all = runtime.history_all(0);
    if (!all) return tl::unexpected(all.error());
    std::ostringstream output;
    output << "tokmond: healthy\nLightPath: epoch " << runtime.light_path()->epoch
           << ", " << runtime.light_path()->lenses.size() << " Lenses\nprovider: "
           << runtime.config().default_model_provider << "\nPhoton tail: "
           << (all->empty() ? 0 : all->back().sequence) << "\nactive ray: "
           << (active_ray.empty() ? "none" : active_ray);
    set("display", output.str());
  } else if (name == "history") {
    const auto target = argument(0).empty() ? active_ray : argument(0);
    auto photons = history_for(target);
    if (!photons) return tl::unexpected(photons.error());
    set("display", photons->empty() ? "没有已提交光子。" : photon_lines(*photons));
    set("photons", photon_array(*photons));
  } else if (name == "resume") {
    const auto target = argument(0);
    if (target.empty()) return tl::unexpected(tokmon::make_error(
        tokmon::ErrorCode::invalid_argument, "/resume requires a ray-id"));
    auto photons = runtime.history(target);
    if (!photons || photons->empty()) return tl::unexpected(photons
        ? tokmon::make_error(tokmon::ErrorCode::not_found, "ray has no committed photons")
        : photons.error());
    active_ray = target;
    set("ray", active_ray);
    set("display", "已恢复光线 " + active_ray + "；后续输入会继续追加光子。");
    set("photons", photon_array(*photons));
  } else if (name == "rename") {
    if (parsed->raw_arguments.empty()) return tl::unexpected(tokmon::make_error(
        tokmon::ErrorCode::invalid_argument, "/rename requires a title"));
    auto renamed = append_fact(active_ray, "session.renamed", "tokmon.session.title.v1",
        tokmon::cbor::object({{"title", parsed->raw_arguments},
                              {"replaces_display_title_only", true}}));
    if (!renamed) return tl::unexpected(renamed.error());
    set("session_title", parsed->raw_arguments);
    set("display", "会话标题已更新为：“" + parsed->raw_arguments + "”。");
  } else if (name == "branch" || name == "rewind") {
    auto parent = runtime.history(active_ray);
    if (!parent) return tl::unexpected(parent.error());
    std::uint64_t from_sequence = parent->empty() ? 0 : parent->back().sequence;
    if (name == "rewind") {
      try { from_sequence = static_cast<std::uint64_t>(std::stoull(argument(0))); }
      catch (...) { return tl::unexpected(tokmon::make_error(
          tokmon::ErrorCode::invalid_argument, "/rewind requires a valid sequence")); }
      if (std::ranges::none_of(*parent, [from_sequence](const auto& photon) {
            return photon.sequence == from_sequence;
          })) return tl::unexpected(tokmon::make_error(
              tokmon::ErrorCode::not_found, "sequence is not part of the active ray"));
    }
    const auto child = tokmon::make_id("ray");
    const auto branch_name = name == "branch" && !parsed->raw_arguments.empty()
        ? parsed->raw_arguments : name + "-from-" + std::to_string(from_sequence);
    auto child_fact = append_fact(child, "ray.branched", "tokmon.ray.branch.v1",
        tokmon::cbor::object({{"parent_ray", active_ray},
          {"from_sequence", static_cast<std::int64_t>(from_sequence)},
          {"name", branch_name}, {"history_deleted", false}}));
    if (!child_fact) return tl::unexpected(child_fact.error());
    auto parent_fact = append_fact(active_ray, "branch.created", "tokmon.ray.branch.v1",
        tokmon::cbor::object({{"child_ray", child},
          {"from_sequence", static_cast<std::int64_t>(from_sequence)},
          {"name", branch_name}, {"history_deleted", false}}));
    if (!parent_fact) return tl::unexpected(parent_fact.error());
    active_ray = child;
    set("ray", active_ray);
    set("session_title", branch_name);
    set("display", "已从序号 #" + std::to_string(from_sequence) +
        " 创建新光线 " + child + "；原光线未被修改。");
  } else if (name == "copy") {
    auto photons = runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::size_t count = 1;
    if (!argument(0).empty()) try { count = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::stoull(argument(0))), 1, 20); } catch (...) {}
    std::vector<std::string> messages;
    for (auto iterator = photons->rbegin(); iterator != photons->rend() && messages.size() < count;
         ++iterator) if (iterator->kind == "assistant.message")
      messages.push_back(photon_text(*iterator));
    std::ranges::reverse(messages);
    std::ostringstream copied;
    for (std::size_t index = 0; index < messages.size(); ++index) {
      if (index != 0) copied << "\n\n";
      copied << messages[index];
    }
    set("copy_text", copied.str());
    set("display", messages.empty() ? "当前会话还没有助手回复。" : "已准备复制助手回复。");
  } else if (name == "export") {
    auto photons = runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::ostringstream markdown;
    markdown << "# Tokmon session\n\n- Ray: `" << active_ray
             << "`\n- Append-only: true\n\n";
    for (const auto& photon : *photons)
      markdown << "## #" << photon.sequence << " · " << photon.kind << "\n\n"
               << photon_text(photon) << "\n\n";
    const auto workspace = runtime.config().paths.project.parent_path();
    const auto destination = argument(0).empty()
        ? "tokmon-session-" + active_ray.substr(0, std::min<std::size_t>(12, active_ray.size())) + ".md"
        : argument(0);
    auto created = invoke_lens(runtime, active_ray, "artifact.create",
        "tokmon.artifact.create.v1", "org.tokmon.lens.cove",
        tokmon::cbor::object({{"workspace_root", workspace.generic_string()},
                              {"content", markdown.str()}}), tokmon::RiskClass::reversible);
    if (!created) return tl::unexpected(created.error());
    auto refreshed = runtime.history(active_ray);
    if (!refreshed) return tl::unexpected(refreshed.error());
    const tokmon::Photon* artifact = nullptr;
    for (auto iterator = refreshed->rbegin(); iterator != refreshed->rend(); ++iterator)
      if (iterator->kind == "artifact.created") { artifact = &*iterator; break; }
    const auto* digest = artifact ? tokmon::cbor::find(artifact->payload, "sha256") : nullptr;
    if (!digest) return tl::unexpected(tokmon::make_error(
        tokmon::ErrorCode::invalid_state, "Cove did not emit artifact.created"));
    auto exported = invoke_lens(runtime, active_ray, "artifact.export",
        "tokmon.artifact.export.v1", "org.tokmon.lens.cove",
        tokmon::cbor::object({{"workspace_root", workspace.generic_string()},
                              {"sha256", *digest}, {"destination", destination}}),
        tokmon::RiskClass::reversible);
    if (!exported) return tl::unexpected(exported.error());
    set("display", "会话已通过 Cove 导出到 " + (workspace / destination).generic_string());
  } else if (name == "context") {
    auto photons = argument(0) == "all" ? runtime.history_all(0) : runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::int64_t user = 0, assistant = 0, tools = 0, summaries = 0;
    for (const auto& photon : *photons) {
      user += photon.kind == "user.input"; assistant += photon.kind == "assistant.message";
      tools += photon.kind == "tool.result"; summaries += photon.kind == "summary.created";
    }
    std::ostringstream output;
    output << "上下文投影\n光子: " << photons->size() << "\n用户输入: " << user
           << "\n助手回复: " << assistant << "\n工具结果: " << tools
           << "\n压缩摘要: " << summaries << "\n窗口上限: "
           << runtime.config().photon_window;
    set("display", output.str());
  } else if (name == "compact") {
    auto compacted = invoke_lens(runtime, active_ray, "text.compact",
        "tokmon.text.compact.v1", "org.tokmon.lens.textus",
        tokmon::cbor::object({{"focus", parsed->raw_arguments}, {"max_chars", 4096}}));
    if (!compacted) return tl::unexpected(compacted.error());
    auto photons = runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    const tokmon::Photon* summary = nullptr;
    for (auto iterator = photons->rbegin(); iterator != photons->rend(); ++iterator)
      if (iterator->kind == "summary.created") { summary = &*iterator; break; }
    set("display", summary == nullptr ? "Textus 已完成压缩。" : photon_text(*summary));
  } else if (name == "model") {
    const auto selected = argument(0);
    if (selected.empty()) {
      std::ostringstream output;
      output << "当前平台: " << runtime.config().default_model_provider << "\n可用平台:";
      for (const auto& [id, provider] : runtime.config().model_providers)
        if (provider.enabled) output << "\n  " << id << " → " << provider.model;
      set("display", output.str());
    } else {
      const auto found = runtime.config().model_providers.find(selected);
      if (found == runtime.config().model_providers.end() || !found->second.enabled)
        return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::not_found,
                                                 "model provider is unavailable"));
      const auto model_name = found->second.model;
      auto saved = select_project_model_provider(
          runtime.config().paths.project / "config.yaml", selected);
      if (!saved) return tl::unexpected(saved.error());
      auto reconciled = runtime.reconcile();
      if (!reconciled) return tl::unexpected(reconciled.error());
      set("provider", selected); set("model", model_name);
      set("display", "当前模型平台已切换为 " + selected + "（" + model_name + "）。");
    }
  } else if (name == "effort") {
    const auto value = argument(0);
    if (!value.empty() && value != "low" && value != "medium" && value != "high" && value != "max")
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
          "effort must be low, medium, high or max"));
    if (!value.empty()) set("effort", value);
    set("display", value.empty() ? "用法：/effort low|medium|high|max" : "推理强度已设为 " + value + "。");
  } else if (name == "permissions") {
    const auto value = argument(0);
    if (!value.empty() && value != "full" && value != "restricted" && value != "read-only")
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
          "permissions must be full, restricted or read-only"));
    if (!value.empty()) set("access_mode", value);
    set("display", value.empty() ? "用法：/permissions full|restricted|read-only" :
        "当前会话访问模式已设为 " + value + "；Fallen 仍会独立裁决每个 Act。");
  } else if (name == "config") {
    set("open_settings", true);
    set("display", "项目配置: " + (runtime.config().paths.project / "config.yaml").generic_string() +
        "\n用户配置: " + (runtime.config().paths.user / "config.yaml").generic_string());
  } else if (name == "usage") {
    auto photons = history_for(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::int64_t input = 0, output_tokens = 0, cost = 0, calls = 0;
    for (const auto& photon : *photons) if (photon.kind == "model.usage") {
      ++calls;
      if (const auto* value = tokmon::cbor::find(photon.payload, "input_tokens")) input += value->as_integer();
      if (const auto* value = tokmon::cbor::find(photon.payload, "output_tokens")) output_tokens += value->as_integer();
      if (const auto* value = tokmon::cbor::find(photon.payload, "cost_microunits")) cost += value->as_integer();
    }
    std::ostringstream output;
    output << "模型调用: " << calls << "\n输入 tokens: " << input
           << "\n输出 tokens: " << output_tokens << "\n成本(微单位): " << cost
           << "\n光子数: " << photons->size();
    set("display", output.str());
  } else if (name == "plan" || name == "review" || name == "security-review" || name == "debug") {
    std::string prompt;
    if (name == "plan") prompt = "制定并执行一个可审计的计划：" + parsed->raw_arguments;
    else if (name == "review") prompt = "审查以下目标，给出具体问题和修改建议：" +
        (parsed->raw_arguments.empty() ? std::string("当前工作区") : parsed->raw_arguments);
    else if (name == "security-review") prompt = "对以下目标执行安全审查，按风险分级并给出证据：" +
        (parsed->raw_arguments.empty() ? std::string("当前工作区") : parsed->raw_arguments);
    else prompt = "诊断以下问题并使用可用透镜收集证据：" +
        (parsed->raw_arguments.empty() ? std::string("当前会话异常") : parsed->raw_arguments);
    if (name == "plan" && parsed->raw_arguments.empty())
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                               "/plan requires a task"));
    auto context = resolved_model_context(runtime.config(), request_payload);
    if (!context) return tl::unexpected(context.error());
    auto ray = active_ray.empty() ? runtime.submit(prompt, *context)
                                  : runtime.submit_to(active_ray, prompt, *context);
    if (!ray) return tl::unexpected(ray.error());
    active_ray = *ray;
    auto advanced = runtime.advance(active_ray);
    if (!advanced) return tl::unexpected(advanced.error());
    auto photons = runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::string answer = "命令已沿光路完成。";
    for (auto iterator = photons->rbegin(); iterator != photons->rend(); ++iterator)
      if (iterator->kind == "assistant.message") { answer = photon_text(*iterator); break; }
    set("ray", active_ray); set("display", answer); set("photons", photon_array(*photons));
  } else if (name == "tasks" || name == "agents") {
    auto photons = history_for(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::vector<tokmon::Photon> selected;
    for (const auto& photon : *photons) {
      const bool relevant = name == "agents"
          ? photon.kind.starts_with("child.")
          : photon.kind.starts_with("act.") || photon.kind.starts_with("tool.") ||
            photon.kind.starts_with("ray.");
      if (relevant) selected.push_back(photon);
    }
    set("display", selected.empty() ? (name == "agents" ? "当前没有子光线。" : "当前没有任务步骤。")
                                     : photon_lines(selected));
  } else if (name == "fork") {
    if (parsed->raw_arguments.empty()) return tl::unexpected(tokmon::make_error(
        tokmon::ErrorCode::invalid_argument, "/fork requires a task"));
    const auto parent_budget = static_cast<std::int64_t>(runtime.config().max_beats);
    auto forked = invoke_lens(runtime, active_ray, "child.spawn", "tokmon.child.spawn.v1",
        "org.tokmon.lens.aya", tokmon::cbor::object({
            {"task", parsed->raw_arguments}, {"parent_budget", parent_budget},
            {"budget", std::max<std::int64_t>(1, parent_budget / 2)},
            {"allowed_acts", tokmon::cbor::Value::Array{"model.request", "tool.call"}},
            {"workspace_mode", "read_only"}, {"workspace_root", runtime.config().paths.project.parent_path().generic_string()},
            {"join_policy", "manual"}, {"mode", "fork"}, {"deadline_ms", 30000}}));
    if (!forked) return tl::unexpected(forked.error());
    auto photons = runtime.history(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::string child;
    for (auto iterator = photons->rbegin(); iterator != photons->rend(); ++iterator)
      if (iterator->kind == "child.started") {
        if (const auto* value = tokmon::cbor::find(iterator->payload, "child_ray"))
          child = std::string(value->as_string());
        break;
      }
    set("child_ray", child);
    set("display", "Aya 已派生只读子光线 " + child + "；任务与预算均已写入光子。");
  } else if (name == "diff") {
    auto diff = invoke_lens(runtime, command_ray, "git.status", "tokmon.git.status.v1",
        "org.tokmon.lens.cove", tokmon::cbor::object({
            {"workspace_root", runtime.config().paths.project.parent_path().generic_string()}}));
    if (!diff) return tl::unexpected(diff.error());
    auto photons = runtime.history(command_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::string output = "Cove 已读取 Git 状态。";
    for (auto iterator = photons->rbegin(); iterator != photons->rend(); ++iterator)
      if (iterator->kind == "git.status-observed") { output = photon_text(*iterator); break; }
    set("display", output.empty() ? "工作区干净。" : output);
  } else if (name == "doctor") {
    auto verified = runtime.verify();
    if (!verified) return tl::unexpected(verified.error());
    set("display", "存储哈希链：通过\n光路：epoch " +
        std::to_string(runtime.light_path()->epoch) + "，" +
        std::to_string(runtime.light_path()->lenses.size()) + " 个透镜\nSnow：正常");
  } else if (name == "init") {
    auto initialized = append_fact(command_ray, "project.initialized",
        "tokmon.project.initialized.v1", tokmon::cbor::object({
            {"workspace", runtime.config().paths.project.parent_path().generic_string()},
            {"config_root", runtime.config().paths.project.generic_string()},
            {"existing_history_preserved", true}}));
    if (!initialized) return tl::unexpected(initialized.error());
    set("display", "项目约定已就绪：" + runtime.config().paths.project.generic_string());
  } else if (name == "lenses" || name == "lens") {
    const auto operation = argument(0).empty() ? "list" : argument(0);
    if (operation == "reconcile") {
      auto reconciled = runtime.reconcile();
      if (!reconciled) return tl::unexpected(reconciled.error());
    } else if (operation != "list") return tl::unexpected(tokmon::make_error(
        tokmon::ErrorCode::invalid_argument, "only list and reconcile are exposed as slash operations"));
    std::ostringstream output;
    output << "LightPath epoch " << runtime.light_path()->epoch << ':';
    for (const auto& mounted : runtime.light_path()->lenses)
      output << "\n  " << mounted.lens->manifest().id << " @ generation " << mounted.generation;
    set("display", output.str());
  } else if (name == "skills") {
    if (argument(0) == "discover") {
      tokmon::cbor::Value::Array roots{
          runtime.config().paths.project.parent_path().generic_string(),
          runtime.config().paths.user.generic_string()};
      auto discovered = invoke_lens(runtime, command_ray, "skill.discover",
          "tokmon.skill.discover.v1", "org.tokmon.lens.enso",
          tokmon::cbor::object({{"roots", std::move(roots)}}));
      if (!discovered) return tl::unexpected(discovered.error());
    }
    auto photons = history_for(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::vector<tokmon::Photon> skills;
    for (const auto& photon : *photons) if (photon.kind == "skill.discovered") skills.push_back(photon);
    set("display", skills.empty() ? "尚未发现技能；使用 /skills discover 扫描配置根。" : photon_lines(skills));
  } else if (name == "mcp" || name == "memory") {
    auto photons = history_for(active_ray);
    if (!photons) return tl::unexpected(photons.error());
    std::vector<tokmon::Photon> selected;
    for (const auto& photon : *photons) {
      const bool relevant = name == "mcp"
          ? photon.kind.starts_with("mcp.") || photon.kind.starts_with("connection.")
          : photon.kind == "memory.accepted" || photon.kind == "memory.invalidated";
      if (relevant) selected.push_back(photon);
    }
    set("display", selected.empty() ? (name == "mcp" ? "当前没有已提交的 MCP 连接事实。" :
        "当前没有已接受的记忆事实。") : photon_lines(selected));
  }

  auto completed = append_fact(command_ray, "command.completed", "tokmon.command.result.v1",
      tokmon::cbor::object({{"name", name}, {"invocation_photon", invoked->id},
        {"result", tokmon::cbor::find(response, "display")
            ? *tokmon::cbor::find(response, "display") : tokmon::cbor::Value("")},
        {"history_deleted", false}}));
  if (!completed) return tl::unexpected(completed.error());
  auto command_photons = runtime.history(command_ray);
  if (command_photons && !tokmon::cbor::find(response, "photons"))
    set("photons", photon_array(*command_photons));
  auto all = runtime.history_all(0);
  set("cursor", static_cast<std::int64_t>(all && !all->empty() ? all->back().sequence : 0));
  return response;
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
  struct ClientLease {
    std::string kind;
    bool shutdown_when_idle{true};
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::steady_clock::time_point expires_at;
  };
  std::mutex lifecycle_mutex;
  std::unordered_map<std::string, ClientLease> client_leases;
  std::optional<std::chrono::steady_clock::time_point> idle_shutdown_at;
  bool daemon_pinned = false;
  tokmon::SnowServer snow;
  const auto endpoint = endpoint_override.value_or(tokmon::workspace_snow_endpoint(
      runtime.config().paths.run, runtime.config().paths.project.parent_path()));
  DaemonInstanceLock instance_lock(endpoint);
  if (!instance_lock.acquired()) return 0;
  auto snow_started = snow.start(endpoint, [&runtime, &runtime_mutex, &request_mutex,
                                        &cancelled_requests, &completed_requests,
                                        &active_requests, &lifecycle_mutex,
                                        &client_leases, &idle_shutdown_at,
                                        &daemon_pinned, &endpoint, &snow](
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
    const auto* early_action_field = request.kind == tokmon::SnowMessageKind::intent
        ? tokmon::cbor::find(request.payload, "action") : nullptr;
    const auto early_action = early_action_field
        ? early_action_field->as_string() : std::string_view{};
    if (early_action == "daemon.client.attach" ||
        early_action == "daemon.client.heartbeat" ||
        early_action == "daemon.client.detach" || early_action == "daemon.pin") {
      const auto lifecycle_result = [&](tokmon::cbor::Value payload) {
        return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
            .request_id = request.request_id, .cursor = request.cursor,
            .payload = std::move(payload)};
      };
      if (early_action == "daemon.pin") {
        std::scoped_lock lifecycle_lock(lifecycle_mutex);
        daemon_pinned = true;
        idle_shutdown_at.reset();
        return lifecycle_result(tokmon::cbor::object({
            {"pinned", true}, {"ownership", "explicit-user-service"}}));
      }
      const auto* id_field = tokmon::cbor::find(request.payload, "client_id");
      const auto client_id = id_field
          ? std::string(id_field->as_string()) : std::string{};
      if (client_id.empty())
        return snow_error(request, tokmon::make_error(
            tokmon::ErrorCode::invalid_argument, "daemon client_id is required"));
      const auto now = std::chrono::steady_clock::now();
      const auto ttl_field = tokmon::cbor::find(request.payload, "lease_ttl_ms");
      const auto ttl_ms = std::clamp<std::int64_t>(
          ttl_field ? ttl_field->as_integer() : 6'000, 2'000, 30'000);
      std::scoped_lock lifecycle_lock(lifecycle_mutex);
      if (!running.load(std::memory_order_acquire))
        return snow_error(request, tokmon::make_error(
            tokmon::ErrorCode::invalid_state, "tokmond is already stopping"));
      if (early_action == "daemon.client.attach") {
        const auto* kind_field = tokmon::cbor::find(request.payload, "client_kind");
        const auto kind = kind_field
            ? std::string(kind_field->as_string()) : std::string{};
        if (kind.empty())
          return snow_error(request, tokmon::make_error(
              tokmon::ErrorCode::invalid_argument, "daemon client_kind is required"));
        const auto* shutdown_field = tokmon::cbor::find(
            request.payload, "shutdown_when_idle");
        const auto idle_field = tokmon::cbor::find(request.payload, "idle_timeout_ms");
        const auto idle_ms = std::clamp<std::int64_t>(
            idle_field ? idle_field->as_integer() : 15'000, 0, 300'000);
        client_leases[client_id] = ClientLease{
            .kind = kind,
            .shutdown_when_idle = !shutdown_field || shutdown_field->as_bool(),
            .idle_timeout = std::chrono::milliseconds(idle_ms),
            .expires_at = now + std::chrono::milliseconds(ttl_ms)};
        return lifecycle_result(tokmon::cbor::object({
            {"attached", true}, {"client_id", client_id},
            {"client_kind", kind},
            {"lease_ttl_ms", ttl_ms},
            {"active_clients", static_cast<std::int64_t>(client_leases.size())}}));
      }
      const auto found = client_leases.find(client_id);
      if (found == client_leases.end() &&
          early_action == "daemon.client.heartbeat")
        return snow_error(request, tokmon::make_error(
            tokmon::ErrorCode::not_found, "daemon client lease is not active"));
      if (early_action == "daemon.client.heartbeat") {
        found->second.expires_at = now + std::chrono::milliseconds(ttl_ms);
        return lifecycle_result(tokmon::cbor::object({
            {"renewed", true}, {"client_id", client_id},
            {"lease_ttl_ms", ttl_ms}}));
      }
      const auto* shutdown_field = tokmon::cbor::find(
          request.payload, "shutdown_when_idle");
      const auto idle_field = tokmon::cbor::find(request.payload, "idle_timeout_ms");
      const auto shutdown_when_idle = found != client_leases.end()
          ? found->second.shutdown_when_idle
          : shutdown_field && shutdown_field->as_bool();
      const auto idle_timeout = found != client_leases.end()
          ? found->second.idle_timeout
          : std::chrono::milliseconds(std::clamp<std::int64_t>(
              idle_field ? idle_field->as_integer() : 15'000, 0, 300'000));
      if (found != client_leases.end()) client_leases.erase(found);
      if (shutdown_when_idle && !daemon_pinned) {
        const auto requested = now + idle_timeout;
        if (!idle_shutdown_at || requested > *idle_shutdown_at)
          idle_shutdown_at = requested;
      }
      return lifecycle_result(tokmon::cbor::object({
          {"detached", true}, {"client_id", client_id},
          {"shutdown_when_idle", shutdown_when_idle && !daemon_pinned},
          {"active_clients", static_cast<std::int64_t>(client_leases.size())}}));
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
                                           {"ownership", "explicit-user-stop"}})});
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
    if (action == "command.execute") {
      auto command_payload = request.payload;
      if (!command_payload.as_map()) command_payload = tokmon::cbor::Value::Map{};
      (*command_payload.as_map())["snow_request_id"] = std::to_string(request.request_id);
      auto result = execute_slash_command(runtime, command_payload);
      if (!result) {
        const auto* ray_field = tokmon::cbor::find(command_payload, "ray");
        const auto failure_ray = ray_field && !ray_field->as_string().empty()
            ? std::string(ray_field->as_string())
            : "command-ray-" + std::to_string(request.request_id);
        (void)runtime.store().append(tokmon::PhotonDraft{.ray = failure_ray,
            .kind = "command.failed", .schema = "tokmon.command.result.v1",
            .payload = tokmon::cbor::object({{"input", tokmon::cbor::find(request.payload, "text")
                ? *tokmon::cbor::find(request.payload, "text") : tokmon::cbor::Value("")},
                {"error", result.error().describe()}, {"history_deleted", false}}),
            .epoch = runtime.light_path()->epoch});
        return snow_error(request, result.error());
      }
      const auto* cursor = tokmon::cbor::find(*result, "cursor");
      return remember(tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id,
          .cursor = cursor ? static_cast<std::uint64_t>(cursor->as_integer()) : request.cursor,
          .payload = std::move(*result)});
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
    bool idle_stop_candidate = false;
    {
      std::scoped_lock lifecycle_lock(lifecycle_mutex);
      const auto now = std::chrono::steady_clock::now();
      for (auto iterator = client_leases.begin(); iterator != client_leases.end();) {
        if (iterator->second.expires_at > now) {
          ++iterator;
          continue;
        }
        if (iterator->second.shutdown_when_idle && !daemon_pinned) {
          const auto requested = now + iterator->second.idle_timeout;
          if (!idle_shutdown_at || requested > *idle_shutdown_at)
            idle_shutdown_at = requested;
        }
        spdlog::warn("expired {} daemon client lease {}",
                     iterator->second.kind, iterator->first);
        iterator = client_leases.erase(iterator);
      }
      idle_stop_candidate = !daemon_pinned && client_leases.empty() &&
          idle_shutdown_at && now >= *idle_shutdown_at;
    }
    if (idle_stop_candidate) {
      std::unique_lock runtime_lock(runtime_mutex, std::try_to_lock);
      if (runtime_lock.owns_lock()) {
        std::scoped_lock lifecycle_lock(lifecycle_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!daemon_pinned && client_leases.empty() && idle_shutdown_at &&
            now >= *idle_shutdown_at) {
          spdlog::info("no client leases or active work remain; stopping tokmond");
          running.store(false, std::memory_order_release);
        }
      }
    }
    if (!running.load(std::memory_order_acquire)) break;
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
