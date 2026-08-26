#include "tokmon/manifest_io.hpp"

#include <limits>
#include <set>

#include "tokmon/yaml.hpp"

namespace tokmon {
namespace {

Result<void> reject_unknown(const cbor::Value& map,
                            const std::set<std::string>& allowed,
                            const std::filesystem::path& source) {
  if (!map.as_map())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + " must contain a YAML map"));
  for (const auto& [key, value] : *map.as_map()) {
    (void)value;
    if (!allowed.contains(key))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          "unknown Lens manifest field '" + key + "' in " + source.string()));
  }
  return {};
}

RuntimeKind runtime_kind(const std::string_view text) {
  if (text == "native_worker") return RuntimeKind::native_worker;
  if (text == "node") return RuntimeKind::node;
  if (text == "cpython") return RuntimeKind::cpython;
  if (text == "wasm") return RuntimeKind::wasm;
  if (text == "desktop") return RuntimeKind::desktop;
  return RuntimeKind::in_process;
}

Result<std::vector<PhotonPattern>> photon_patterns(
    const cbor::Value* sequence, const std::filesystem::path& source) {
  if (!sequence || !sequence->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": observes must be a sequence"));
  std::vector<PhotonPattern> patterns;
  patterns.reserve(sequence->as_array()->size());
  for (const auto& entry : *sequence->as_array()) {
    if (auto checked = reject_unknown(entry, {"kind", "schema"}, source); !checked)
      return tl::unexpected(checked.error());
    const auto* kind = cbor::find(entry, "kind");
    const auto* schema = cbor::find(entry, "schema");
    if (!kind || !schema || !std::holds_alternative<std::string>(kind->data) ||
        !std::holds_alternative<std::string>(schema->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "observe pattern requires kind and schema"));
    patterns.push_back({std::string(kind->as_string()), std::string(schema->as_string())});
  }
  return patterns;
}

Result<std::vector<ActPattern>> act_patterns(
    const cbor::Value* sequence, const std::filesystem::path& source) {
  if (!sequence || !sequence->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": refracts must be a sequence"));
  std::vector<ActPattern> patterns;
  patterns.reserve(sequence->as_array()->size());
  for (const auto& entry : *sequence->as_array()) {
    if (auto checked = reject_unknown(entry, {"kind", "schema"}, source); !checked)
      return tl::unexpected(checked.error());
    const auto* kind = cbor::find(entry, "kind");
    const auto* schema = cbor::find(entry, "schema");
    if (!kind || !schema || !std::holds_alternative<std::string>(kind->data) ||
        !std::holds_alternative<std::string>(schema->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "refract pattern requires kind and schema"));
    patterns.push_back({std::string(kind->as_string()), std::string(schema->as_string())});
  }
  return patterns;
}

Result<std::vector<std::string>> strings(const cbor::Value* sequence,
                                         const std::string_view field,
                                         const std::filesystem::path& source) {
  std::vector<std::string> result;
  if (!sequence) return result;
  if (!sequence->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + ": " + std::string(field) + " must be a sequence"));
  result.reserve(sequence->as_array()->size());
  for (const auto& value : *sequence->as_array()) {
    if (!std::holds_alternative<std::string>(value.data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
          source.string() + ": " + std::string(field) +
              " entries must be strings"));
    result.emplace_back(value.as_string());
  }
  return result;
}

