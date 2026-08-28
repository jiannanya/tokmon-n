#include "tokmon/config.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <unordered_map>

#include "tokmon/builtin_lens.hpp"
#include "tokmon/yaml.hpp"

namespace tokmon {
namespace {

std::filesystem::path home_directory() {
#if defined(_WIN32)
  if (const auto* profile = std::getenv("USERPROFILE")) return profile;
  const auto* drive = std::getenv("HOMEDRIVE");
  const auto* path = std::getenv("HOMEPATH");
  if (drive && path) return std::string(drive) + path;
#else
  if (const auto* home = std::getenv("HOME")) return home;
#endif
  return std::filesystem::current_path();
}

Result<void> reject_unknown(const cbor::Value& map, const std::set<std::string>& allowed,
                            const std::filesystem::path& source) {
  if (!map.as_map()) return {};
  for (const auto& [key, value] : *map.as_map()) {
    (void)value;
    if (!allowed.contains(key))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "unknown YAML field '" + key + "' in " + source.string()));
  }
  return {};
}

Result<RuntimeKind> parse_runtime(const std::string& text) {
  if (text == "in_process") return RuntimeKind::in_process;
  if (text == "native_worker") return RuntimeKind::native_worker;
  if (text == "node") return RuntimeKind::node;
  if (text == "cpython") return RuntimeKind::cpython;
  if (text == "wasm") return RuntimeKind::wasm;
  if (text == "desktop") return RuntimeKind::desktop;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown Lens runtime: " + text));
}

Result<PolicyEffect> parse_policy_effect(const std::string& text) {
  if (text == "allow") return PolicyEffect::allow;
  if (text == "ask") return PolicyEffect::ask;
  if (text == "deny") return PolicyEffect::deny;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown Fallen policy effect: " + text));
}

Result<std::vector<std::string>> string_sequence(const cbor::Value* node,
                                                 const std::string_view field,
                                                 const std::filesystem::path& source) {
  std::vector<std::string> values;
  if (!node) return values;
  if (!node->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": fallen." + std::string(field) + " must be a sequence"));
  for (const auto& value : *node->as_array()) {
    if (!std::holds_alternative<std::string>(value.data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": fallen." + std::string(field) +
              " entries must be strings"));
    values.emplace_back(value.as_string());
  }
  return values;
}

template <typename Type>
Result<void> require_type(const cbor::Value& map, const std::string_view key,
                          const std::string_view expected,
                          const std::filesystem::path& source) {
  const auto* value = cbor::find(map, key);
  if (value && !std::holds_alternative<Type>(value->data))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": " + std::string(key) + " must be " +
            std::string(expected)));
  return {};
}

Result<void> parse_fallen_policy(FallenPolicy& policy, const cbor::Value& fallen,
                                 const std::filesystem::path& source) {
  if (!fallen.as_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": fallen must be a map"));
  if (auto result = reject_unknown(fallen, {"defaults", "rules", "approvals"}, source);
      !result) return result;
  policy.configured = true;
  if (const auto* defaults = cbor::find(fallen, "defaults")) {
    auto effect = parse_policy_effect(std::string(defaults->as_string()));
    if (!effect) return tl::unexpected(effect.error());
    policy.default_effect = *effect;
  }
  if (const auto* rules = cbor::find(fallen, "rules")) {
    if (!rules->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": fallen.rules must be a sequence"));
    policy.rules.clear();
    for (const auto& encoded : *rules->as_array()) {
      if (!encoded.as_map())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         source.string() + ": Fallen rule must be a map"));
      if (auto result = reject_unknown(encoded,
          {"effect", "acts", "targets", "trusts", "risks", "paths", "argv0", "parameters",
           "not_before_ms", "not_after_ms"}, source); !result) return result;
      if (auto result = require_type<std::string>(encoded, "effect", "a string", source);
          !result) return result;
      for (const auto* key : {"not_before_ms", "not_after_ms"})
        if (auto result = require_type<std::int64_t>(encoded, key, "an integer", source);
            !result) return result;
      const auto* encoded_effect = cbor::find(encoded, "effect");
      if (!encoded_effect)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         source.string() + ": Fallen rule effect is required"));
      auto effect = parse_policy_effect(std::string(encoded_effect->as_string()));
      if (!effect) return tl::unexpected(effect.error());
      PolicyRule rule; rule.effect = *effect;
      auto acts = string_sequence(cbor::find(encoded, "acts"), "rules.acts", source);
      auto targets = string_sequence(cbor::find(encoded, "targets"), "rules.targets", source);
      auto trusts = string_sequence(cbor::find(encoded, "trusts"), "rules.trusts", source);
      auto risks = string_sequence(cbor::find(encoded, "risks"), "rules.risks", source);
      auto paths = string_sequence(cbor::find(encoded, "paths"), "rules.paths", source);
      auto argv0 = string_sequence(cbor::find(encoded, "argv0"), "rules.argv0", source);
      if (!acts) return tl::unexpected(acts.error());
      if (!targets) return tl::unexpected(targets.error());
      if (!trusts) return tl::unexpected(trusts.error());
      if (!risks) return tl::unexpected(risks.error());
      if (!paths) return tl::unexpected(paths.error());
      if (!argv0) return tl::unexpected(argv0.error());
      rule.acts = std::move(*acts); rule.targets = std::move(*targets);
      rule.trusts = std::move(*trusts);
      rule.risks = std::move(*risks); rule.paths = std::move(*paths);
      rule.argv0 = std::move(*argv0);
      if (const auto* parameters = cbor::find(encoded, "parameters")) {
        if (!parameters->as_map())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              source.string() + ": Fallen rule parameters must be a map"));
        for (const auto& [key, value] : *parameters->as_map())
          rule.parameters.emplace(key, std::string(value.as_string()));
      }
      if (const auto* value = cbor::find(encoded, "not_before_ms"))
        rule.not_before_ms = value->as_integer();
      if (const auto* value = cbor::find(encoded, "not_after_ms"))
        rule.not_after_ms = value->as_integer();
      if (rule.not_before_ms > 0 && rule.not_after_ms > 0 &&
          rule.not_before_ms > rule.not_after_ms)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            source.string() + ": Fallen rule time window is inverted"));
      policy.rules.push_back(std::move(rule));
    }
  }
  if (const auto* approvals = cbor::find(fallen, "approvals")) {
    if (!approvals->as_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": fallen.approvals must be a map"));
    policy.approval_risks.clear();
    for (const auto& [risk, encoded] : *approvals->as_map()) {
      auto principals = string_sequence(&encoded, "approvals." + risk, source);
      if (!principals) return tl::unexpected(principals.error());
      if (!principals->empty()) policy.approval_risks.push_back(risk);
    }
  }
  return {};
}

