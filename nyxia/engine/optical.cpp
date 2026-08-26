#include "tokmon/optical.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "tokmon/hash.hpp"
#include "tokmon/logging.hpp"
#include "tokmon/photon.hpp"
#include "tokmon/surface.hpp"

namespace tokmon {
namespace {

thread_local bool inside_optical_provider = false;

const cbor::Value null_state{};

struct ProviderRuntime {
  OpticalQueryCapability capability;
  FrozenLensState state;
  std::shared_ptr<IOpticalLensExtension> extension;
  std::atomic_size_t active{0};
  std::atomic_size_t calls{0};
};

struct CachedQuery {
  cbor::Value response;
  std::string response_hash;
};

bool preserves_optical_error(const ErrorCode code) {
  switch (code) {
    case ErrorCode::provider_not_found:
    case ErrorCode::ambiguous_provider:
    case ErrorCode::schema_mismatch:
    case ErrorCode::deadline_exceeded:
    case ErrorCode::budget_exceeded:
    case ErrorCode::provider_failed:
    case ErrorCode::stale_generation:
    case ErrorCode::recursive_query_denied:
    case ErrorCode::nondeterministic_result:
      return true;
    default:
      return false;
  }
}

bool contains_sensitive_value(const cbor::Value& value) {
  if (const auto* map = value.as_map()) {
    for (const auto& [raw_key, child] : *map) {
      auto key = raw_key;
      std::ranges::transform(key, key.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      if (key == "secret_value" || key == "api_key" || key == "apikey" ||
          key == "password" || key == "authorization" || key == "access_token" ||
          key == "refresh_token" || key == "credential")
        return true;
      if (contains_sensitive_value(child)) return true;
    }
  } else if (const auto* array = value.as_array()) {
    if (std::ranges::any_of(*array, contains_sensitive_value)) return true;
  } else if (const auto* text = std::get_if<std::string>(&value.data)) {
    return redact(*text) != *text;
  }
  return false;
}

std::string cache_key(const BeatMetadata& metadata, const ProviderRuntime& provider,
                      const std::string_view request_hash) {
  return sha256_hex(metadata.beat + "\n" + metadata.path_hash + "\n" +
      provider.state.lens + "\n" + std::to_string(provider.state.generation) + "\n" +
      provider.capability.capability + "\n" + provider.capability.request_schema + "\n" +
      std::string(request_hash));
}

Error provider_error(const ErrorCode code, std::string message,
                     const ProviderRuntime* provider = nullptr) {
  auto error = make_error(code, std::move(message));
  if (provider) error.lens = provider->state.lens;
  return error;
}

struct ActiveGuard {
  std::shared_ptr<ProviderRuntime> provider;
  ~ActiveGuard() {
    if (provider) provider->active.fetch_sub(1, std::memory_order_acq_rel);
  }
};

struct RecursionGuard {
  bool previous{inside_optical_provider};
  RecursionGuard() { inside_optical_provider = true; }
  ~RecursionGuard() { inside_optical_provider = previous; }
};

}  // namespace

bool QueryBudget::expired() const noexcept {
  return std::chrono::steady_clock::now() >= deadline;
}

const cbor::Value& FrozenLensState::data() const noexcept {
  return value ? *value : null_state;
}

cbor::Value to_cbor(const QueryTrace& trace) {
  return cbor::object({
      {"beat", trace.beat}, {"ray", trace.ray}, {"consumer", trace.consumer},
      {"consumer_generation", static_cast<std::int64_t>(trace.consumer_generation)},
      {"provider", trace.provider},
      {"provider_generation", static_cast<std::int64_t>(trace.provider_generation)},
      {"capability", trace.capability}, {"request_schema", trace.request_schema},
      {"response_schema", trace.response_schema}, {"request_hash", trace.request_hash},
      {"response_hash", trace.response_hash}, {"cache_hit", trace.cache_hit},
      {"duration_us", trace.duration_us}, {"status", trace.status}});
}

std::string_view to_string(const OpticalQueryCardinality value) noexcept {
  switch (value) {
    case OpticalQueryCardinality::single: return "single";
    case OpticalQueryCardinality::optional_single: return "optional-single";
    case OpticalQueryCardinality::many: return "many";
  }
  return "optional-single";
}

std::string_view to_string(const OpticalQueryMerge value) noexcept {
  switch (value) {
    case OpticalQueryMerge::first: return "first";
    case OpticalQueryMerge::all: return "all";
    case OpticalQueryMerge::priority_then_path: return "priority_then_path";
  }
  return "first";
}

std::string_view to_string(const OpticalQueryCache value) noexcept {
  return value == OpticalQueryCache::per_beat ? "per_beat" : "none";
}

bool IOpticalLensExtension::supports_derive() const noexcept { return false; }
bool IOpticalLensExtension::supports_coordinate() const noexcept { return false; }
bool IOpticalLensExtension::supports_query() const noexcept { return false; }

Result<cbor::Value> IOpticalLensExtension::derive(const PhotonWindow&) {
  return cbor::Value{};
}

Result<void> IOpticalLensExtension::coordinate(const PhotonWindow&,
                                                const OpticalContext&,
                                                SurfaceBuilder&) {
  return {};
}

Result<cbor::Value> IOpticalLensExtension::optical_query(
    const FrozenLensState&, const std::string_view, const cbor::Value&,
    const QueryBudget&) const {
  return tl::unexpected(make_error(ErrorCode::unsupported,
                                    "Lens has no optical query handler"));
}

struct BeatBoardBuilder::Impl {
  struct Published {
    FrozenLensState state;
    std::vector<OpticalQueryCapability> capabilities;
    std::shared_ptr<IOpticalLensExtension> extension;
  };
  BeatMetadata metadata;
  std::vector<Published> published;
  bool frozen{false};
};

struct BeatBoardSnapshot::Impl {
  BeatMetadata metadata;
  std::vector<FrozenLensState> states;
  std::vector<SurfaceContribution> contributions;
  std::unordered_map<LensId, std::size_t> path_indices;
  std::vector<std::shared_ptr<ProviderRuntime>> providers;
  mutable std::mutex mutex;
  mutable std::unordered_map<std::string, CachedQuery> cache;
  mutable std::unordered_map<std::string, std::string> deterministic_hashes;
  mutable std::unordered_map<std::string, std::size_t> consumer_calls;
  mutable std::vector<QueryTrace> traces;
};

BeatBoardBuilder::BeatBoardBuilder(BeatMetadata metadata)
    : impl_(std::make_unique<Impl>()) {
  impl_->metadata = std::move(metadata);
}

BeatBoardBuilder::~BeatBoardBuilder() = default;
BeatBoardBuilder::BeatBoardBuilder(BeatBoardBuilder&&) noexcept = default;
BeatBoardBuilder& BeatBoardBuilder::operator=(BeatBoardBuilder&&) noexcept = default;

Result<void> BeatBoardBuilder::publish(
    LensId lens, std::string artifact_hash, const GenerationId generation,
    const std::size_t path_index,
    std::vector<OpticalQueryCapability> capabilities,
    std::shared_ptr<IOpticalLensExtension> extension, cbor::Value state) {
  if (!impl_ || impl_->frozen)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "BeatBoard is already frozen"));
  if (lens.empty() || generation == 0)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "frozen Lens identity is incomplete"));
  if (!capabilities.empty() && (!extension || !extension->supports_query()))
    return tl::unexpected(make_error(ErrorCode::unsupported,
        lens + " declares optical queries without a query extension"));
  auto frozen = FrozenLensState{.lens = std::move(lens),
      .artifact_hash = std::move(artifact_hash), .epoch = impl_->metadata.epoch,
      .generation = generation, .path_index = path_index,
      .value = std::make_shared<const cbor::Value>(std::move(state))};
  impl_->published.push_back(Impl::Published{std::move(frozen),
                                             std::move(capabilities),
                                             std::move(extension)});
  return {};
}

