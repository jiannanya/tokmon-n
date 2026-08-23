#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tokmon/act.hpp"
#include "tokmon/error.hpp"
#include "tokmon/lens.hpp"

namespace tokmon {

struct TokmonPaths {
  std::filesystem::path user;
  std::filesystem::path project;
  std::filesystem::path data;
  std::filesystem::path database;
  std::filesystem::path logs;
  std::filesystem::path run;
  std::filesystem::path runtimes;
};

struct DesiredLens {
  LensId id;
  std::string artifact;
  bool enabled{true};
  RuntimeKind runtime{RuntimeKind::in_process};
};

enum class PolicyEffect : std::uint8_t { allow, ask, deny };

struct PolicyRule {
  PolicyEffect effect{PolicyEffect::deny};
  std::vector<std::string> acts;
  std::vector<std::string> targets;
  std::vector<std::string> trusts;
  std::vector<std::string> risks;
  std::vector<std::string> paths;
  std::vector<std::string> argv0;
  std::unordered_map<std::string, std::string> parameters;
  std::int64_t not_before_ms{0};
  std::int64_t not_after_ms{0};
};

struct FallenPolicy {
  PolicyEffect default_effect{PolicyEffect::allow};
  std::vector<PolicyRule> rules;
  std::vector<std::string> approval_risks{"external_irreversible"};
  bool configured{false};
};

// `id` names the platform/account while `protocol` selects the wire adapter.
// DeepSeek, OpenRouter and private gateways can therefore share the same
// openai-compatible adapter without becoming special cases in Rhea.
struct ModelProviderConfig {
  std::string id;
  std::string protocol{"openai-compatible"};
  std::string endpoint;
  std::string model;
  std::string secret_ref;
  // Optional name of an environment variable used only to bootstrap the
  // operating-system credential vault. The variable value never enters
  // configuration, Facts, Acts, Photons, process arguments, or logs.
  std::string secret_env;
  std::string auth{"protocol-default"};
  bool enabled{true};
  bool allow_anonymous{false};
  bool thinking{false};
  std::string reasoning_effort;
  std::int64_t max_output_tokens{4096};
  std::int64_t max_attempts{2};
  std::int64_t retry_backoff_ms{250};
};

struct RuntimeConfig {
  TokmonPaths paths;
  std::vector<DesiredLens> light_path;
  std::string log_level{"info"};
  std::size_t photon_window{4096};
  std::size_t max_beats{32};
  bool require_signatures{false};
  std::unordered_map<std::string, std::string> trusted_signers;
  FallenPolicy user_policy;
  FallenPolicy project_policy;
  std::string default_model_provider{"local"};
  std::unordered_map<std::string, ModelProviderConfig> model_providers;
};

[[nodiscard]] std::string_view to_string(PolicyEffect effect) noexcept;
[[nodiscard]] PolicyEffect evaluate_policy(const RuntimeConfig& config, const Act& act,
                                            TrustLevel target_trust,
                                            std::string_view workspace);

[[nodiscard]] Result<TokmonPaths> resolve_paths(
    const std::optional<std::filesystem::path>& workspace = std::nullopt);
[[nodiscard]] Result<RuntimeConfig> load_config(
    const std::optional<std::filesystem::path>& workspace = std::nullopt);
Result<void> ensure_directory_layout(const TokmonPaths& paths);

}  // namespace tokmon