bool loopback_endpoint(const std::string_view endpoint) {
  return endpoint.starts_with("http://127.0.0.1") ||
      endpoint.starts_with("http://localhost") || endpoint.starts_with("http://[::1]");
}

Result<RuntimeProfile> parse_runtime_profile(const std::string_view text) {
  if (text == "production") return RuntimeProfile::production;
  if (text == "development") return RuntimeProfile::development;
  if (text == "test") return RuntimeProfile::test;
  return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                   "unknown runtime profile: " + std::string(text)));
}

const std::set<std::string>& fixed_model_provider_fields() {
  static const std::set<std::string> fields{
      "protocol", "endpoint", "model", "secret_ref", "secret_env", "auth", "enabled",
      "allow_anonymous", "thinking", "reasoning_effort", "max_output_tokens",
      "max_attempts", "retry_backoff_ms", "first_token_timeout_ms", "idle_timeout_ms",
      "request_parameters"};
  return fields;
}

Result<void> merge_model_request_parameter(
    ModelProviderConfig& provider, const std::string& provider_id,
    const std::string& key, const cbor::Value& value,
    const std::filesystem::path& source) {
  static const std::set<std::string> protected_fields{
      "api_key", "secret", "secret_value", "secret_binding", "secret_purpose",
      "authorization", "provider", "protocol", "endpoint", "model", "messages",
      "contents", "tools", "prompt", "stream", "stream_options", "thinking",
      "reasoning_effort", "max_tokens", "max_output_tokens", "request_body", "fallbacks",
      "workspace_root", "access_mode", "effort", "idempotency_key"};
  if (key.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": model request parameter names cannot be empty"));
  if (protected_fields.contains(key))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        source.string() + ": model request parameter '" + key +
        "' is controlled by Tokmon and cannot be configured for provider " + provider_id));
  if (!provider.request_parameters.as_map())
    provider.request_parameters = cbor::Value::Map{};
  (*provider.request_parameters.as_map())[key] = value;
  return {};
}

