#include "lenses/nota/nota_lens.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "lenses/common/http_client.hpp"
#include "lenses/common/prometheus_endpoint.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/json.hpp"
#include "tokmon/logging.hpp"

namespace tokmon::builtin {
namespace {

cbor::Value metrics(const PhotonWindow& photons) {
  std::map<std::string, std::int64_t, std::less<>> counts;
  std::int64_t failures = 0; std::int64_t input_tokens = 0; std::int64_t output_tokens = 0;
  std::int64_t cost = 0; std::int64_t bytes = 0;
  for (const auto& photon : photons.photons()) {
    ++counts[photon.kind.substr(0, photon.kind.find('.'))];
    if (photon.kind.ends_with("failed") || photon.kind.ends_with("rejected") ||
        photon.kind.ends_with("timed-out")) ++failures;
    if (photon.kind == "model.usage") {
      if (const auto* value = cbor::find(photon.payload, "input_tokens")) input_tokens += value->as_integer();
      if (const auto* value = cbor::find(photon.payload, "output_tokens")) output_tokens += value->as_integer();
      if (const auto* value = cbor::find(photon.payload, "cost_microunits")) cost += value->as_integer();
    }
    bytes += static_cast<std::int64_t>(cbor::encode(photon.payload).size());
  }
  cbor::Value::Map families;
  for (const auto& [kind, count] : counts) families[kind] = count;
  return cbor::object({{"events_total", static_cast<std::int64_t>(photons.photons().size())},
      {"failures_total", failures}, {"input_tokens_total", input_tokens},
      {"output_tokens_total", output_tokens}, {"cost_microunits_total", cost},
      {"observed_payload_bytes", bytes}, {"families", std::move(families)},
      {"tail_sequence", photons.latest() ? static_cast<std::int64_t>(photons.latest()->sequence) : 0}});
}

cbor::Value spans(const PhotonWindow& photons) {
  cbor::Value::Array result;
  for (const auto& photon : photons.photons()) {
    if (!(photon.kind.starts_with("act.") || photon.kind.starts_with("model.") ||
          photon.kind.starts_with("worker.") || photon.kind.starts_with("workflow."))) continue;
    result.push_back(cbor::object({{"traceId", sha256_hex(photon.ray).substr(0, 32)},
        {"spanId", sha256_hex(photon.id).substr(0, 16)}, {"name", photon.kind},
        {"startTimeUnixNano", std::to_string(photon.committed_at_ms * 1'000'000)},
        {"endTimeUnixNano", std::to_string(photon.committed_at_ms * 1'000'000)},
        {"attributes", cbor::Value::Array{
            cbor::object({{"key", "tokmon.ray"},
                          {"value", cbor::object({{"stringValue", photon.ray}})}}),
            cbor::object({{"key", "tokmon.epoch"}, {"value", cbor::object({
                          {"intValue", std::to_string(photon.epoch)}})}})}}}));
  }
  cbor::Value::Array resource_attributes;
  resource_attributes.push_back(cbor::object({{"key", "service.name"},
      {"value", cbor::object({{"stringValue", "tokmon"}})}}));
  auto scope = cbor::object({{"scope", cbor::object({{"name", "org.tokmon.lens.nota"}})},
                             {"spans", std::move(result)}});
  cbor::Value::Array scope_spans;
  scope_spans.push_back(std::move(scope));
  auto resource = cbor::object({
      {"resource", cbor::object({{"attributes", std::move(resource_attributes)}})},
      {"scopeSpans", std::move(scope_spans)}});
  cbor::Value::Array resources;
  resources.push_back(std::move(resource));
  return cbor::object({{"resourceSpans", std::move(resources)}});
}

cbor::Value otlp_metrics(const PhotonWindow& photons) {
  const auto snapshot = metrics(photons);
  cbor::Value::Array encoded;
  for (const auto* name : {"events_total", "failures_total", "input_tokens_total",
                           "output_tokens_total", "cost_microunits_total",
                           "observed_payload_bytes"}) {
    const auto* value = cbor::find(snapshot, name);
    encoded.push_back(cbor::object({{"name", std::string("tokmon.") + name},
        {"unit", "1"}, {"sum", cbor::object({
          {"aggregationTemporality", 2}, {"isMonotonic", true},
          {"dataPoints", cbor::Value::Array{cbor::object({
            {"asInt", std::to_string(value ? value->as_integer() : 0)},
            {"timeUnixNano", std::to_string((photons.latest()
                ? photons.latest()->committed_at_ms : 0) * 1'000'000)}})}}})}}));
  }
  cbor::Value::Array attributes;
  attributes.push_back(cbor::object({{"key", "service.name"},
      {"value", cbor::object({{"stringValue", "tokmon"}})}}));
  cbor::Value::Array scope_metrics;
  scope_metrics.push_back(cbor::object({
      {"scope", cbor::object({{"name", "org.tokmon.lens.nota"}})},
      {"metrics", std::move(encoded)}}));
  cbor::Value::Array resources;
  resources.push_back(cbor::object({
      {"resource", cbor::object({{"attributes", std::move(attributes)}})},
      {"scopeMetrics", std::move(scope_metrics)}}));
  return cbor::object({{"resourceMetrics", std::move(resources)}});
}

std::string prometheus_metrics(const PhotonWindow& photons) {
  const auto snapshot = metrics(photons);
  std::string output;
  for (const auto* name : {"events_total", "failures_total", "input_tokens_total",
                           "output_tokens_total", "cost_microunits_total",
                           "observed_payload_bytes"}) {
    const auto* value = cbor::find(snapshot, name);
    output += "# TYPE tokmon_" + std::string(name) + " counter\n";
    output += "tokmon_" + std::string(name) + " " +
        std::to_string(value ? value->as_integer() : 0) + "\n";
  }
  return output;
}

Result<std::filesystem::path> write_artifact(const cbor::Value& parameters,
                                             const std::string_view child,
                                             const std::string_view extension,
                                             const std::string& content) {
  const auto* root = cbor::find(parameters, "storage_root");
  if (!root || root->as_string().empty())
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "diagnostic artifact requires storage_root"));
  std::error_code error;
  const auto directory = std::filesystem::absolute(
      std::filesystem::path(root->as_string())) / child;
  std::filesystem::create_directories(directory, error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot create diagnostic directory"));
  const auto path = directory / (sha256_hex(content) + std::string(extension));
  if (!std::filesystem::exists(path)) {
    std::ofstream output(path, std::ios::binary);
    output << content;
    output.flush();
    if (!output) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot write diagnostic artifact"));
  }
  return path;
}

}  // namespace

