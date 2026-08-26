#include "lenses/aya/aya_lens.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "lenses/common/process_runner.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/ids.hpp"

namespace tokmon::builtin {
namespace {

std::string field(const cbor::Value& value, const std::string_view name,
                  const std::string_view fallback = {}) {
  const auto* item = cbor::find(value, name);
  return item ? std::string(item->as_string(fallback)) : std::string(fallback);
}

std::set<std::string> strings(const cbor::Value* value) {
  std::set<std::string> result;
  if (value && value->as_array())
    for (const auto& item : *value->as_array())
      if (std::holds_alternative<std::string>(item.data)) result.emplace(item.as_string());
  return result;
}

bool secret_material(const cbor::Value& value) {
  if (const auto* map = value.as_map()) {
    for (const auto& [key, child] : *map) {
      std::string lower = key;
      std::ranges::transform(lower, lower.begin(),
          [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
      if (lower == "secret" || lower == "secret_value" || lower == "api_key" ||
          lower == "password" || lower == "credential") return true;
      if (secret_material(child)) return true;
    }
  }
  if (const auto* array = value.as_array())
    return std::ranges::any_of(*array, secret_material);
  return false;
}

}  // namespace

AyaLens::AyaLens() : LensBase(make_manifest("aya", "Aya / 子运行分形复眼镜",
    {"child.runs", "ui.child-runs", "child.topology"},
    {{"child.*", "*"}, {"workspace.merge-*", "*"}},
    {{"child.spawn", "tokmon.child.spawn.v1"},
     {"child.message", "tokmon.child.message.v1"},
     {"child.join", "tokmon.child.join.v1"},
     {"child.cancel", "tokmon.child.cancel.v1"},
     {"workspace.merge-proposal", "tokmon.workspace.merge-proposal.v1"}},
    {"photon.emit", "io.process.git", "workspace.worktree", "log.write"})) {}

Result<void> AyaLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  std::map<std::string, cbor::Value, std::less<>> states;
  cbor::Value::Array messages;
  for (const auto& photon : photons.photons()) {
    if (!photon.kind.starts_with("child.") && !photon.kind.starts_with("workspace.merge-"))
      continue;
    if (photon.kind == "child.message-delivered" ||
        photon.kind == "child.help-requested") messages.push_back(photon.payload);
    const auto child = field(photon.payload, "child_ray");
    if (child.empty()) continue;
    auto& state = states[child];
    if (!state.is_map()) state = cbor::object({{"child_ray", child}});
    auto* map = state.as_map();
    (*map)["last_sequence"] = static_cast<std::int64_t>(photon.sequence);
    (*map)["state"] = photon.kind;
    (*map)["detail"] = photon.payload;
    if (photon.kind == "child.started") {
      for (const auto* key : {"budget", "deadline_ms", "workspace_mode", "workspace_path",
                              "worktree_ref", "join_policy", "allowed_acts"})
        if (const auto* value = cbor::find(photon.payload, key)) (*map)[key] = *value;
    }
    if (photon.kind == "child.progress-observed")
      (*map)["progress"] = cbor::find(photon.payload, "progress")
          ? *cbor::find(photon.payload, "progress") : cbor::Value(0);
    if (photon.kind == "child.heartbeat-observed")
      (*map)["last_heartbeat_ms"] = photon.committed_at_ms;
    if (photon.kind == "child.usage-observed") {
      auto usage = cbor::find(state, "usage") ? *cbor::find(state, "usage")
                                               : cbor::Value(cbor::Value::Map{});
      for (const auto* key : {"tokens", "cost_microunits", "elapsed_ms", "tool_calls"}) {
        const auto current = cbor::find(usage, key) ? cbor::find(usage, key)->as_integer() : 0;
        const auto delta = cbor::find(photon.payload, key)
            ? cbor::find(photon.payload, key)->as_integer() : 0;
        (*usage.as_map())[key] = current + delta;
      }
      (*map)["usage"] = std::move(usage);
    }
  }
  cbor::Value::Array children;
  cbor::Value::Array edges;
  for (auto& [child, state] : states) {
    children.push_back(state);
    edges.push_back(cbor::object({{"from", "parent"}, {"to", child},
                                  {"relation", "child"}}));
  }
  if (auto result = identify(surface, "child.runs", cbor::object({
      {"items", children}, {"messages", std::move(messages)},
      {"default_workspace_mode", "read_only"}, {"secret_inheritance", false}})); !result)
    return result;
  if (auto result = surface.add("child.topology", "active-ray", std::move(edges), 15);
      !result) return result;
  return surface.add("ui.child-runs", "active-ray", std::move(children), 20);
}

Result<RefractionResult> AyaLens::refract(const PhotonWindow& photons, const Act& act,
                                           RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "child.spawn") {
    const auto* parent_budget = cbor::find(act.parameters, "parent_budget");
    const auto* child_budget = cbor::find(act.parameters, "budget");
    const auto* allowed = cbor::find(act.parameters, "allowed_acts");
    const auto workspace = field(act.parameters, "workspace_mode");
    const auto join = field(act.parameters, "join_policy", "all");
    const auto mode = field(act.parameters, "mode", "spawn");
    const auto deadline_ms = cbor::find(act.parameters, "deadline_ms")
        ? cbor::find(act.parameters, "deadline_ms")->as_integer() : 30'000;
    if (!parent_budget || !child_budget || parent_budget->as_integer() <= 0 ||
        child_budget->as_integer() <= 0 || !allowed || !allowed->as_array() ||
        deadline_ms <= 0 || deadline_ms > 86'400'000 ||
        (workspace != "read_only" && workspace != "isolated_write") ||
        (join != "all" && join != "any" && join != "quorum" && join != "manual") ||
        (mode != "spawn" && mode != "fork"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "child.spawn requires valid mode, budgets, allowed_acts, workspace_mode and join_policy"));
    if (child_budget->as_integer() > parent_budget->as_integer())
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "child budget exceeds parent budget"));
    const auto child_acts = strings(allowed);
    const auto parent_acts = strings(cbor::find(act.parameters, "parent_allowed_acts"));
    if (!parent_acts.empty() && !std::ranges::all_of(child_acts,
        [&](const auto& kind) { return parent_acts.contains(kind); }))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "child ActKind scope exceeds parent scope"));
    if (secret_material(act.parameters))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "child context cannot inherit secret material"));

    const auto child_ray = make_id("ray");
    std::string workspace_path = field(act.parameters, "workspace_root");
    std::string worktree_ref;
    if (workspace == "isolated_write") {
      std::error_code error;
      const auto root = std::filesystem::weakly_canonical(workspace_path, error);
      if (error || !std::filesystem::is_directory(root / ".git"))
        return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                         "isolated_write requires a Git worktree"));
      const auto worktree_root = cbor::find(act.parameters, "worktree_root")
          ? std::filesystem::path(cbor::find(act.parameters, "worktree_root")->as_string())
          : root.parent_path() / ".tokmon-worktrees";
      std::filesystem::create_directories(worktree_root, error);
      if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot create worktree root"));
      const auto worktree = std::filesystem::absolute(worktree_root) / child_ray;
      auto created = run_process(ProcessRequest{.argv = {"git", "worktree", "add", "--detach",
          worktree.string(), "HEAD"}, .cwd = root, .timeout = act.timeout,
          .max_output_bytes = 256u * 1024u, .stop = beam.stop_token()});
      if (!created) return tl::unexpected(created.error());
      if (created->exit_code != 0)
        return tl::unexpected(make_error(ErrorCode::io_error,
                                         "git worktree creation failed: " + created->stderr_text));
      workspace_path = worktree.generic_string();
      worktree_ref = sha256_hex(workspace_path);
    }
    const auto task = cbor::find(act.parameters, "task") ?
        std::string(cbor::find(act.parameters, "task")->as_string()) : std::string{};
    if (task.empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "child.spawn requires a task"));
    auto child_input = beam.emit_to(child_ray, "user.input", "tokmon.user.input.v1",
        cbor::object({{"text", task}, {"parent_ray", act.ray},
          {"spawn_act", act.id}, {"mode", mode},
          {"inherited_refs", cbor::find(act.parameters, "inherit_refs") ?
              *cbor::find(act.parameters, "inherit_refs") : cbor::Value::Array{}},
          {"secret_inherited", false}}));
    if (!child_input) return tl::unexpected(child_input.error());
    auto started = beam.emit("child.started", "tokmon.child.result.v1", cbor::object({
        {"parent_ray", act.ray}, {"child_ray", child_ray}, {"mode", mode},
        {"task", task},
        {"budget", *child_budget}, {"allowed_acts", *allowed},
        {"deadline_ms", deadline_ms},
        {"workspace_mode", workspace}, {"workspace_path", workspace_path},
        {"worktree_ref", worktree_ref}, {"join_policy", join},
        {"child_input", child_input->id},
        {"secret_inherited", false}, {"history_deleted", false}}));
    if (!started) return tl::unexpected(started.error());
    return RefractionResult{.status = RefractionStatus::completed,
        .emitted = {child_input->id, started->id}, .detail = "child ray started"};
  }
  if (act.kind == "child.message") {
    const auto recipient = field(act.parameters, "recipient");
    const auto sender = field(act.parameters, "sender", act.ray);
    const auto type = field(act.parameters, "message_type", "message");
    const auto* payload = cbor::find(act.parameters, "payload");
    if (recipient.empty() || sender.empty() || !payload || secret_material(*payload) ||
        (type != "message" && type != "progress" && type != "help" &&
         type != "heartbeat" && type != "usage"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "child.message requires safe payload and recipient"));
    if (type == "progress") {
      const auto* progress = cbor::find(*payload, "progress");
      if (!progress || progress->as_integer(-1) < 0 || progress->as_integer() > 100)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "child progress must be within 0..100"));
    }
    if (type == "usage") {
      for (const auto* key : {"tokens", "cost_microunits", "elapsed_ms", "tool_calls"})
        if (const auto* value = cbor::find(*payload, key); value && value->as_integer(-1) < 0)
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "child usage counters cannot be negative"));
    }
    const auto kind = type == "progress" ? "child.progress-observed" :
        type == "help" ? "child.help-requested" :
        type == "heartbeat" ? "child.heartbeat-observed" :
        type == "usage" ? "child.usage-observed" : "child.message-delivered";
    auto result = cbor::object({{"sender", sender},
          {"recipient", recipient}, {"payload", *payload},
          {"child_ray", field(act.parameters, "child_ray", sender)},
          {"message_type", type},
          {"delivery_id", sha256_hex(act.idempotency_key + recipient)}});
    if (type == "progress")
      (*result.as_map())["progress"] = *cbor::find(*payload, "progress");
    if (type == "usage")
      for (const auto* key : {"tokens", "cost_microunits", "elapsed_ms", "tool_calls"})
        (*result.as_map())[key] = cbor::find(*payload, key)
            ? *cbor::find(*payload, key) : cbor::Value(0);
    return emit(beam, kind, "tokmon.child.message-result.v1", std::move(result));
  }
  if (act.kind == "child.join") {
    auto children = strings(cbor::find(act.parameters, "child_rays"));
    const auto one = field(act.parameters, "child_ray");
    if (children.empty() && !one.empty()) children.insert(one);
    if (children.empty()) return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                                            "child.join requires child_rays"));
    std::set<std::string> completed;
    std::set<std::string> failed;
    cbor::Value::Array artifacts;
    cbor::Value::Array summaries;
    cbor::Value::Array conflicts;
    for (const auto& photon : photons.photons()) {
      const auto child = field(photon.payload, "child_ray");
      if (!children.contains(child)) continue;
      if (photon.kind == "child.completed") completed.insert(child);
      if (photon.kind == "child.failed" || photon.kind == "child.cancelled") failed.insert(child);
      if (const auto* refs = cbor::find(photon.payload, "artifact_refs")) artifacts.push_back(*refs);
      if (const auto* summary = cbor::find(photon.payload, "summary"))
        summaries.push_back(cbor::object({{"child_ray", child}, {"summary", *summary}}));
      if (const auto* found = cbor::find(photon.payload, "conflicts"); found && found->as_array())
        for (const auto& conflict : *found->as_array())
          conflicts.push_back(cbor::object({{"child_ray", child}, {"conflict", conflict}}));
    }
    const auto policy = field(act.parameters, "policy", "all");
    const auto quorum = static_cast<std::size_t>(std::max<std::int64_t>(1,
        cbor::find(act.parameters, "quorum") ? cbor::find(act.parameters, "quorum")->as_integer(1) : 1));
    const bool ready = policy == "manual" || (policy == "all" && completed.size() == children.size()) ||
        (policy == "any" && !completed.empty()) || (policy == "quorum" && completed.size() >= quorum);
    if (!ready)
      return emit(beam, "child.join-pending", "tokmon.child.join.v1", cbor::object({
          {"policy", policy}, {"expected", static_cast<std::int64_t>(children.size())},
          {"completed", static_cast<std::int64_t>(completed.size())},
          {"failed", static_cast<std::int64_t>(failed.size())}}));
    cbor::Value::Array joined;
    for (const auto& child : completed) joined.push_back(child);
    return emit(beam, "child.joined", "tokmon.child.result.v1", cbor::object({
        {"parent_ray", act.ray}, {"policy", policy}, {"children", std::move(joined)},
        {"summaries", std::move(summaries)}, {"artifact_refs", std::move(artifacts)},
        {"conflicts", std::move(conflicts)}, {"source_mutated", false}}));
  }
  const auto child_ray = field(act.parameters, "child_ray");
  if (child_ray.empty()) return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                                           "child action requires child_ray"));
  if (act.kind == "workspace.merge-proposal")
    return emit(beam, "workspace.merge-proposed", "tokmon.workspace.merge-proposal.v1",
        cbor::object({{"child_ray", child_ray}, {"worktree", field(act.parameters, "worktree")},
          {"base", field(act.parameters, "base")}, {"target", field(act.parameters, "target")},
          {"automatic_overwrite", false}, {"requires_cove", true}, {"requires_approval", true}}));
  return emit(beam, "child.cancel-requested", "tokmon.child.result.v1",
      cbor::object({{"parent_ray", act.ray}, {"child_ray", child_ray},
        {"propagation", field(act.parameters, "propagation", "descendants")},
        {"reason", field(act.parameters, "reason")}, {"history_deleted", false}}));
}

}  // namespace tokmon::builtin