Result<std::shared_ptr<const BeatBoardSnapshot>> BeatBoardBuilder::freeze(
    const SurfaceSnapshot& surface) && {
  if (!impl_ || impl_->frozen)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "BeatBoard can only be frozen once"));
  impl_->frozen = true;
  auto board = std::shared_ptr<BeatBoardSnapshot>(new BeatBoardSnapshot());
  board->impl_->metadata = std::move(impl_->metadata);
  board->impl_->contributions = surface.contributions;
  for (auto& item : impl_->published) {
    board->impl_->path_indices[item.state.lens] = item.state.path_index;
    board->impl_->states.push_back(item.state);
    for (auto& declaration : item.capabilities) {
      auto provider = std::make_shared<ProviderRuntime>();
      provider->capability = std::move(declaration);
      provider->state = item.state;
      provider->extension = item.extension;
      board->impl_->providers.push_back(std::move(provider));
    }
  }
  std::stable_sort(board->impl_->providers.begin(), board->impl_->providers.end(),
      [](const auto& left, const auto& right) {
        if (left->capability.capability != right->capability.capability)
          return left->capability.capability < right->capability.capability;
        if (left->capability.priority != right->capability.priority)
          return left->capability.priority > right->capability.priority;
        return left->state.path_index < right->state.path_index;
      });
  return std::shared_ptr<const BeatBoardSnapshot>(std::move(board));
}