Result<void> parse_model_providers(RuntimeConfig& config, const cbor::Value& models,
                                   const std::filesystem::path& source) {
  if (!models.as_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": models must be a map"));
  if (auto result = reject_unknown(models, {"default", "providers"}, source); !result)
    return result;
  if (auto result = require_type<std::string>(models, "default", "a string", source);
      !result) return result;
  if (const auto* selected = cbor::find(models, "default"))
    config.default_model_provider = std::string(selected->as_string());
  const auto* providers = cbor::find(models, "providers");
  if (!providers) return {};
  if (!providers->as_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": models.providers must be a map"));
  static const std::regex id_pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
  static const std::set<std::string> protocols{
      "local", "openai-compatible", "anthropic", "gemini"};
  static const std::set<std::string> auth_modes{
      "protocol-default", "bearer", "x-api-key", "x-goog-api-key", "none"};
  for (const auto& [id, value] : *providers->as_map()) {
    if (!std::regex_match(id, id_pattern) || !value.as_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": invalid model provider id or definition: " + id));
    for (const auto* key : {"protocol", "endpoint", "model", "secret_ref", "secret_env",
                            "auth", "reasoning_effort"})
      if (auto result = require_type<std::string>(value, key, "a string", source); !result)
        return result;
    for (const auto* key : {"enabled", "allow_anonymous", "thinking"})
      if (auto result = require_type<bool>(value, key, "a boolean", source); !result)
        return result;
    for (const auto* key : {"max_output_tokens", "max_attempts", "retry_backoff_ms",
                            "first_token_timeout_ms", "idle_timeout_ms"})
      if (auto result = require_type<std::int64_t>(value, key, "an integer", source); !result)
        return result;
    if (const auto* parameters = cbor::find(value, "request_parameters");
        parameters && !parameters->as_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": request_parameters must be a map for provider " + id));
    ModelProviderConfig provider;
    if (const auto found = config.model_providers.find(id);
        found != config.model_providers.end()) provider = found->second;
    provider.id = id;
    const auto read_string = [&](const char* key, std::string& output) {
      if (const auto* field = cbor::find(value, key))
        output = std::string(field->as_string());
    };
    read_string("protocol", provider.protocol);
    read_string("endpoint", provider.endpoint);
    read_string("model", provider.model);
    read_string("secret_ref", provider.secret_ref);
    read_string("secret_env", provider.secret_env);
    read_string("auth", provider.auth);
    read_string("reasoning_effort", provider.reasoning_effort);
    if (const auto* field = cbor::find(value, "enabled")) provider.enabled = field->as_bool();
    if (const auto* field = cbor::find(value, "allow_anonymous"))
      provider.allow_anonymous = field->as_bool();
    if (const auto* field = cbor::find(value, "thinking"))
      provider.thinking = field->as_bool();
    if (const auto* field = cbor::find(value, "max_output_tokens"))
      provider.max_output_tokens = field->as_integer();
    if (const auto* field = cbor::find(value, "max_attempts"))
      provider.max_attempts = field->as_integer();
    if (const auto* field = cbor::find(value, "retry_backoff_ms"))
      provider.retry_backoff_ms = field->as_integer();
    if (const auto* field = cbor::find(value, "first_token_timeout_ms"))
      provider.first_token_timeout_ms = field->as_integer();
    if (const auto* field = cbor::find(value, "idle_timeout_ms"))
      provider.idle_timeout_ms = field->as_integer();
    std::set<std::string> explicitly_nested;
    if (const auto* parameters = cbor::find(value, "request_parameters")) {
      for (const auto& [key, parameter] : *parameters->as_map()) {
        auto merged = merge_model_request_parameter(provider, id, key, parameter, source);
        if (!merged) return merged;
        explicitly_nested.insert(key);
      }
    }
    for (const auto& [key, parameter] : *value.as_map()) {
      if (fixed_model_provider_fields().contains(key)) continue;
      if (explicitly_nested.contains(key))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            source.string() + ": model request parameter '" + key +
            "' is declared both directly and under request_parameters for provider " + id));
      auto merged = merge_model_request_parameter(provider, id, key, parameter, source);
      if (!merged) return merged;
    }
    if (!protocols.contains(provider.protocol))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": unsupported model protocol for " + id));
    if (!auth_modes.contains(provider.auth))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": unsupported auth mode for " + id));
    if (provider.model.empty() || (provider.protocol != "local" && provider.endpoint.empty()))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": model and endpoint are required for provider " + id));
    if (provider.protocol != "local" && !provider.endpoint.starts_with("https://") &&
        !loopback_endpoint(provider.endpoint))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          source.string() + ": provider endpoint must use HTTPS or loopback HTTP"));
    if (provider.allow_anonymous && !loopback_endpoint(provider.endpoint) &&
        provider.auth != "none")
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          source.string() + ": anonymous remote providers must explicitly use auth: none"));
    if (provider.auth != "none" && provider.protocol != "local" &&
        !provider.allow_anonymous && provider.secret_ref.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": secret_ref is required for provider " + id));
    if (provider.protocol != "local" && provider.auth != "none" &&
        provider.secret_ref != "model-provider/" + id)
      return tl::unexpected(make_error(ErrorCode::permission_denied,
          source.string() + ": provider SecretRef must be scoped to its own id"));
    if (provider.secret_ref.find('\0') != std::string::npos || provider.secret_ref.size() > 240)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": invalid SecretRef for provider " + id));
    static const std::regex environment_name("^[A-Z_][A-Z0-9_]{0,127}$");
    if (!provider.secret_env.empty() &&
        (!std::regex_match(provider.secret_env, environment_name) ||
         provider.secret_ref.empty()))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": invalid secret_env for provider " + id));
    if (provider.max_output_tokens <= 0 || provider.max_output_tokens > 1'000'000 ||
        provider.max_attempts <= 0 || provider.max_attempts > 10 ||
        provider.retry_backoff_ms < 0 || provider.retry_backoff_ms > 60'000 ||
        provider.first_token_timeout_ms <= 0 || provider.first_token_timeout_ms > 600'000 ||
        provider.idle_timeout_ms <= 0 || provider.idle_timeout_ms > 600'000)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
          source.string() + ": provider retry/token/timeout limits are outside the allowed range"));
    config.model_providers[id] = std::move(provider);
  }
  return {};
}