struct NotaLens::Impl {
  std::mutex mutex;
  std::string prometheus;
  PrometheusEndpoint endpoint;
};

NotaLens::NotaLens() : LensBase(make_manifest("nota", "Nota / 可观测性光谱分析仪",
    {"diagnostic.metrics", "diagnostic.health", "ui.diagnostics"},
    {{"act.*", "*"}, {"lens.*", "*"}, {"worker.*", "*"}, {"model.*", "*"},
     {"workflow.*", "*"}, {"waveguide.*", "*"}, {"system.*", "*"}},
    {{"telemetry.export", "tokmon.telemetry.export.v1"},
     {"telemetry.configure", "tokmon.telemetry.configure.v1"},
     {"telemetry.serve", "tokmon.telemetry.serve.v1"},
     {"profile.capture", "tokmon.profile.capture.v1"},
     {"diagnostic.bundle", "tokmon.diagnostic.bundle.v1"}},
    {"photon.emit", "telemetry.network", "artifact.write", "log.write"})),
    impl_(std::make_unique<Impl>()) {
  mark_stateful();
}

NotaLens::~NotaLens() { impl_->endpoint.stop(); }

void NotaLens::request_stop() noexcept {
  impl_->endpoint.stop();
  LensBase::request_stop();
}

Result<void> NotaLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->prometheus = prometheus_metrics(photons.photon_window());
  }
  auto snapshot = metrics(photons.photon_window());
  const auto failures = cbor::find(snapshot, "failures_total")->as_integer();
  if (auto result = surface.add("diagnostic.metrics", "active-ray", snapshot, 0); !result)
    return result;
  if (auto result = identify(surface, "diagnostic.health", cbor::object({
      {"healthy", failures == 0}, {"recovery_source", false},
      {"exporter_blocks_commit", false}, {"bounded_queue", true},
      {"payload_captured", false}, {"prometheus_endpoint", impl_->endpoint.running()},
      {"prometheus_port", static_cast<std::int64_t>(impl_->endpoint.port())}})); !result)
    return result;
  return surface.add("ui.diagnostics", "active-ray", cbor::object({
      {"status", failures == 0 ? "healthy" : "degraded"},
      {"metrics", std::move(snapshot)}, {"sensitive_payloads", false}}), 10);
}