Result<std::vector<LensDependency>> dependencies(
    const cbor::Value* sequence, const std::filesystem::path& source) {
  std::vector<LensDependency> result;
  if (!sequence) return result;
  if (!sequence->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     source.string() + ": dependencies must be a sequence"));
  std::set<std::string> seen;
  for (const auto& item : *sequence->as_array()) {
    if (auto checked = reject_unknown(item, {"id", "version"}, source); !checked)
      return tl::unexpected(checked.error());
    const auto* id = cbor::find(item, "id");
    const auto* version = cbor::find(item, "version");
    if (!id || !std::holds_alternative<std::string>(id->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens dependency requires id"));
    LensDependency dependency{std::string(id->as_string()),
        version ? std::string(version->as_string()) : "*"};
    if (dependency.id.empty() || !seen.insert(dependency.id).second)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens dependencies must have unique non-empty ids"));
    result.push_back(std::move(dependency));
  }
  return result;
}

bool integer(const cbor::Value* value);

Result<std::vector<OpticalPortSpec>> optical_ports(
    const cbor::Value* sequence, const std::filesystem::path& source,
    const bool output) {
  if (!sequence || !sequence->as_array())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
        source.string() + (output ? ": outputs must be a sequence"
                                  : ": inputs must be a sequence")));
  std::vector<OpticalPortSpec> result;
  std::set<std::string> names;
  for (const auto& item : *sequence->as_array()) {
    if (auto checked = reject_unknown(item,
        {"port", "band", "schema", "cardinality", "required", "merge",
         "sensitivity", "maximum_trust_tier", "allowed_audiences",
         "redaction_policy", "exportable", "transient_handle",
         "max_cells", "max_cell_bytes", "surface"},
        source); !checked)
      return tl::unexpected(checked.error());
    const auto* port = cbor::find(item, "port");
    const auto* band = cbor::find(item, "band");
    const auto* schema = cbor::find(item, "schema");
    if (!port || !band || !schema)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "optical port requires port, band, and schema"));
    OpticalPortSpec spec;
    spec.name = std::string(port->as_string());
    spec.band = std::string(band->as_string());
    spec.schema = std::string(schema->as_string());
    if (const auto* field = cbor::find(item, "cardinality")) {
      auto parsed = port_cardinality_from_string(field->as_string());
      if (!parsed) return tl::unexpected(parsed.error());
      spec.cardinality = *parsed;
    }
    if (const auto* field = cbor::find(item, "required")) {
      if (!std::holds_alternative<bool>(field->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "optical port required must be boolean"));
      spec.requirement = field->as_bool() ? PortRequirement::required
                                         : PortRequirement::optional;
    }
    if (const auto* field = cbor::find(item, "merge")) {
      auto parsed = merge_law_from_string(field->as_string());
      if (!parsed) return tl::unexpected(parsed.error());
      spec.merge = *parsed;
    }
    if (const auto* field = cbor::find(item, "sensitivity")) {
      auto parsed = field_sensitivity_from_string(field->as_string());
      if (!parsed) return tl::unexpected(parsed.error());
      spec.sensitivity = *parsed;
    }
    if (const auto* field = cbor::find(item, "maximum_trust_tier")) {
      if (!integer(field) || field->as_integer() < 0 || field->as_integer() > 3)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "maximum_trust_tier must be between 0 and 3"));
      spec.maximum_trust_tier = static_cast<std::uint8_t>(field->as_integer());
    }
    if (auto audiences = strings(cbor::find(item, "allowed_audiences"),
                                 "allowed_audiences", source); audiences)
      spec.allowed_audiences = std::move(*audiences);
    else return tl::unexpected(audiences.error());
    if (const auto* field = cbor::find(item, "redaction_policy"))
      spec.redaction_policy = std::string(field->as_string());
    if (const auto* field = cbor::find(item, "exportable"))
      spec.exportable = field->as_bool(true);
    if (const auto* field = cbor::find(item, "transient_handle"))
      spec.transient_handle = field->as_bool();
    if (const auto* field = cbor::find(item, "max_cells")) {
      if (!integer(field) || field->as_integer() <= 0)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "max_cells must be positive"));
      spec.max_cells = static_cast<std::size_t>(field->as_integer());
    }
    if (const auto* field = cbor::find(item, "max_cell_bytes")) {
      if (!integer(field) || field->as_integer() <= 0)
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "max_cell_bytes must be positive"));
      spec.max_cell_bytes = static_cast<std::size_t>(field->as_integer());
    }
    if (const auto* field = cbor::find(item, "surface")) {
      if (!output || !std::holds_alternative<bool>(field->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "surface is valid only as an output boolean"));
      spec.surface = field->as_bool();
    }
    if (spec.name.empty() || spec.band.empty() || spec.schema.empty() ||
        !names.insert(spec.name).second)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "optical port names must be unique and non-empty"));
    result.push_back(std::move(spec));
  }
  return result;
}

bool integer(const cbor::Value* value) {
  return value && std::holds_alternative<std::int64_t>(value->data);
}

}  // namespace