Result<void> merge_config_file(RuntimeConfig& config, const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  auto loaded = yaml::load(source);
  if (!loaded) return tl::unexpected(loaded.error());
  const auto& root = *loaded;
  if (!root.is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + " must contain a YAML map"));
  if (auto result = reject_unknown(root,
      {"profile", "logging", "engine", "security", "ui", "fallen", "models"}, source);
      !result) return result;
  if (auto result = require_type<std::string>(root, "profile", "a string", source);
      !result) return result;
  if (const auto* profile = cbor::find(root, "profile")) {
    auto parsed = parse_runtime_profile(profile->as_string());
    if (!parsed) return tl::unexpected(parsed.error());
    config.profile = *parsed;
  }
  if (const auto* logging = cbor::find(root, "logging")) {
    if (!logging->is_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": logging must be a map"));
    if (auto result = reject_unknown(*logging, {"level"}, source); !result) return result;
    if (auto result = require_type<std::string>(*logging, "level", "a string", source);
        !result) return result;
    if (const auto* level = cbor::find(*logging, "level"))
      config.log_level = std::string(level->as_string());
  }
  if (const auto* engine = cbor::find(root, "engine")) {
    if (!engine->is_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": engine must be a map"));
    if (auto result = reject_unknown(*engine, {"photon_window", "max_beats"}, source);
        !result) return result;
    for (const auto* key : {"photon_window", "max_beats"})
      if (auto result = require_type<std::int64_t>(*engine, key, "an integer", source);
          !result) return result;
    if (const auto* field = cbor::find(*engine, "photon_window")) {
      if (field->as_integer() <= 0)
        return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                         "engine limits must be positive"));
      config.photon_window = static_cast<std::size_t>(field->as_integer());
    }
    if (const auto* field = cbor::find(*engine, "max_beats")) {
      if (field->as_integer() <= 0)
        return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                         "engine limits must be positive"));
      config.max_beats = static_cast<std::size_t>(field->as_integer());
    }
  }
  if (const auto* security = cbor::find(root, "security")) {
    if (!security->is_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": security must be a map"));
    if (auto result = reject_unknown(*security,
        {"require_signatures", "trusted_signers"}, source); !result) return result;
    if (auto result = require_type<bool>(*security, "require_signatures", "a boolean", source);
        !result) return result;
    if (const auto* field = cbor::find(*security, "require_signatures"))
      config.require_signatures = field->as_bool();
    if (const auto* signers = cbor::find(*security, "trusted_signers")) {
      if (!signers->as_map())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            source.string() + ": security.trusted_signers must be a map"));
      for (const auto& [signer, encoded] : *signers->as_map()) {
        if (!std::holds_alternative<std::string>(encoded.data))
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              source.string() + ": trusted signer SecretRefs must be strings"));
        const auto secret_ref = std::string(encoded.as_string());
        if (signer.empty() || secret_ref.empty())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              source.string() + ": trusted signer names and SecretRefs cannot be empty"));
        config.trusted_signers[signer] = secret_ref;
      }
    }
  }
  if (const auto* fallen = cbor::find(root, "fallen")) {
    const auto project_file = source.parent_path().filename() == ".tokmon" &&
                              source.parent_path() == config.paths.project;
    auto result = parse_fallen_policy(project_file ? config.project_policy : config.user_policy,
                                      *fallen, source);
    if (!result) return result;
  }
  if (const auto* models = cbor::find(root, "models")) {
    auto result = parse_model_providers(config, *models, source);
    if (!result) return result;
  }
  if (config.photon_window == 0 || config.photon_window > 100'000 ||
      config.max_beats == 0 || config.max_beats > 1024)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "engine limits are outside the allowed range"));
  return {};
}

