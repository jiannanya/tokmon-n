#include "lenses/textus/textus_lens.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "tokmon/hash.hpp"

namespace tokmon::builtin {
namespace {

struct Fragment {
  std::string role;
  std::string source_class;
  std::string content;
  std::string photon_id;
  std::string source_hash;
  std::string trust{"local"};
  std::string sensitivity{"normal"};
  std::int32_t priority{0};
  std::uint64_t sequence{0};
  std::int64_t tokens{0};
  bool mandatory{false};
};

std::int64_t estimate_tokens(const std::string_view text) {
  std::int64_t tokens = 0;
  std::size_t ascii_run = 0;
  const auto flush = [&] {
    if (ascii_run != 0) tokens += static_cast<std::int64_t>((ascii_run + 3u) / 4u);
    ascii_run = 0;
  };
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80u) {
      if (std::isalnum(byte) != 0 || byte == '_' || byte == '-') ++ascii_run;
      else { flush(); if (std::isspace(byte) == 0) ++tokens; }
      ++index;
      continue;
    }
    flush();
    std::size_t width = (byte & 0xe0u) == 0xc0u ? 2u :
        (byte & 0xf0u) == 0xe0u ? 3u : (byte & 0xf8u) == 0xf0u ? 4u : 1u;
    index += std::min(width, text.size() - index);
    ++tokens;
  }
  flush();
  return std::max<std::int64_t>(tokens, text.empty() ? 0 : 1);
}

std::string photon_text(const Photon& photon) {
  if (const auto* content = cbor::find(photon.payload, "content"); content) {
    if (std::holds_alternative<std::string>(content->data)) return std::string(content->as_string());
    return cbor::diagnostic(*content);
  }
  if (const auto* text = cbor::find(photon.payload, "text"))
    return std::string(text->as_string());
  return cbor::diagnostic(photon.payload);
}

std::int32_t category(const std::string_view kind) {
  if (kind == "system.fragment" || kind == "instruction.observed") return 800;
  if (kind == "skill.loaded" || kind == "skill.mounted") return 600;
  if (kind == "user.input") return 700;
  if (kind == "model.tool-call" || kind == "tool.result") return 650;
  if (kind == "assistant.message") return 500;
  if (kind == "memory.accepted") return 300;
  if (kind == "context.retrieved" || kind.starts_with("rag.")) return 200;
  if (kind == "summary.created") return 100;
  return -1;
}

std::string source_class(const std::string_view kind) {
  if (kind == "system.fragment") return "system";
  if (kind == "instruction.observed") return "instruction";
  if (kind.starts_with("skill.")) return "skill";
  if (kind == "user.input" || kind == "assistant.message") return "conversation";
  if (kind == "model.tool-call" || kind == "tool.result") return "tool";
  if (kind == "memory.accepted") return "memory";
  if (kind.starts_with("rag.") || kind == "context.retrieved") return "rag";
  return "summary";
}

std::string role(const std::string_view kind) {
  if (kind == "user.input") return "user";
  if (kind == "assistant.message") return "assistant";
  if (kind == "model.tool-call") return "assistant";
  if (kind == "tool.result") return "tool";
  return "system";
}

}  // namespace

TextusLens::TextusLens() : LensBase(make_manifest("textus", "Textus / ModelSurface 光谱滤波镜",
    {"model.messages", "model.context", "diagnostic.context-budget"},
    {{"user.input", "*"}, {"assistant.message", "*"}, {"model.tool-call", "*"},
     {"tool.result", "*"}, {"system.fragment", "*"}, {"instruction.observed", "*"},
     {"skill.*", "*"}, {"memory.accepted", "*"}, {"context.retrieved", "*"},
     {"rag.*", "*"}, {"summary.created", "*"}, {"model.budget", "*"}},
    {{"text.compact", "tokmon.text.compact.v1"},
     {"text.summarize", "tokmon.text.summarize.v1"}})) {}