BeatBoardSnapshot::BeatBoardSnapshot() : impl_(std::make_unique<Impl>()) {}
BeatBoardSnapshot::~BeatBoardSnapshot() = default;

const BeatMetadata& BeatBoardSnapshot::metadata() const noexcept {
  return impl_->metadata;
}

std::vector<QueryTrace> BeatBoardSnapshot::traces() const {
  std::scoped_lock lock(impl_->mutex);
  return impl_->traces;
}

struct OpticalContext::Impl {
  std::shared_ptr<const BeatBoardSnapshot> board;
  LensId consumer;
  GenerationId generation{0};
  std::vector<OpticalQueryConsumption> consumptions;
  GetCallback get_callback;
  GetAllCallback get_all_callback;
  QueryCallback query_callback;
};

OpticalContext::OpticalContext(
    std::shared_ptr<const BeatBoardSnapshot> board, LensId consumer,
    const GenerationId generation,
    std::vector<OpticalQueryConsumption> consumptions)
    : impl_(std::make_shared<Impl>()) {
  impl_->board = std::move(board);
  impl_->consumer = std::move(consumer);
  impl_->generation = generation;
  impl_->consumptions = std::move(consumptions);
}

OpticalContext OpticalContext::from_callbacks(
    GetCallback get, GetAllCallback get_all, QueryCallback query) {
  OpticalContext context(nullptr, {}, 0, {});
  context.impl_->get_callback = std::move(get);
  context.impl_->get_all_callback = std::move(get_all);
  context.impl_->query_callback = std::move(query);
  return context;
}

Result<cbor::Value> OpticalContext::get(const std::string_view channel,
                                        const std::string_view key) const {
  if (inside_optical_provider)
    return tl::unexpected(make_error(ErrorCode::recursive_query_denied,
        "optical access from a query handler is forbidden"));
  if (impl_->get_callback) return impl_->get_callback(channel, key);
  if (!impl_->board)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "OpticalContext has no BeatBoard"));
  const auto& board = *impl_->board->impl_;
  const SurfaceContribution* selected = nullptr;
  std::size_t selected_path = 0;
  for (const auto& contribution : board.contributions) {
    if (contribution.channel != channel || contribution.key != key) continue;
    const auto found = board.path_indices.find(contribution.lens);
    const auto path_index = found == board.path_indices.end() ?
        static_cast<std::size_t>(-1) : found->second;
    if (!selected || contribution.priority > selected->priority ||
        (contribution.priority == selected->priority && path_index < selected_path)) {
      selected = &contribution;
      selected_path = path_index;
    }
  }
  if (!selected)
    return tl::unexpected(make_error(ErrorCode::not_found,
        "BeatBoard value was not found: " + std::string(channel) + "/" +
        std::string(key)));
  return selected->value;
}

Result<std::vector<cbor::Value>> OpticalContext::get_all(
    const std::string_view channel) const {
  if (inside_optical_provider)
    return tl::unexpected(make_error(ErrorCode::recursive_query_denied,
        "optical access from a query handler is forbidden"));
  if (impl_->get_all_callback) return impl_->get_all_callback(channel);
  if (!impl_->board)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "OpticalContext has no BeatBoard"));
  struct Selected {
    const SurfaceContribution* contribution;
    std::size_t path_index;
    std::size_t insertion;
  };
  std::vector<Selected> selected;
  const auto& board = *impl_->board->impl_;
  for (std::size_t index = 0; index < board.contributions.size(); ++index) {
    const auto& contribution = board.contributions[index];
    if (contribution.channel != channel) continue;
    const auto found = board.path_indices.find(contribution.lens);
    selected.push_back(Selected{&contribution,
        found == board.path_indices.end() ? static_cast<std::size_t>(-1) : found->second,
        index});
  }
  std::stable_sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
    if (left.contribution->priority != right.contribution->priority)
      return left.contribution->priority > right.contribution->priority;
    if (left.path_index != right.path_index) return left.path_index < right.path_index;
    return left.insertion < right.insertion;
  });
  std::vector<cbor::Value> values;
  values.reserve(selected.size());
  for (const auto& item : selected) values.push_back(item.contribution->value);
  return values;
}