Result<void> parse_optical_budget(const cbor::Value* encoded, OpticalBudget& budget,
                                  const std::filesystem::path& source) {
  if (!encoded) return {};
  if (auto checked = reject_unknown(*encoded,
      {"max_cells", "max_bytes", "max_cell_bytes", "max_lens_executions",
       "max_rounds", "deadline_ms"}, source); !checked)
    return checked;
  const auto positive = [&](const std::string_view name) -> Result<std::int64_t> {
    const auto* field = cbor::find(*encoded, name);
    if (!field) return std::int64_t{0};
    if (!std::holds_alternative<std::int64_t>(field->data) || field->as_integer() <= 0)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "optical budget values must be positive integers"));
    return field->as_integer();
  };
  auto max_cells = positive("max_cells");
  auto max_bytes = positive("max_bytes");
  auto max_cell_bytes = positive("max_cell_bytes");
  auto max_executions = positive("max_lens_executions");
  auto max_rounds = positive("max_rounds");
  auto deadline = positive("deadline_ms");
  for (const auto* value : {&max_cells, &max_bytes, &max_cell_bytes,
                            &max_executions, &max_rounds, &deadline})
    if (!*value) return tl::unexpected(value->error());
  if (*max_cells) budget.max_cells = static_cast<std::size_t>(*max_cells);
  if (*max_bytes) budget.max_bytes = static_cast<std::size_t>(*max_bytes);
  if (*max_cell_bytes) budget.max_cell_bytes = static_cast<std::size_t>(*max_cell_bytes);
  if (*max_executions)
    budget.max_lens_executions = static_cast<std::size_t>(*max_executions);
  if (*max_rounds) budget.max_rounds = static_cast<std::uint32_t>(*max_rounds);
  if (*deadline) budget.deadline = std::chrono::milliseconds(*deadline);
  return {};
}

Result<OpticalEndpoint> parse_optical_endpoint(
    const cbor::Value* value, const std::filesystem::path& source) {
  if (!value || !value->is_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "optical endpoint must be a map"));
  if (auto checked = reject_unknown(*value, {"lens", "port"}, source); !checked)
    return tl::unexpected(checked.error());
  const auto* lens = cbor::find(*value, "lens");
  const auto* port = cbor::find(*value, "port");
  if (!lens || !port || lens->as_string().empty() || port->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "optical endpoint requires lens and port"));
  return OpticalEndpoint{std::string(lens->as_string()),
                         std::string(port->as_string())};
}

Result<void> merge_light_path(RuntimeConfig& config,
                              const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  auto loaded = yaml::load(source);
  if (!loaded) return tl::unexpected(loaded.error());
  const auto& root = *loaded;
  if (auto result = reject_unknown(root, {"api", "lenses", "assembly"}, source); !result)
    return result;
  const auto* api = cbor::find(root, "api");
  if (!api || api->as_string() != "tokmon.light-path/wavefront")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": unsupported light-path api"));
  const auto* entries = cbor::find(root, "lenses");
  if (!entries || !entries->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": lenses must be a sequence"));
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < config.light_path.size(); ++i)
    index[config.light_path[i].id] = i;
  for (const auto& entry : *entries->as_array()) {
    if (!entry.is_map())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": Lens entry must be a map"));
    if (auto result = reject_unknown(entry, {"id", "artifact", "enabled", "runtime"}, source);
        !result) return result;
    if (auto result = require_type<std::string>(entry, "id", "a string", source); !result)
      return result;
    if (auto result = require_type<std::string>(entry, "artifact", "a string", source);
        !result) return result;
    if (auto result = require_type<bool>(entry, "enabled", "a boolean", source); !result)
      return result;
    if (auto result = require_type<std::string>(entry, "runtime", "a string", source);
        !result) return result;
    DesiredLens lens;
    const auto* id = cbor::find(entry, "id");
    const auto* artifact_field = cbor::find(entry, "artifact");
    const auto* enabled = cbor::find(entry, "enabled");
    const auto* runtime_field = cbor::find(entry, "runtime");
    lens.id = id ? std::string(id->as_string()) : std::string{};
    if (lens.id.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": Lens id cannot be empty"));
    auto builtin_name = lens.id;
    constexpr std::string_view official_prefix = "org.tokmon.lens.";
    if (builtin_name.starts_with(official_prefix))
      builtin_name.erase(0, official_prefix.size());
    lens.artifact = artifact_field ? std::string(artifact_field->as_string())
                                   : "builtin:" + builtin_name;
    if (!lens.artifact.starts_with("builtin:")) {
      auto artifact = std::filesystem::path(lens.artifact);
      if (artifact.is_relative())
        lens.artifact = (source.parent_path() / artifact).lexically_normal().string();
    }
    lens.enabled = !enabled || enabled->as_bool();
    auto runtime = parse_runtime(runtime_field ? std::string(runtime_field->as_string())
                                               : "in_process");
    if (!runtime) return tl::unexpected(runtime.error());
    lens.runtime = *runtime;
    if (const auto found = index.find(lens.id); found != index.end())
      config.light_path[found->second] = lens;
    else {
      index[lens.id] = config.light_path.size();
      config.light_path.push_back(std::move(lens));
    }
  }
  if (const auto* assembly = cbor::find(root, "assembly")) {
    if (auto result = reject_unknown(*assembly,
        {"id", "autowire_unique", "connections", "resonators", "budget"}, source);
        !result) return result;
    OpticalAssemblySpec parsed;
    if (const auto* id = cbor::find(*assembly, "id")) parsed.id = std::string(id->as_string());
    if (const auto* autowire = cbor::find(*assembly, "autowire_unique")) {
      if (!std::holds_alternative<bool>(autowire->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "assembly.autowire_unique must be boolean"));
      parsed.autowire_unique = autowire->as_bool();
    }
    if (auto budget = parse_optical_budget(cbor::find(*assembly, "budget"),
                                           parsed.budget, source); !budget)
      return budget;
    if (const auto* connections = cbor::find(*assembly, "connections")) {
      if (!connections->as_array())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "assembly.connections must be a sequence"));
      for (const auto& item : *connections->as_array()) {
        if (auto checked = reject_unknown(item, {"from", "to"}, source); !checked)
          return checked;
        auto from = parse_optical_endpoint(cbor::find(item, "from"), source);
        auto to = parse_optical_endpoint(cbor::find(item, "to"), source);
        if (!from) return tl::unexpected(from.error());
        if (!to) return tl::unexpected(to.error());
        parsed.connections.push_back({std::move(*from), std::move(*to)});
      }
    }
    if (const auto* resonators = cbor::find(*assembly, "resonators")) {
      if (!resonators->as_array())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "assembly.resonators must be a sequence"));
      for (const auto& item : *resonators->as_array()) {
        if (auto checked = reject_unknown(item, {"id", "lenses", "budget"}, source);
            !checked) return checked;
        ResonatorSpec resonator;
        if (const auto* id = cbor::find(item, "id")) resonator.id = std::string(id->as_string());
        const auto* members = cbor::find(item, "lenses");
        if (resonator.id.empty() || !members || !members->as_array())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "resonator requires id and lenses"));
        for (const auto& member : *members->as_array())
          resonator.lenses.emplace_back(member.as_string());
        if (auto budget = parse_optical_budget(cbor::find(item, "budget"),
                                               resonator.budget, source); !budget)
          return budget;
        parsed.resonators.push_back(std::move(resonator));
      }
    }
    config.optical_assembly = std::move(parsed);
  }
  return {};
}

