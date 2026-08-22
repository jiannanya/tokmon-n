#include "lenses/tracket/tracket_lens.hpp"

#include <filesystem>
#include <fstream>
#include <set>

#include "tokmon/hash.hpp"
#include "tokmon/ids.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

std::string hash_material(const Photon& photon) {
  const auto encoded = cbor::encode(photon.payload);
  std::string value;
  value.append(photon.previous_hash).push_back('\0');
  value.append(std::to_string(photon.sequence)).push_back('\0');
  value.append(photon.id).push_back('\0');
  value.append(photon.ray).push_back('\0');
  value.append(photon.parent.value_or("")).push_back('\0');
  value.append(photon.kind).push_back('\0');
  value.append(photon.schema).push_back('\0');
  value.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  value.push_back('\0');
  value.append(std::to_string(photon.epoch)).push_back('\0');
  value.append(std::to_string(photon.committed_at_ms)).push_back('\0');
  value.append(photon.caused_by_act);
  return value;
}

cbor::Value verify_window(const PhotonWindow& photons) {
  cbor::Value::Array violations;
  std::set<std::string> ids;
  std::set<std::string> acts;
  const Photon* previous = nullptr;
  for (const auto& photon : photons.photons()) {
    const auto add = [&](const std::string_view code) {
      violations.push_back(cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
          {"photon", photon.id}, {"code", std::string(code)}}));
    };
    if (previous && photon.sequence != previous->sequence + 1u) add("sequence-gap-or-reuse");
    if (previous && photon.previous_hash != previous->hash) add("previous-hash-mismatch");
    if (photon.hash.size() != 64u || photon.hash != sha256_hex(hash_material(photon)))
      add("content-hash-mismatch");
    if (photon.id.empty() || !ids.insert(photon.id).second) add("duplicate-or-empty-id");
    if (photon.ray.empty() || photon.kind.empty() || photon.schema.empty()) add("schema-or-identity-missing");
    if (photon.parent && !ids.contains(*photon.parent)) add("invalid-parent");
    if (photon.kind.starts_with("act.") && !photon.caused_by_act.empty())
      acts.insert(photon.caused_by_act);
    if (!photon.caused_by_act.empty() && !photon.kind.starts_with("act.") &&
        !acts.contains(photon.caused_by_act)) add("unknown-causing-act");
    previous = &photon;
  }
  return cbor::object({{"valid", violations.empty()}, {"violations", std::move(violations)},
      {"photon_count", static_cast<std::int64_t>(photons.photons().size())},
      {"tail_hash", photons.latest() ? photons.latest()->hash : ""}});
}

Result<std::filesystem::path> directory(const cbor::Value& parameters,
                                        const std::string_view child) {
  const auto* root = cbor::find(parameters, "storage_root");
  if (!root || root->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch, "storage_root is required"));
  std::error_code error;
  auto path = std::filesystem::absolute(std::filesystem::path(root->as_string())) / child;
  std::filesystem::create_directories(path, error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot create trace directory"));
  return path;
}

Result<void> write_once(const std::filesystem::path& path,
                        const std::span<const std::uint8_t> bytes) {
  if (std::filesystem::exists(path)) return {};
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output) return tl::unexpected(make_error(ErrorCode::io_error,
                                                 "cannot write trace artifact"));
  return {};
}

std::vector<std::uint8_t> compress_rle(const std::span<const std::uint8_t> input) {
  std::vector<std::uint8_t> output{'T', 'R', 'L', 'E', '1'};
  for (std::size_t offset = 0; offset < input.size();) {
    std::size_t count = 1;
    while (offset + count < input.size() && input[offset + count] == input[offset] && count < 255)
      ++count;
    output.push_back(static_cast<std::uint8_t>(count));
    output.push_back(input[offset]); offset += count;
  }
  return output;
}

}  // namespace

TracketLens::TracketLens() : LensBase(make_manifest("tracket", "Tracket / 因果记录与回放光路镜",
    {"fact.integrity", "ui.trajectory", "ui.causality"}, {{"*", "*"}},
    {{"integrity.verify", "tokmon.integrity.verify.v1"},
     {"trace.vault-write", "tokmon.trace.vault-write.v1"},
     {"replay.create", "tokmon.replay.create.v1"},
     {"ray.fork", "tokmon.ray.fork.v1"},
     {"trajectory.export", "tokmon.trajectory.export.v1"}},
    {"photon.emit", "trace.write", "artifact.write", "log.write"})) {}

