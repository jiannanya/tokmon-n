#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "tokmon/tokmon.hpp"

namespace {
std::atomic_bool running{true};
void stop_signal(int) { running.store(false, std::memory_order_release); }

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
}

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> workspace;
  bool once = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--once") once = true;
    else if (argument == "--workspace" && index + 1 < argc) workspace = argv[++index];
  }
  std::signal(SIGINT, stop_signal);
  std::signal(SIGTERM, stop_signal);
  tokmon::TokmonRuntime runtime;
  if (auto opened = runtime.open(workspace, "tokmond"); !opened) {
    std::cerr << opened.error().describe() << '\n'; return 2;
  }
  if (once) return runtime.verify() ? 0 : 1;

  std::mutex runtime_mutex;
  tokmon::SnowServer snow;
  const auto endpoint = tokmon::default_snow_endpoint(runtime.config().paths.run);
  auto snow_started = snow.start(endpoint, [&runtime, &runtime_mutex](
      const tokmon::SnowMessage& request) -> tokmon::SnowMessage {
    std::scoped_lock lock(runtime_mutex);
    if (request.kind == tokmon::SnowMessageKind::hello) {
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::hello,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"service", "tokmond"},
              {"protocol_major", tokmon::snow_protocol_major},
              {"protocol_minor", tokmon::snow_protocol_minor}})};
    }
    if (request.kind == tokmon::SnowMessageKind::ping) {
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::pong,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"healthy", true}})};
    }
    if (request.kind == tokmon::SnowMessageKind::snapshot_request) {
      auto photons = runtime.history_all(request.cursor);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::snapshot,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"photons", photon_array(*photons)},
              {"light_path_epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"light_path_hash", runtime.light_path()->hash}})};
    }
    if (request.kind != tokmon::SnowMessageKind::intent)
      return snow_error(request, tokmon::make_error(tokmon::ErrorCode::protocol_error,
                                                    "unsupported Snow request"));
    const auto* action_field = tokmon::cbor::find(request.payload, "action");
    const auto action = action_field ? action_field->as_string() : std::string_view{};
    if (action == "chat") {
      const auto* text_field = tokmon::cbor::find(request.payload, "text");
      const auto text = text_field ? std::string(text_field->as_string()) : std::string{};
      if (text.empty())
        return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                      "chat text is required"));
      auto ray = runtime.submit(text);
      if (!ray) return snow_error(request, ray.error());
      auto advanced = runtime.advance(*ray);
      if (!advanced) return snow_error(request, advanced.error());
      auto photons = runtime.history(*ray);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"ray", *ray},
              {"beats", static_cast<std::int64_t>(*advanced)},
              {"photons", photon_array(*photons)}})};
    }
    if (action == "history") {
      const auto* ray_field = tokmon::cbor::find(request.payload, "ray");
      tokmon::Result<std::vector<tokmon::Photon>> photons =
          ray_field && !ray_field->as_string().empty()
              ? runtime.history(std::string(ray_field->as_string()))
              : runtime.history_all(request.cursor);
      if (!photons) return snow_error(request, photons.error());
      const auto cursor = photons->empty() ? request.cursor : photons->back().sequence;
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = cursor,
          .payload = tokmon::cbor::object({{"photons", photon_array(*photons)}})};
    }
    if (action == "lens.reconcile") {
      auto result = runtime.reconcile();
      if (!result) return snow_error(request, result.error());
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({
              {"epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"hash", runtime.light_path()->hash}})};
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
      return tokmon::SnowMessage{.kind = tokmon::SnowMessageKind::intent_result,
          .request_id = request.request_id, .cursor = request.cursor,
          .payload = tokmon::cbor::object({{"verified", true},
              {"epoch", static_cast<std::int64_t>(runtime.light_path()->epoch)},
              {"hash", runtime.light_path()->hash}, {"lenses", std::move(lenses)}})};
    }
    return snow_error(request, tokmon::make_error(tokmon::ErrorCode::invalid_argument,
                                                  "unknown intent action"));
  });
  if (!snow_started) {
    std::cerr << snow_started.error().describe() << '\n'; return 2;
  }
  spdlog::info("Snow endpoint listening at {}", endpoint.string());

  auto user_config = runtime.config().paths.user / "light-path.yaml";
  auto project_config = runtime.config().paths.project / "light-path.yaml";
  auto user_time = std::filesystem::file_time_type::min();
  auto project_time = std::filesystem::file_time_type::min();
  while (running.load(std::memory_order_acquire)) {
    std::error_code error;
    const auto next_user = std::filesystem::exists(user_config, error)
        ? std::filesystem::last_write_time(user_config, error) : std::filesystem::file_time_type::min();
    const auto next_project = std::filesystem::exists(project_config, error)
        ? std::filesystem::last_write_time(project_config, error) : std::filesystem::file_time_type::min();
    if ((user_time != std::filesystem::file_time_type::min() && next_user != user_time) ||
        (project_time != std::filesystem::file_time_type::min() && next_project != project_time)) {
      std::scoped_lock lock(runtime_mutex);
      if (auto result = runtime.reconcile(); !result)
        spdlog::error("light-path reconcile rejected: {}", result.error().describe());
    }
    user_time = next_user; project_time = next_project;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  snow.stop();
  runtime.stop();
  return 0;
}