Result<cbor::Value> OpticalContext::query(OpticalQueryRequest request) const {
  if (inside_optical_provider)
    return tl::unexpected(make_error(ErrorCode::recursive_query_denied,
        "nested cross-Lens optical query denied"));
  if (impl_->query_callback) return impl_->query_callback(std::move(request));
  if (!impl_->board)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "OpticalContext has no BeatBoard"));
  if (request.capability.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "optical query capability is required"));

  const auto& board = *impl_->board->impl_;
  const auto consumer_state = std::find_if(board.states.begin(), board.states.end(),
      [&](const FrozenLensState& state) {
        return state.lens == impl_->consumer && state.generation == impl_->generation;
      });
  if (consumer_state == board.states.end())
    return tl::unexpected(make_error(ErrorCode::stale_generation,
                                     "query consumer is not part of this BeatBoard"));
  const auto consumption = std::find_if(impl_->consumptions.begin(),
      impl_->consumptions.end(), [&](const OpticalQueryConsumption& item) {
        return item.capability == request.capability;
      });
  if (consumption == impl_->consumptions.end())
    return tl::unexpected(make_error(ErrorCode::permission_denied,
        impl_->consumer + " did not declare consumption of " + request.capability));

  std::vector<std::shared_ptr<ProviderRuntime>> providers;
  for (const auto& provider : board.providers)
    if (provider->capability.capability == request.capability)
      providers.push_back(provider);
  if (providers.empty())
    return tl::unexpected(make_error(ErrorCode::provider_not_found,
                                     "optical query provider not found: " +
                                     request.capability));
  if ((consumption->cardinality == OpticalQueryCardinality::single ||
       consumption->cardinality == OpticalQueryCardinality::optional_single) &&
      providers.size() != 1u)
    return tl::unexpected(make_error(ErrorCode::ambiguous_provider,
        "optical query requires one provider: " + request.capability));
  std::stable_sort(providers.begin(), providers.end(), [](const auto& left,
                                                           const auto& right) {
    return left->state.path_index < right->state.path_index;
  });
  if (consumption->merge == OpticalQueryMerge::priority_then_path)
    std::stable_sort(providers.begin(), providers.end(), [](const auto& left,
                                                             const auto& right) {
      if (left->capability.priority != right->capability.priority)
        return left->capability.priority > right->capability.priority;
      return left->state.path_index < right->state.path_index;
    });
  if (consumption->merge == OpticalQueryMerge::first && providers.size() > 1u)
    providers.resize(1u);

  const auto invoke = [&](const std::shared_ptr<ProviderRuntime>& provider)
      -> Result<cbor::Value> {
    const auto started = std::chrono::steady_clock::now();
    QueryTrace trace{.beat = board.metadata.beat, .ray = board.metadata.ray,
        .consumer = impl_->consumer, .consumer_generation = impl_->generation,
        .provider = provider->state.lens,
        .provider_generation = provider->state.generation,
        .capability = request.capability,
        .request_schema = provider->capability.request_schema,
        .response_schema = provider->capability.response_schema};
    const auto finish = [&](const std::string_view status, const bool cache_hit = false) {
      trace.status = status;
      trace.cache_hit = cache_hit;
      trace.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count();
      std::scoped_lock lock(board.mutex);
      board.traces.push_back(trace);
    };

    if ((!request.request_schema.empty() &&
         request.request_schema != provider->capability.request_schema) ||
        (!request.response_schema.empty() &&
         request.response_schema != provider->capability.response_schema)) {
      finish("schema_mismatch");
      return tl::unexpected(provider_error(ErrorCode::schema_mismatch,
          "optical query schema does not match provider manifest", provider.get()));
    }
    const auto encoded_request = cbor::encode(request.parameters);
    trace.request_hash = sha256_hex(encoded_request);
    if (encoded_request.size() > provider->capability.max_request_bytes) {
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical query request exceeds byte budget", provider.get()));
    }
    bool consumer_budget_exhausted = false;
    {
      std::scoped_lock lock(board.mutex);
      auto& count = board.consumer_calls[impl_->consumer + "\n" +
          std::to_string(impl_->generation) + "\n" + provider->state.lens + "\n" +
          provider->capability.capability];
      consumer_budget_exhausted = ++count > provider->capability.max_queries_per_beat;
    }
    if (consumer_budget_exhausted) {
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical consumer/provider call budget exhausted", provider.get()));
    }
    const auto call_index = provider->calls.fetch_add(1, std::memory_order_acq_rel) + 1u;
    if (call_index > provider->capability.max_queries_per_beat) {
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical query call budget exhausted", provider.get()));
    }
    const auto key = cache_key(board.metadata, *provider, trace.request_hash);
    if (provider->capability.cache == OpticalQueryCache::per_beat) {
      std::scoped_lock lock(board.mutex);
      if (const auto cached = board.cache.find(key); cached != board.cache.end()) {
        trace.response_hash = cached->second.response_hash;
        trace.status = "ok";
        trace.cache_hit = true;
        trace.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        board.traces.push_back(trace);
        return cached->second.response;
      }
    }
    const auto active = provider->active.fetch_add(1, std::memory_order_acq_rel) + 1u;
    if (active > provider->capability.max_concurrent_queries) {
      provider->active.fetch_sub(1, std::memory_order_acq_rel);
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical provider concurrency budget exhausted", provider.get()));
    }
    auto timeout = request.timeout.count() == 0 ? provider->capability.default_timeout :
                                                  request.timeout;
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout > provider->capability.max_timeout) {
      provider->active.fetch_sub(1, std::memory_order_acq_rel);
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical query timeout is outside provider budget", provider.get()));
    }
    const auto response_limit = request.max_response_bytes == 0 ?
        provider->capability.max_response_bytes :
        std::min(request.max_response_bytes, provider->capability.max_response_bytes);
    const QueryBudget budget{.deadline = started + timeout,
        .max_request_bytes = provider->capability.max_request_bytes,
        .max_response_bytes = response_limit, .call_index = call_index};

    auto promise = std::make_shared<std::promise<Result<cbor::Value>>>();
    auto future = promise->get_future();
    std::thread([provider, parameters = request.parameters, budget, promise]() mutable {
      ActiveGuard active_guard{provider};
      RecursionGuard recursion_guard;
      try {
        promise->set_value(provider->extension->optical_query(
            provider->state, provider->capability.capability, parameters, budget));
      } catch (const std::exception& exception) {
        promise->set_value(tl::unexpected(make_error(ErrorCode::provider_failed,
            "optical provider exception: " + std::string(exception.what()))));
      } catch (...) {
        promise->set_value(tl::unexpected(make_error(ErrorCode::provider_failed,
            "optical provider raised an unknown exception")));
      }
    }).detach();
    if (future.wait_until(budget.deadline) != std::future_status::ready) {
      finish("deadline_exceeded");
      return tl::unexpected(provider_error(ErrorCode::deadline_exceeded,
          "optical query deadline exceeded", provider.get()));
    }
    auto response = future.get();
    if (!response) {
      auto failure = response.error();
      if (!preserves_optical_error(failure.code))
        failure.code = ErrorCode::provider_failed;
      failure.lens = provider->state.lens;
      finish(to_string(failure.code));
      return tl::unexpected(std::move(failure));
    }
    if (budget.expired()) {
      finish("deadline_exceeded");
      return tl::unexpected(provider_error(ErrorCode::deadline_exceeded,
          "optical provider returned after its deadline", provider.get()));
    }
    if (contains_sensitive_value(*response)) {
      finish("provider_failed");
      return tl::unexpected(provider_error(ErrorCode::provider_failed,
          "optical provider response contains sensitive material", provider.get()));
    }
    const auto encoded_response = cbor::encode(*response);
    if (encoded_response.size() > response_limit) {
      finish("budget_exceeded");
      return tl::unexpected(provider_error(ErrorCode::budget_exceeded,
          "optical query response exceeds byte budget", provider.get()));
    }
    trace.response_hash = sha256_hex(encoded_response);
    bool nondeterministic = false;
    if (provider->capability.deterministic) {
      std::scoped_lock lock(board.mutex);
      const auto [observed, inserted] = board.deterministic_hashes.emplace(
          key, trace.response_hash);
      nondeterministic = !inserted && observed->second != trace.response_hash;
    }
    if (nondeterministic) {
      finish("nondeterministic_result");
      return tl::unexpected(provider_error(ErrorCode::nondeterministic_result,
          "deterministic optical provider returned a different response for the same request",
          provider.get()));
    }
    if (provider->capability.cache == OpticalQueryCache::per_beat) {
      std::scoped_lock lock(board.mutex);
      board.cache.emplace(key, CachedQuery{*response, trace.response_hash});
    }
    finish("ok");
    return response;
  };

  if (providers.size() == 1u) return invoke(providers.front());
  cbor::Value::Array results;
  results.reserve(providers.size());
  for (const auto& provider : providers) {
    auto result = invoke(provider);
    if (!result) return tl::unexpected(result.error());
    results.push_back(std::move(*result));
  }
  return cbor::Value(std::move(results));
}

std::vector<QueryTrace> OpticalContext::query_traces() const {
  if (!impl_->board) return {};
  auto traces = impl_->board->traces();
  std::erase_if(traces, [&](const QueryTrace& trace) {
    return trace.consumer != impl_->consumer ||
        trace.consumer_generation != impl_->generation;
  });
  return traces;
}

}  // namespace tokmon