Result<LensManifest> load_lens_manifest(const std::filesystem::path& path) {
  auto loaded = yaml::load(path);
  if (!loaded) return tl::unexpected(loaded.error());
  const auto& root = *loaded;
  if (auto checked = reject_unknown(root,
      {"api", "id", "display_name", "version", "abi", "runtime", "trust", "stateless",
       "observes", "inputs", "outputs", "trigger", "monotone", "refracts",
       "light_permissions", "dependencies",
       "conflicts", "optical_order", "resources", "replacement", "schema_bundle", "sbom"}, path);
      !checked) return tl::unexpected(checked.error());

  const auto* id = cbor::find(root, "id");
  const auto* display_name = cbor::find(root, "display_name");
  const auto* version = cbor::find(root, "version");
  const auto* abi = cbor::find(root, "abi");
  const auto* runtime = cbor::find(root, "runtime");
  const auto* observes_field = cbor::find(root, "observes");
  const auto* inputs_field = cbor::find(root, "inputs");
  const auto* outputs_field = cbor::find(root, "outputs");
  const auto* refracts_field = cbor::find(root, "refracts");
  const auto* permissions = cbor::find(root, "light_permissions");
  if (!id || !display_name || !version || !abi || !runtime || !observes_field ||
      !inputs_field || !outputs_field || !refracts_field || !permissions)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     path.string() + ": required manifest field is missing"));

  LensManifest manifest;
  manifest.id = std::string(id->as_string());
  manifest.display_name = std::string(display_name->as_string());
  manifest.version = std::string(version->as_string());
  const auto* abi_major = cbor::find(*abi, "major");
  const auto* abi_minor = cbor::find(*abi, "minor");
  if (!abi->as_map() || !integer(abi_major) || !integer(abi_minor) ||
      abi_major->as_integer() < 0 || abi_minor->as_integer() < 0 ||
      abi_major->as_integer() > static_cast<std::int64_t>(
          std::numeric_limits<std::uint32_t>::max()) ||
      abi_minor->as_integer() > static_cast<std::int64_t>(
          std::numeric_limits<std::uint32_t>::max()))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "manifest abi requires numeric major and minor"));
  manifest.abi_major = static_cast<std::uint32_t>(abi_major->as_integer());
  manifest.abi_minor = static_cast<std::uint32_t>(abi_minor->as_integer());

  const auto* runtime_kind_field = cbor::find(*runtime, "kind");
  if (!runtime->as_map() || !runtime_kind_field)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "manifest runtime.kind is required"));
  if (auto checked = reject_unknown(*runtime, {"kind", "version", "entry", "module"}, path);
      !checked) return tl::unexpected(checked.error());
  const auto runtime_text = runtime_kind_field->as_string();
  manifest.runtime = runtime_kind(runtime_text);
  if (to_string(manifest.runtime) != runtime_text)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "unknown Lens runtime kind: " + std::string(runtime_text)));
  if (const auto* field = cbor::find(*runtime, "version"))
    manifest.runtime_version = std::string(field->as_string());
  if (const auto* field = cbor::find(*runtime, "entry"))
    manifest.runtime_entry = std::string(field->as_string());
  if ((manifest.runtime == RuntimeKind::node || manifest.runtime == RuntimeKind::cpython) &&
      manifest.runtime_entry.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "language Lens manifest runtime.entry is required"));

  const auto* trust_field = cbor::find(root, "trust");
  const auto trust = trust_field ? trust_field->as_string() : std::string_view("t1");
  if (trust == "t0") manifest.trust = TrustLevel::t0;
  else if (trust == "t1") manifest.trust = TrustLevel::t1;
  else if (trust == "t2") manifest.trust = TrustLevel::t2;
  else if (trust == "t3") manifest.trust = TrustLevel::t3;
  else return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                        "unknown Lens trust level: " + std::string(trust)));
  const auto* stateless = cbor::find(root, "stateless");
  if (stateless && !std::holds_alternative<bool>(stateless->data))
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "manifest stateless must be a boolean"));
  manifest.stateless = !stateless || stateless->as_bool();
  if (const auto* api = cbor::find(root, "api");
      api && api->as_string() != "tokmon.lens/wavefront")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "unsupported Lens manifest api"));

  auto observes = photon_patterns(observes_field, path);
  auto refracts = act_patterns(refracts_field, path);
  if (!observes) return tl::unexpected(observes.error());
  if (!refracts) return tl::unexpected(refracts.error());
  manifest.observes = std::move(*observes);
  manifest.refracts = std::move(*refracts);
  auto inputs = optical_ports(inputs_field, path, false);
  auto outputs = optical_ports(outputs_field, path, true);
  auto light_permissions = strings(permissions, "light_permissions", path);
  if (!inputs) return tl::unexpected(inputs.error());
  if (!outputs) return tl::unexpected(outputs.error());
  if (!light_permissions) return tl::unexpected(light_permissions.error());
  manifest.inputs = std::move(*inputs);
  manifest.outputs = std::move(*outputs);
  if (const auto* trigger = cbor::find(root, "trigger")) {
    auto parsed = trigger_policy_from_string(trigger->as_string());
    if (!parsed) return tl::unexpected(parsed.error());
    manifest.trigger = *parsed;
  }
  if (const auto* monotone = cbor::find(root, "monotone")) {
    if (!std::holds_alternative<bool>(monotone->data))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "manifest monotone must be boolean"));
    manifest.monotone = monotone->as_bool();
  }
  manifest.light_permissions = std::move(*light_permissions);
  auto required_lenses = dependencies(cbor::find(root, "dependencies"), path);
  if (!required_lenses) return tl::unexpected(required_lenses.error());
  manifest.dependencies = std::move(*required_lenses);
  auto conflicts = strings(cbor::find(root, "conflicts"), "conflicts", path);
  if (!conflicts) return tl::unexpected(conflicts.error());
  manifest.conflicts = std::move(*conflicts);

  if (const auto* order = cbor::find(root, "optical_order")) {
    if (auto checked = reject_unknown(*order, {"before", "after"}, path); !checked)
      return tl::unexpected(checked.error());
    auto before = strings(cbor::find(*order, "before"), "optical_order.before", path);
    auto after = strings(cbor::find(*order, "after"), "optical_order.after", path);
    if (!before) return tl::unexpected(before.error());
    if (!after) return tl::unexpected(after.error());
    manifest.optical_before = std::move(*before);
    manifest.optical_after = std::move(*after);
  }
  if (const auto* resources = cbor::find(root, "resources")) {
    if (auto checked = reject_unknown(*resources,
        {"memory_mb", "output_bytes", "deadline_ms"}, path); !checked)
      return tl::unexpected(checked.error());
    const auto* memory_mb = cbor::find(*resources, "memory_mb");
    const auto* output_bytes = cbor::find(*resources, "output_bytes");
    const auto* deadline_ms = cbor::find(*resources, "deadline_ms");
    if ((memory_mb && (!integer(memory_mb) || memory_mb->as_integer() <= 0)) ||
        (output_bytes && (!integer(output_bytes) || output_bytes->as_integer() <= 0)) ||
        (deadline_ms && (!integer(deadline_ms) || deadline_ms->as_integer() <= 0)))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens resource limits must be integers"));
    if (memory_mb) manifest.resources.memory_mb = static_cast<std::size_t>(memory_mb->as_integer());
    if (output_bytes)
      manifest.resources.output_bytes = static_cast<std::size_t>(output_bytes->as_integer());
    if (deadline_ms)
      manifest.resources.deadline = std::chrono::milliseconds(deadline_ms->as_integer());
    if (manifest.resources.memory_mb == 0 || manifest.resources.memory_mb > 65'536u ||
        manifest.resources.output_bytes == 0 ||
        manifest.resources.output_bytes > 1024u * 1024u * 1024u ||
        manifest.resources.deadline <= std::chrono::milliseconds::zero() ||
        manifest.resources.deadline > std::chrono::hours(24))
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Lens resource limits are outside safe bounds"));
  }
  if (const auto* replacement = cbor::find(root, "replacement"))
    manifest.replacement = std::string(replacement->as_string());
  if (manifest.replacement != "R1" && manifest.replacement != "R2")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Lens replacement must be R1 or R2"));
  if (const auto* bundle = cbor::find(root, "schema_bundle"))
    manifest.schema_bundle = std::string(bundle->as_string());
  if (const auto* sbom = cbor::find(root, "sbom"))
    manifest.sbom = std::string(sbom->as_string());
  if (manifest.id.empty() || manifest.display_name.empty() || manifest.abi_major != 2u ||
      manifest.outputs.empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "Lens manifest identity or contract is invalid"));
  return manifest;
}

}  // namespace tokmon