bool wildcard_match(const std::string_view pattern, const std::string_view value) {
  std::size_t pattern_index = 0, value_index = 0;
  std::size_t star = std::string_view::npos, retry = 0;
  while (value_index < value.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' || pattern[pattern_index] == value[value_index])) {
      ++pattern_index; ++value_index;
    } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star = pattern_index++; retry = value_index;
    } else if (star != std::string_view::npos) {
      pattern_index = star + 1; value_index = ++retry;
    } else return false;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') ++pattern_index;
  return pattern_index == pattern.size();
}

bool any_match(const std::vector<std::string>& patterns, const std::string_view value) {
  return patterns.empty() || std::any_of(patterns.begin(), patterns.end(),
      [value](const std::string& pattern) { return wildcard_match(pattern, value); });
}

std::string scalar_text(const cbor::Value& value) {
  if (std::holds_alternative<std::string>(value.data)) return std::string(value.as_string());
  if (std::holds_alternative<std::int64_t>(value.data)) return std::to_string(value.as_integer());
  if (std::holds_alternative<bool>(value.data)) return value.as_bool() ? "true" : "false";
  return cbor::diagnostic(value);
}

std::vector<std::string> act_paths(const Act& act) {
  std::vector<std::string> values;
  for (const auto* key : {"path", "destination", "cwd", "workspace_root"})
    if (const auto* value = cbor::find(act.parameters, key);
        value && std::holds_alternative<std::string>(value->data))
      values.emplace_back(value->as_string());
  return values;
}

bool rule_matches(const PolicyRule& rule, const Act& act, const TrustLevel target_trust,
                  const std::string_view workspace) {
  if (!any_match(rule.acts, act.kind) || !any_match(rule.targets, act.target) ||
      !any_match(rule.risks, to_string(act.risk))) return false;
  const auto trust_text = target_trust == TrustLevel::t0 ? "t0" :
                          target_trust == TrustLevel::t1 ? "t1" :
                          target_trust == TrustLevel::t2 ? "t2" : "t3";
  if (!any_match(rule.trusts, trust_text)) return false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if ((rule.not_before_ms > 0 && now < rule.not_before_ms) ||
      (rule.not_after_ms > 0 && now > rule.not_after_ms)) return false;
  if (!rule.argv0.empty()) {
    const auto* argv = cbor::find(act.parameters, "argv");
    if (!argv || !argv->as_array() || argv->as_array()->empty() ||
        !any_match(rule.argv0, argv->as_array()->front().as_string())) return false;
  }
  if (!rule.paths.empty()) {
    const auto paths = act_paths(act);
    if (paths.empty()) return false;
    const auto expanded_matches = [&](const std::string& candidate) {
      return std::any_of(rule.paths.begin(), rule.paths.end(), [&](std::string pattern) {
        const auto marker = pattern.find("${workspace}");
        if (marker != std::string::npos) pattern.replace(marker, 12, workspace);
        return wildcard_match(pattern, candidate);
      });
    };
    if (!std::all_of(paths.begin(), paths.end(), expanded_matches)) return false;
  }
  for (const auto& [key, expected] : rule.parameters) {
    const auto* actual = cbor::find(act.parameters, key);
    if (!actual || scalar_text(*actual) != expected) return false;
  }
  return true;
}