Result<void> TextusLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  std::int64_t budget = 32'768;
  std::int64_t reserve = 4'096;
  std::string model = "unspecified";
  std::string tokenizer = "conservative-utf8-v1";
  if (const auto* configured = photons.latest("model.budget")) {
    if (const auto* field = cbor::find(configured->payload, "max_tokens"))
      budget = std::clamp<std::int64_t>(field->as_integer(budget), 256, 1'000'000);
    if (const auto* field = cbor::find(configured->payload, "reserve_output_tokens"))
      reserve = std::clamp<std::int64_t>(field->as_integer(reserve), 0, budget - 1);
    if (const auto* field = cbor::find(configured->payload, "model")) model = field->as_string();
    if (const auto* field = cbor::find(configured->payload, "tokenizer"))
      tokenizer = field->as_string();
  }
  const auto input_budget = budget - reserve;
  const auto* latest_user = photons.latest("user.input");
  const auto* latest_call = photons.latest("model.tool-call");
  const auto* latest_result = photons.latest("tool.result");

  std::vector<Fragment> fragments;
  std::set<std::string> hashes;
  for (const auto& photon : photons.photons()) {
    const auto priority = category(photon.kind);
    if (priority < 0) continue;
    auto content = photon_text(photon);
    const auto content_hash = sha256_hex(content);
    if (!hashes.insert(content_hash).second) continue;
    const bool pending_call = photon.kind == "model.tool-call" && latest_call == &photon &&
        (!latest_result || latest_result->sequence < photon.sequence);
    const bool mandatory = (&photon == latest_user) || pending_call ||
        photon.kind == "system.fragment" || photon.kind == "instruction.observed";
    fragments.push_back(Fragment{.role = role(photon.kind),
        .source_class = source_class(photon.kind), .content = std::move(content),
        .photon_id = photon.id, .source_hash = content_hash,
        .trust = (photon.kind.starts_with("rag.") || photon.kind == "context.retrieved")
            ? "untrusted-data" : "local",
        .sensitivity = cbor::find(photon.payload, "sensitivity")
            ? std::string(cbor::find(photon.payload, "sensitivity")->as_string("normal"))
            : "normal",
        .priority = priority, .sequence = photon.sequence,
        .tokens = estimate_tokens(content), .mandatory = mandatory});
  }

  std::vector<std::size_t> candidates(fragments.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) candidates[index] = index;
  std::stable_sort(candidates.begin(), candidates.end(), [&](const auto left, const auto right) {
    if (fragments[left].mandatory != fragments[right].mandatory)
      return fragments[left].mandatory > fragments[right].mandatory;
    if (fragments[left].priority != fragments[right].priority)
      return fragments[left].priority > fragments[right].priority;
    return fragments[left].sequence > fragments[right].sequence;
  });
  std::set<std::size_t> retained;
  cbor::Value::Array decisions;
  std::int64_t used = 0;
  bool blocked = false;
  for (const auto index : candidates) {
    const auto& fragment = fragments[index];
    if (used + fragment.tokens <= input_budget) {
      retained.insert(index); used += fragment.tokens;
      continue;
    }
    if (fragment.mandatory) blocked = true;
    decisions.push_back(cbor::object({{"source", fragment.photon_id},
        {"source_hash", fragment.source_hash}, {"tokens", fragment.tokens},
        {"decision", fragment.mandatory ? "blocked-mandatory-overflow" : "dropped"},
        {"reason", "model_budget"}}));
  }

  std::vector<std::size_t> output(retained.begin(), retained.end());
  std::sort(output.begin(), output.end(), [&](const auto left, const auto right) {
    if (fragments[left].priority != fragments[right].priority)
      return fragments[left].priority > fragments[right].priority;
    return fragments[left].sequence < fragments[right].sequence;
  });
  cbor::Value::Array messages;
  cbor::Value::Array context;
  for (const auto index : output) {
    const auto& fragment = fragments[index];
    auto value = cbor::object({{"role", fragment.role}, {"content", fragment.content},
        {"source_class", fragment.source_class}, {"source_photon", fragment.photon_id},
        {"source_hash", fragment.source_hash}, {"tokens", fragment.tokens},
        {"trust", fragment.trust}, {"sensitivity", fragment.sensitivity},
        {"mandatory", fragment.mandatory}});
    context.push_back(value);
    if (fragment.source_class == "conversation" || fragment.source_class == "tool")
      messages.push_back(std::move(value));
  }
  if (!blocked) {
    if (auto result = surface.add("model.messages", "active-ray", std::move(messages), 50);
        !result) return result;
    if (auto result = surface.add("model.context", "textus.fragments", std::move(context), 40);
        !result) return result;
  }
  return identify(surface, "diagnostic.context-budget", cbor::object({
      {"estimated_tokens", used}, {"input_budget_tokens", input_budget},
      {"window_tokens", budget}, {"reserved_output_tokens", reserve},
      {"blocked", blocked}, {"model", model}, {"tokenizer", tokenizer},
      {"decisions", std::move(decisions)}, {"reducer_version", "textus-v2"},
      {"cache_key", sha256_hex(std::to_string(photons.latest() ?
          photons.latest()->sequence : 0) + ":" + model + ":" + tokenizer + ":textus-v2")},
      {"tail_sequence", photons.latest() ?
          static_cast<std::int64_t>(photons.latest()->sequence) : 0}}));
}

Result<RefractionResult> TextusLens::refract(const PhotonWindow& photons, const Act& act,
                                              RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  const auto first = photons.photons().empty() ? 0 : photons.photons().front().sequence;
  const auto last = photons.latest() ? photons.latest()->sequence : 0;
  const auto encoded_window = cbor::encode(to_cbor(photons));
  const auto max_chars = static_cast<std::size_t>(std::clamp<std::int64_t>(
      cbor::find(act.parameters, "max_chars")
          ? cbor::find(act.parameters, "max_chars")->as_integer(4096) : 4096, 128, 65536));
  std::string summary;
  cbor::Value::Array sources;
  std::int64_t input_tokens = 0;
  for (auto iterator = photons.photons().rbegin(); iterator != photons.photons().rend();
       ++iterator) {
    if (category(iterator->kind) < 0) continue;
    auto content = photon_text(*iterator);
    input_tokens += estimate_tokens(content);
    if (summary.size() < max_chars) {
      if (!summary.empty()) summary.insert(0, "\n");
      const auto remaining = max_chars - summary.size();
      if (content.size() > remaining) content.resize(remaining);
      summary.insert(0, content);
    }
    sources.push_back(iterator->id);
  }
  return emit(beam, "summary.created", "tokmon.text.summary.v1", cbor::object({
      {"covered_from", static_cast<std::int64_t>(first)},
      {"covered_to", static_cast<std::int64_t>(last)},
      {"covered_hash", sha256_hex(encoded_window)}, {"source_refs", std::move(sources)},
      {"strategy", act.kind == "text.compact" ? "deterministic-extractive" : "model-requested"},
      {"summary_model", cbor::find(act.parameters, "model")
          ? *cbor::find(act.parameters, "model") : cbor::Value("none")},
      {"prompt_version", "textus-summary-v1"}, {"input_tokens", input_tokens},
      {"output_tokens", estimate_tokens(summary)}, {"quality", "extractive"},
      {"summary", std::move(summary)}}));
}

}  // namespace tokmon::builtin