Result<RefractionResult> NotaLens::refract(const PhotonWindow& photons, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  if (act.kind == "telemetry.serve") {
    const auto* operation_value = cbor::find(act.parameters, "operation");
    const auto operation = operation_value
        ? std::string(operation_value->as_string("start")) : std::string("start");
    if (operation == "stop") {
      const auto old_port = impl_->endpoint.port();
      impl_->endpoint.stop();
      return emit(beam, "telemetry.endpoint-stopped", "tokmon.telemetry.endpoint.v1",
          cbor::object({{"host", "127.0.0.1"},
            {"port", static_cast<std::int64_t>(old_port)},
            {"history_deleted", false}}));
    }
    if (operation != "start")
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "telemetry.serve operation must be start or stop"));
    if (const auto* host = cbor::find(act.parameters, "host"); host &&
        host->as_string() != "127.0.0.1" && host->as_string() != "localhost")
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "Prometheus endpoint is loopback-only"));
    const auto configured_port = cbor::find(act.parameters, "port")
        ? cbor::find(act.parameters, "port")->as_integer() : 0;
    if (configured_port < 0 || configured_port > 65'535)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "Prometheus port is outside 0..65535"));
    {
      std::scoped_lock lock(impl_->mutex);
      impl_->prometheus = prometheus_metrics(photons);
    }
    auto started = impl_->endpoint.start(static_cast<std::uint16_t>(configured_port), [this] {
      std::scoped_lock lock(impl_->mutex);
      return impl_->prometheus;
    });
    if (!started) return tl::unexpected(started.error());
    return emit(beam, "telemetry.endpoint-started", "tokmon.telemetry.endpoint.v1",
        cbor::object({{"host", "127.0.0.1"},
          {"port", static_cast<std::int64_t>(*started)},
          {"url", "http://127.0.0.1:" + std::to_string(*started) + "/metrics"},
          {"format", "prometheus"}, {"loopback_only", true},
          {"exporter_blocks_commit", false}}));
  }
  if (act.kind == "telemetry.configure")
    return emit(beam, "telemetry.configuration-observed", "tokmon.telemetry.config.v1",
        cbor::object({{"level", cbor::find(act.parameters, "level")
            ? *cbor::find(act.parameters, "level") : cbor::Value("info")},
          {"sampling", cbor::find(act.parameters, "sampling")
            ? *cbor::find(act.parameters, "sampling") : cbor::Value(1.0)},
          {"auditable", true}}));

  const auto signal = cbor::find(act.parameters, "signal")
      ? std::string(cbor::find(act.parameters, "signal")->as_string("traces")) : "traces";
  const auto format = cbor::find(act.parameters, "format")
      ? std::string(cbor::find(act.parameters, "format")->as_string("otlp-json")) : "otlp-json";
  if (act.kind == "telemetry.export" && signal != "traces" && signal != "metrics")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "telemetry signal must be traces or metrics"));
  if (act.kind == "telemetry.export" && format != "otlp-json" && format != "prometheus")
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "telemetry format must be otlp-json or prometheus"));
  auto document = act.kind == "telemetry.export"
      ? (signal == "metrics" ? otlp_metrics(photons) : spans(photons)) :
      cbor::object({{"metrics", metrics(photons)},
        {"events", act.kind == "diagnostic.bundle" ? redact_value(to_cbor(photons))
                                                     : cbor::Value(nullptr)},
        {"profile_kind", act.kind == "profile.capture" ? "event-distribution" : "none"},
        {"secrets_included", false}, {"payloads_redacted", true}});
  const auto body = format == "prometheus" ? prometheus_metrics(photons)
                                             : json::stringify(document);
  if (act.kind == "telemetry.export") {
    if (const auto* endpoint = cbor::find(act.parameters, "endpoint");
        endpoint && !endpoint->as_string().empty()) {
      auto response = perform_http(HttpRequest{.url = std::string(endpoint->as_string()),
          .headers = {{"Content-Type", format == "prometheus"
              ? "text/plain; version=0.0.4" : "application/json"}}, .body = body,
          .timeout = act.timeout, .max_response_bytes = 256u * 1024u,
          .cwd = std::filesystem::current_path(), .stop = beam.stop_token()});
      if (!response) return tl::unexpected(response.error());
      if (response->status < 200 || response->status >= 300)
        return tl::unexpected(make_error(ErrorCode::io_error,
            "OTLP endpoint returned HTTP " + std::to_string(response->status), true));
      return emit(beam, "telemetry.exported", "tokmon.telemetry.result.v1",
          cbor::object({{"endpoint", std::string(endpoint->as_string())},
            {"status", response->status}, {"bytes", static_cast<std::int64_t>(body.size())},
            {"signal", signal}, {"format", format}, {"payloads_included", false}}));
    }
  }
  auto path = write_artifact(act.parameters,
      act.kind == "profile.capture" ? "profiles" :
      act.kind == "diagnostic.bundle" ? "diagnostics" : "telemetry",
      format == "prometheus" ? ".prom" : ".json", body);
  if (!path) return tl::unexpected(path.error());
  const auto kind = act.kind == "telemetry.export" ? "telemetry.exported" :
      act.kind == "profile.capture" ? "profile.captured" : "diagnostic.bundle-created";
  return emit(beam, kind, "tokmon.diagnostic.result.v1", cbor::object({
      {"path", path->generic_string()}, {"sha256", sha256_hex(body)},
      {"bytes", static_cast<std::int64_t>(body.size())}, {"secrets_included", false},
      {"fact_source", false}}));
}

}  // namespace tokmon::builtin