PolicyEffect evaluate_single_policy(const FallenPolicy& policy, const Act& act,
                                    const TrustLevel target_trust,
                                    const std::string_view workspace) {
  bool allow = false, ask = false;
  for (const auto& rule : policy.rules) {
    if (!rule_matches(rule, act, target_trust, workspace)) continue;
    if (rule.effect == PolicyEffect::deny) return PolicyEffect::deny;
    if (rule.effect == PolicyEffect::allow) allow = true;
    if (rule.effect == PolicyEffect::ask) ask = true;
  }
  auto result = allow ? PolicyEffect::allow : ask ? PolicyEffect::ask : policy.default_effect;
  if (result != PolicyEffect::deny &&
      std::find(policy.approval_risks.begin(), policy.approval_risks.end(),
                std::string(to_string(act.risk))) != policy.approval_risks.end())
    result = PolicyEffect::ask;
  return result;
}

}  // namespace

std::string_view to_string(const RuntimeProfile profile) noexcept {
  switch (profile) {
    case RuntimeProfile::production: return "production";
    case RuntimeProfile::development: return "development";
    case RuntimeProfile::test: return "test";
  }
  return "production";
}

std::string_view to_string(const PolicyEffect effect) noexcept {
  switch (effect) {
    case PolicyEffect::allow: return "allow";
    case PolicyEffect::ask: return "ask";
    case PolicyEffect::deny: return "deny";
  }
  return "deny";
}

PolicyEffect evaluate_policy(const RuntimeConfig& config, const Act& act,
                             const TrustLevel target_trust,
                             const std::string_view workspace) {
  auto user = evaluate_single_policy(config.user_policy, act, target_trust, workspace);
  if (user == PolicyEffect::deny) return user;
  if (!config.project_policy.configured) return user;
  const auto project = evaluate_single_policy(config.project_policy, act, target_trust, workspace);
  if (project == PolicyEffect::deny || user == PolicyEffect::ask || project == PolicyEffect::ask)
    return project == PolicyEffect::deny ? PolicyEffect::deny : PolicyEffect::ask;
  return PolicyEffect::allow;
}

Result<TokmonPaths> resolve_paths(const std::optional<std::filesystem::path>& workspace) {
  TokmonPaths paths;
  paths.user = home_directory() / ".tokmon";
  paths.project = (workspace ? std::filesystem::absolute(*workspace) :
                               std::filesystem::current_path()) / ".tokmon";
  paths.data = paths.user / "data";
  paths.database = paths.data / "photons.sqlite3";
  paths.logs = paths.user / "logs";
  paths.run = paths.user / "run";
  paths.runtimes = paths.user / "runtimes";
  return paths;
}

Result<void> ensure_directory_layout(const TokmonPaths& paths) {
  std::error_code error;
  const std::vector<std::filesystem::path> directories = {
      paths.user, paths.data, paths.data / "blobs", paths.data / "segments",
      paths.data / "checkpoints", paths.data / "artifacts", paths.user / "lenses" / "installed",
      paths.user / "lenses" / "quarantine", paths.runtimes / "node",
      paths.runtimes / "cpython", paths.runtimes / "environments", paths.logs,
      paths.run, paths.user / "cache" / "projection", paths.user / "cache" / "ui"};
  for (const auto& directory : directories) {
    std::filesystem::create_directories(directory, error);
    if (error)
      return tl::unexpected(make_error(ErrorCode::io_error,
          "cannot create " + directory.string() + ": " + error.message()));
  }
  return {};
}