Result<void> TracketLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array timeline;
  cbor::Value::Array edges;
  for (const auto& photon : photons.photons()) {
    if (photon.parent) edges.push_back(cbor::object({{"from", *photon.parent},
        {"to", photon.id}, {"relation", "parent"}}));
    if (!photon.caused_by_act.empty()) edges.push_back(cbor::object({
        {"from", photon.caused_by_act}, {"to", photon.id}, {"relation", "caused_by_act"}}));
    timeline.push_back(cbor::object({{"sequence", static_cast<std::int64_t>(photon.sequence)},
        {"id", photon.id}, {"kind", photon.kind}, {"schema", photon.schema},
        {"epoch", static_cast<std::int64_t>(photon.epoch)},
        {"caused_by_act", photon.caused_by_act}}));
  }
  if (auto result = surface.add("ui.trajectory", "active-ray", std::move(timeline), 20);
      !result) return result;
  if (auto result = surface.add("ui.causality", "active-ray", std::move(edges), 15);
      !result) return result;
  return identify(surface, "fact.integrity", verify_window(photons));
}

Result<RefractionResult> TracketLens::refract(const PhotonWindow& photons, const Act& act,
                                               RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "integrity.verify") {
    auto report = verify_window(photons);
    const auto valid = cbor::find(report, "valid")->as_bool();
    return emit(beam, valid ? "integrity.verified" : "integrity.failed",
                "tokmon.integrity.report.v1", std::move(report),
                valid ? "" : "causal integrity violations found");
  }
  if (act.kind == "replay.create") {
    const auto* level = cbor::find(act.parameters, "level");
    if (!level || (level->as_string() != "R0" && level->as_string() != "R1" &&
                   level->as_string() != "R2" && level->as_string() != "R3"))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "replay level must be R0, R1, R2 or R3"));
    if (level->as_string() == "R3" && !act.approved)
      return tl::unexpected(make_error(ErrorCode::approval_required,
                                       "R3 replay requires a bound approval"));
    cbor::Value::Array replay;
    for (const auto& photon : photons.photons()) {
      const bool include = level->as_string() == "R0"
          ? photon.kind == "user.input" || photon.kind == "assistant.message" ||
              photon.kind == "tool.result" || photon.kind == "fs.changed"
          : level->as_string() == "R2" ? photon.kind.starts_with("act.") : true;
      if (include) replay.push_back(redact_value(to_cbor(photon)));
    }
    return emit(beam, "replay.created", "tokmon.trajectory.result.v1", cbor::object({
        {"level", *level}, {"events", std::move(replay)}, {"source_stream_mutated", false},
        {"reality_actions_executed", false},
        {"fork_ray", level->as_string() == "R3" ? make_id("ray") : ""}}));
  }
  if (act.kind == "ray.fork")
    return emit(beam, "ray.forked", "tokmon.trajectory.result.v1", cbor::object({
        {"source_ray", act.ray}, {"source_tail", photons.latest() ? photons.latest()->hash : ""},
        {"fork_ray", make_id("ray")}, {"source_mutated", false}}));

  const bool vault = act.kind == "trace.vault-write";
  auto root = directory(act.parameters, vault ? "trace-vault" : "exports");
  if (!root) return tl::unexpected(root.error());
  auto raw = cbor::encode(vault && cbor::find(act.parameters, "frames")
      ? *cbor::find(act.parameters, "frames") : redact_value(to_cbor(photons)));
  const auto source_hash = sha256_hex(raw);
  auto stored = vault ? compress_rle(raw) : raw;
  const auto digest = sha256_hex(stored);
  const auto path = *root / (digest + (vault ? ".trle" : ".cbor"));
  if (auto written = write_once(path, stored); !written) return tl::unexpected(written.error());
  return emit(beam, vault ? "trace.vault-stored" : "trajectory.exported",
      "tokmon.trajectory.export.v1", cbor::object({{"path", path.generic_string()},
        {"sha256", digest}, {"source_sha256", source_hash},
        {"bytes", static_cast<std::int64_t>(stored.size())},
        {"compression", vault ? "rle-v1" : "none"},
        {"schemas_included", !vault}, {"secrets_included", false}}));
}

}  // namespace tokmon::builtin
