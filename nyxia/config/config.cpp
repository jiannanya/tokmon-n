#include "tokmon/config.hpp"

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "tokmon/builtin_lens.hpp"

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

Result<void> reject_unknown(const YAML::Node& map, const std::set<std::string>& allowed,
                            const std::filesystem::path& source) {
  if (!map || !map.IsMap()) return {};
  for (const auto& entry : map) {
    const auto key = entry.first.as<std::string>();
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

Result<std::vector<std::string>> string_sequence(const YAML::Node& node,
                                                 const std::string_view field,
                                                 const std::filesystem::path& source) {
  std::vector<std::string> values;
  if (!node) return values;
  if (!node.IsSequence())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": fallen." + std::string(field) + " must be a sequence"));
  for (const auto& value : node) values.push_back(value.as<std::string>());
  return values;
}

Result<void> parse_fallen_policy(FallenPolicy& policy, const YAML::Node& fallen,
                                 const std::filesystem::path& source) {
  if (!fallen || !fallen.IsMap())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": fallen must be a map"));
  if (auto result = reject_unknown(fallen, {"defaults", "rules", "approvals"}, source);
      !result) return result;
  policy.configured = true;
  if (fallen["defaults"]) {
    auto effect = parse_policy_effect(fallen["defaults"].as<std::string>());
    if (!effect) return tl::unexpected(effect.error());
    policy.default_effect = *effect;
  }
  if (const auto rules = fallen["rules"]) {
    if (!rules.IsSequence())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": fallen.rules must be a sequence"));
    policy.rules.clear();
    for (const auto& encoded : rules) {
      if (!encoded.IsMap())
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         source.string() + ": Fallen rule must be a map"));
      if (auto result = reject_unknown(encoded,
          {"effect", "acts", "targets", "trusts", "risks", "paths", "argv0", "parameters",
           "not_before_ms", "not_after_ms"}, source); !result) return result;
      if (!encoded["effect"])
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         source.string() + ": Fallen rule effect is required"));
      auto effect = parse_policy_effect(encoded["effect"].as<std::string>());
      if (!effect) return tl::unexpected(effect.error());
      PolicyRule rule; rule.effect = *effect;
      auto acts = string_sequence(encoded["acts"], "rules.acts", source);
      auto targets = string_sequence(encoded["targets"], "rules.targets", source);
      auto trusts = string_sequence(encoded["trusts"], "rules.trusts", source);
      auto risks = string_sequence(encoded["risks"], "rules.risks", source);
      auto paths = string_sequence(encoded["paths"], "rules.paths", source);
      auto argv0 = string_sequence(encoded["argv0"], "rules.argv0", source);
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
      if (const auto parameters = encoded["parameters"]) {
        if (!parameters.IsMap())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              source.string() + ": Fallen rule parameters must be a map"));
        for (const auto& entry : parameters)
          rule.parameters.emplace(entry.first.as<std::string>(),
                                  entry.second.as<std::string>());
      }
      if (encoded["not_before_ms"]) rule.not_before_ms = encoded["not_before_ms"].as<std::int64_t>();
      if (encoded["not_after_ms"]) rule.not_after_ms = encoded["not_after_ms"].as<std::int64_t>();
      if (rule.not_before_ms > 0 && rule.not_after_ms > 0 &&
          rule.not_before_ms > rule.not_after_ms)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
            source.string() + ": Fallen rule time window is inverted"));
      policy.rules.push_back(std::move(rule));
    }
  }
  if (const auto approvals = fallen["approvals"]) {
    if (!approvals.IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": fallen.approvals must be a map"));
    policy.approval_risks.clear();
    for (const auto& entry : approvals) {
      const auto risk = entry.first.as<std::string>();
      auto principals = string_sequence(entry.second, "approvals." + risk, source);
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

Result<void> parse_model_providers(RuntimeConfig& config, const YAML::Node& models,
                                   const std::filesystem::path& source) {
  if (!models.IsMap())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": models must be a map"));
  if (auto result = reject_unknown(models, {"default", "providers"}, source); !result)
    return result;
  if (models["default"]) config.default_model_provider = models["default"].as<std::string>();
  const auto providers = models["providers"];
  if (!providers) return {};
  if (!providers.IsMap())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": models.providers must be a map"));
  static const std::regex id_pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
  static const std::set<std::string> protocols{
      "local", "openai-compatible", "anthropic", "gemini"};
  static const std::set<std::string> auth_modes{
      "protocol-default", "bearer", "x-api-key", "x-goog-api-key", "none"};
  for (const auto& encoded : providers) {
    const auto id = encoded.first.as<std::string>();
    const auto value = encoded.second;
    if (!std::regex_match(id, id_pattern) || !value.IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": invalid model provider id or definition: " + id));
    if (auto result = reject_unknown(value,
        {"protocol", "endpoint", "model", "secret_ref", "auth", "enabled",
         "allow_anonymous", "thinking", "reasoning_effort", "max_output_tokens",
         "max_attempts", "retry_backoff_ms"}, source); !result) return result;
    ModelProviderConfig provider;
    if (const auto found = config.model_providers.find(id);
        found != config.model_providers.end()) provider = found->second;
    provider.id = id;
    if (value["protocol"]) provider.protocol = value["protocol"].as<std::string>();
    if (value["endpoint"]) provider.endpoint = value["endpoint"].as<std::string>();
    if (value["model"]) provider.model = value["model"].as<std::string>();
    if (value["secret_ref"]) provider.secret_ref = value["secret_ref"].as<std::string>();
    if (value["auth"]) provider.auth = value["auth"].as<std::string>();
    if (value["enabled"]) provider.enabled = value["enabled"].as<bool>();
    if (value["allow_anonymous"]) provider.allow_anonymous = value["allow_anonymous"].as<bool>();
    if (value["thinking"]) provider.thinking = value["thinking"].as<bool>();
    if (value["reasoning_effort"])
      provider.reasoning_effort = value["reasoning_effort"].as<std::string>();
    if (value["max_output_tokens"])
      provider.max_output_tokens = value["max_output_tokens"].as<std::int64_t>();
    if (value["max_attempts"])
      provider.max_attempts = value["max_attempts"].as<std::int64_t>();
    if (value["retry_backoff_ms"])
      provider.retry_backoff_ms = value["retry_backoff_ms"].as<std::int64_t>();
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
    if (provider.max_output_tokens <= 0 || provider.max_output_tokens > 1'000'000 ||
        provider.max_attempts <= 0 || provider.max_attempts > 10 ||
        provider.retry_backoff_ms < 0 || provider.retry_backoff_ms > 60'000)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
          source.string() + ": provider retry/token limits are outside the allowed range"));
    config.model_providers[id] = std::move(provider);
  }
  return {};
}