Result<RuntimeConfig> load_config(const std::optional<std::filesystem::path>& workspace) {
  auto paths = resolve_paths(workspace);
  if (!paths) return tl::unexpected(paths.error());
  RuntimeConfig config;
  config.paths = *paths;
  config.model_providers.emplace("local", ModelProviderConfig{
      .id = "local", .protocol = "local", .endpoint = "builtin://rhea",
      .model = "local-deterministic", .auth = "none", .allow_anonymous = true});
  for (const auto& short_id : official_lens_order()) {
    config.light_path.push_back(DesiredLens{
        .id = "org.tokmon.lens." + short_id,
        .artifact = "builtin:" + short_id,
        .enabled = true,
        .runtime = short_id == "termon" ? RuntimeKind::desktop : RuntimeKind::in_process});
  }
  if (auto result = merge_config_file(config, config.paths.user / "config.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_config_file(config, config.paths.project / "config.yaml"); !result)
    return tl::unexpected(result.error());
  if (config.profile != RuntimeProfile::production)
    config.light_path.push_back(DesiredLens{
        .id = "org.tokmon.lens.calculator", .artifact = "builtin:calculator",
        .enabled = true, .runtime = RuntimeKind::in_process});
  const auto selected = config.model_providers.find(config.default_model_provider);
  if (selected == config.model_providers.end() || !selected->second.enabled)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "models.default must name an enabled configured provider"));
  if (auto result = merge_light_path(config, config.paths.user / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_light_path(config, config.paths.project / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  if (config.profile == RuntimeProfile::production &&
      std::ranges::any_of(config.light_path, [](const DesiredLens& lens) {
        return lens.enabled && lens.id == "org.tokmon.lens.calculator";
      }))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        "Calculator Lens is available only in development or test profile"));
  return config;
}

Result<cbor::Value> update_light_path_document(
    cbor::Value root, std::string id, std::optional<std::string> artifact,
    std::optional<std::string> runtime, const bool enabled) {
  if (id.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "Lens id is required"));
  if (!root.is_map()) root = cbor::Value::Map{};
  auto& root_map = *root.as_map();
  root_map.erase("version");
  root_map["api"] = "tokmon.light-path/wavefront";
  auto& lenses_value = root_map["lenses"];
  if (!lenses_value.as_array()) lenses_value = cbor::Value::Array{};
  auto& lenses = *std::get_if<cbor::Value::Array>(&lenses_value.data);
  auto selected = lenses.end();
  for (auto iterator = lenses.begin(); iterator != lenses.end(); ++iterator) {
    const auto* mounted_id = cbor::find(*iterator, "id");
    if (mounted_id && mounted_id->as_string() == id) {
      selected = iterator;
      break;
    }
  }
  auto entry = selected == lenses.end() ? cbor::Value(cbor::Value::Map{}) : *selected;
  if (!entry.is_map()) entry = cbor::Value::Map{};
  auto& entry_map = *entry.as_map();
  entry_map["id"] = std::move(id);
  entry_map["enabled"] = enabled;
  if (enabled) {
    if (!artifact || artifact->empty())
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "Lens artifact is required"));
    const auto runtime_name = runtime && !runtime->empty() ? *runtime : "in_process";
    if (auto parsed = parse_runtime(runtime_name); !parsed)
      return tl::unexpected(parsed.error());
    entry_map["artifact"] = std::move(*artifact);
    entry_map["runtime"] = runtime_name;
  }
  if (selected == lenses.end()) lenses.push_back(std::move(entry));
  else *selected = std::move(entry);
  return root;
}

Result<cbor::Value> resolve_model_provider_context(
    const RuntimeConfig& config, const cbor::Value& request) {
  const auto* requested_provider = cbor::find(request, "provider");
  const auto provider_id = requested_provider && !requested_provider->as_string().empty()
      ? std::string(requested_provider->as_string())
      : config.default_model_provider;
  const auto found = config.model_providers.find(provider_id);
  if (found == config.model_providers.end() || !found->second.enabled)
    return tl::unexpected(make_error(ErrorCode::not_found,
        "requested model provider is not configured or is disabled: " + provider_id));

  const auto& provider = found->second;
  const auto* requested_model = cbor::find(request, "model");
  if (requested_model && !requested_model->as_string().empty() &&
      requested_model->as_string() != provider.model)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "requested model " + std::string(requested_model->as_string()) +
        " does not belong to provider " + provider.id +
        "; configured model is " + provider.model));

  const auto requested_effort = cbor::find(request, "effort")
      ? std::string(cbor::find(request, "effort")->as_string()) : std::string{};
  const auto normalized_effort = requested_effort == "较低" || requested_effort == "低"
      ? std::string("low") : requested_effort == "中等" || requested_effort == "标准"
      ? std::string("medium") : requested_effort == "高" ? std::string("high") :
        requested_effort == "最高" ? std::string("max") : provider.reasoning_effort;
  auto context = cbor::object({
      {"provider", provider.id}, {"protocol", provider.protocol},
      {"endpoint", provider.endpoint}, {"model", provider.model},
      {"auth", provider.auth}, {"allow_anonymous", provider.allow_anonymous},
      {"thinking", provider.thinking}, {"reasoning_effort", normalized_effort},
      {"max_output_tokens", provider.max_output_tokens},
      {"max_attempts", provider.max_attempts},
      {"retry_backoff_ms", provider.retry_backoff_ms},
      {"first_token_timeout_ms", provider.first_token_timeout_ms},
      {"idle_timeout_ms", provider.idle_timeout_ms},
      {"request_parameters", provider.request_parameters},
      {"workspace_root", config.paths.project.parent_path().generic_string()},
      {"access_mode", cbor::find(request, "access_mode")
          ? std::string(cbor::find(request, "access_mode")->as_string())
          : std::string("完全访问")},
      {"effort", cbor::find(request, "effort")
          ? std::string(cbor::find(request, "effort")->as_string())
          : std::string("标准")}});
  if (!provider.secret_ref.empty())
    (*context.as_map())["secret_ref"] = provider.secret_ref;
  return context;
}

}  // namespace tokmon