Result<void> merge_config_file(RuntimeConfig& config, const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  try {
    const auto root = YAML::LoadFile(source.string());
    if (!root.IsMap())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + " must contain a YAML map"));
    if (auto result = reject_unknown(root,
        {"logging", "engine", "security", "ui", "fallen", "models"}, source);
        !result) return result;
    if (const auto logging = root["logging"]) {
      if (auto result = reject_unknown(logging, {"level"}, source); !result) return result;
      if (logging["level"]) config.log_level = logging["level"].as<std::string>();
    }
    if (const auto engine = root["engine"]) {
      if (auto result = reject_unknown(engine, {"photon_window", "max_beats"}, source); !result)
        return result;
      if (engine["photon_window"]) config.photon_window = engine["photon_window"].as<std::size_t>();
      if (engine["max_beats"]) config.max_beats = engine["max_beats"].as<std::size_t>();
    }
    if (const auto security = root["security"]) {
      if (auto result = reject_unknown(security,
          {"require_signatures", "trusted_signers"}, source); !result)
        return result;
      if (security["require_signatures"])
        config.require_signatures = security["require_signatures"].as<bool>();
      if (const auto signers = security["trusted_signers"]) {
        if (!signers.IsMap())
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
              source.string() + ": security.trusted_signers must be a map"));
        for (const auto& entry : signers) {
          const auto signer = entry.first.as<std::string>();
          const auto secret_ref = entry.second.as<std::string>();
          if (signer.empty() || secret_ref.empty())
            return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                source.string() + ": trusted signer names and SecretRefs cannot be empty"));
          config.trusted_signers[signer] = secret_ref;
        }
      }
    }
    if (const auto fallen = root["fallen"]) {
      const auto project_file = source.parent_path().filename() == ".tokmon" &&
                                source.parent_path() == config.paths.project;
      auto result = parse_fallen_policy(project_file ? config.project_policy : config.user_policy,
                                        fallen, source);
      if (!result) return result;
    }
    if (const auto models = root["models"]) {
      auto result = parse_model_providers(config, models, source);
      if (!result) return result;
    }
    if (config.photon_window == 0 || config.photon_window > 100'000 ||
        config.max_beats == 0 || config.max_beats > 1024)
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "engine limits are outside the allowed range"));
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot parse " + source.string() + ": " + exception.what()));
  }
}

Result<void> merge_light_path(std::vector<DesiredLens>& lenses,
                              const std::filesystem::path& source) {
  if (!std::filesystem::exists(source)) return {};
  try {
    const auto root = YAML::LoadFile(source.string());
    if (auto result = reject_unknown(root, {"version", "lenses"}, source); !result) return result;
    const auto entries = root["lenses"];
    if (!entries || !entries.IsSequence())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       source.string() + ": lenses must be a sequence"));
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < lenses.size(); ++i) index[lenses[i].id] = i;
    for (const auto& entry : entries) {
      if (auto result = reject_unknown(entry, {"id", "artifact", "enabled", "runtime"}, source);
          !result) return result;
      DesiredLens lens;
      lens.id = entry["id"].as<std::string>();
      lens.artifact = entry["artifact"] ? entry["artifact"].as<std::string>() : "builtin:" + lens.id;
      if (!lens.artifact.starts_with("builtin:")) {
        auto artifact = std::filesystem::path(lens.artifact);
        if (artifact.is_relative())
          lens.artifact = (source.parent_path() / artifact).lexically_normal().string();
      }
      lens.enabled = !entry["enabled"] || entry["enabled"].as<bool>();
      auto runtime = parse_runtime(entry["runtime"] ?
          entry["runtime"].as<std::string>() : "in_process");
      if (!runtime) return tl::unexpected(runtime.error());
      lens.runtime = *runtime;
      if (const auto found = index.find(lens.id); found != index.end()) lenses[found->second] = lens;
      else { index[lens.id] = lenses.size(); lenses.push_back(std::move(lens)); }
    }
    return {};
  } catch (const YAML::Exception& exception) {
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "cannot parse " + source.string() + ": " + exception.what()));
  }
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
  const auto selected = config.model_providers.find(config.default_model_provider);
  if (selected == config.model_providers.end() || !selected->second.enabled)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        "models.default must name an enabled configured provider"));
  if (auto result = merge_light_path(config.light_path, config.paths.user / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  if (auto result = merge_light_path(config.light_path, config.paths.project / "light-path.yaml"); !result)
    return tl::unexpected(result.error());
  return config;
}

}  // namespace tokmon
