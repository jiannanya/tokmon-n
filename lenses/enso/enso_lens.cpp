#include "lenses/enso/enso_lens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "tokmon/hash.hpp"
#include "tokmon/yaml.hpp"

namespace tokmon::builtin {
namespace {

Result<std::string> read_text(const std::filesystem::path& path,
                              const std::uintmax_t limit = 4u * 1024u * 1024u) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > limit)
    return tl::unexpected(make_error(error ? ErrorCode::io_error : ErrorCode::invalid_argument,
                                     "context source is unreadable or too large"));
  std::ifstream input(path, std::ios::binary);
  if (!input) return tl::unexpected(make_error(ErrorCode::io_error,
                                                "cannot read context source"));
  return std::string(std::istreambuf_iterator<char>(input), {});
}

bool within(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

Result<std::filesystem::path> allowed_path(const cbor::Value& parameters) {
  const auto* root_value = cbor::find(parameters, "allowed_root");
  const auto* path_value = cbor::find(parameters, "path");
  if (!root_value || !path_value)
    return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                     "allowed_root and path are required"));
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(root_value->as_string()), error);
  if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                               "cannot resolve allowed_root"));
  const auto supplied = std::filesystem::path(path_value->as_string());
  const auto path = std::filesystem::weakly_canonical(
      supplied.is_absolute() ? supplied : root / supplied, error);
  if (error || !within(root, path))
    return tl::unexpected(make_error(ErrorCode::permission_denied,
                                     "context path escapes allowed_root"));
  return path;
}

bool indexable(const std::filesystem::path& path) {
  static const std::set<std::string> extensions{
      ".md", ".txt", ".cpp", ".hpp", ".c", ".h", ".py", ".js", ".ts",
      ".json", ".yaml", ".yml", ".toml", ".slint", ".cmake"};
  return extensions.contains(path.extension().string());
}

std::set<std::string> terms(const std::string_view text) {
  std::set<std::string> result;
  std::string token;
  const auto flush = [&] { if (token.size() > 1u) result.insert(token); token.clear(); };
  for (const unsigned char character : text) {
    if (std::isalnum(character) != 0 || character >= 0x80u)
      token.push_back(static_cast<char>(std::tolower(character)));
    else flush();
  }
  flush(); return result;
}

std::int64_t score(const std::set<std::string>& query, std::string text,
                   std::string path) {
  std::ranges::transform(text, text.begin(),
      [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  std::ranges::transform(path, path.begin(),
      [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  std::int64_t result = 0;
  for (const auto& term : query) {
    for (std::size_t cursor = 0; (cursor = text.find(term, cursor)) != std::string::npos;
         cursor += term.size()) result += 10;
    if (path.find(term) != std::string::npos) result += 25;
  }
  return result;
}

struct SkillInfo {
  std::string name;
  std::string description;
  std::string version{"unversioned"};
  cbor::Value::Array triggers;
  cbor::Value::Array permissions;
};

SkillInfo parse_skill_info(const std::filesystem::path& path, const std::string& text) {
  SkillInfo info{.name = path.parent_path().filename().string()};
  if (text.starts_with("---")) {
    const auto end = text.find("\n---", 3);
    if (end != std::string::npos) {
      auto metadata = yaml::parse(text.substr(4, end - 4), "skill front matter");
      if (metadata && metadata->is_map()) {
        if (const auto* name = cbor::find(*metadata, "name"))
          info.name = std::string(name->as_string());
        if (const auto* description = cbor::find(*metadata, "description"))
          info.description = std::string(description->as_string());
        if (const auto* version = cbor::find(*metadata, "version"))
          info.version = std::string(version->as_string());
        const auto copy_strings = [](const cbor::Value* values,
                                     cbor::Value::Array& output) {
          if (!values || !values->as_array()) return;
          for (const auto& value : *values->as_array())
            if (std::holds_alternative<std::string>(value.data)) output.push_back(value);
        };
        copy_strings(cbor::find(*metadata, "triggers"), info.triggers);
        copy_strings(cbor::find(*metadata, "permissions"), info.permissions);
      }
    }
  }
  if (info.description.empty()) {
    if (const auto heading = text.find("# "); heading != std::string::npos) {
      const auto end = text.find('\n', heading);
      if (info.name.empty()) info.name = text.substr(heading + 2, end - heading - 2);
      if (end != std::string::npos) {
        const auto start = text.find_first_not_of("\r\n ", end);
        if (start != std::string::npos)
          info.description = text.substr(start, std::min<std::size_t>(240, text.size() - start));
      }
    }
  }
  return info;
}

cbor::Value skill_metadata(const std::filesystem::path& path, const std::string& text) {
  auto info = parse_skill_info(path, text);
  return cbor::object({{"name", std::move(info.name)},
      {"description", std::move(info.description)}, {"version", std::move(info.version)},
      {"triggers", std::move(info.triggers)}, {"permissions", std::move(info.permissions)},
      {"path", path.generic_string()}, {"content_hash", sha256_hex(text)},
      {"body_loaded", false}, {"trust", "mounted"}, {"source", "skill-root"}});
}

bool skill_matches_task(const SkillInfo& info, std::string task) {
  if (info.triggers.empty()) return true;
  std::ranges::transform(task, task.begin(),
      [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  for (const auto& trigger_value : info.triggers) {
    std::string trigger(trigger_value.as_string());
    std::ranges::transform(trigger, trigger.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (!trigger.empty() && task.find(trigger) != std::string::npos) return true;
  }
  return false;
}

cbor::Value::Array embedding(const std::string_view text) {
  constexpr std::size_t dimensions = 64;
  std::array<double, dimensions> vector{};
  for (const auto& term : terms(text)) {
    const auto digest = sha256_hex(term);
    const auto bucket = static_cast<std::size_t>(std::stoul(digest.substr(0, 4), nullptr, 16)) %
                        dimensions;
    const auto sign = (std::stoul(digest.substr(4, 2), nullptr, 16) & 1u) == 0u ? 1.0 : -1.0;
    vector[bucket] += sign;
  }
  double norm = 0.0;
  for (const auto value : vector) norm += value * value;
  norm = std::sqrt(norm);
  cbor::Value::Array result;
  result.reserve(dimensions);
  for (const auto value : vector) result.emplace_back(norm == 0.0 ? 0.0 : value / norm);
  return result;
}

double cosine(const cbor::Value::Array& left, const cbor::Value& right) {
  const auto* values = right.as_array();
  if (!values || values->size() != left.size()) return 0.0;
  double result = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lhs = std::get_if<double>(&left[index].data);
    const auto rhs = std::get_if<double>(&(*values)[index].data);
    if (lhs && rhs) result += *lhs * *rhs;
  }
  return result;
}

}  // namespace

EnsoLens::EnsoLens() : LensBase(make_manifest("enso", "Enso / 上下文全息定影镜",
    {"model.context", "ui.context-sources", "memory.catalog", "rag.catalog"},
    {{"instruction.observed", "*"}, {"skill.*", "*"}, {"memory.*", "*"},
     {"rag.*", "*"}, {"workspace.*", "*"}},
    {{"skill.discover", "tokmon.skill.discover.v1"},
     {"skill.load", "tokmon.skill.load.v1"},
     {"context.retrieve", "tokmon.context.retrieve.v1"},
     {"memory.propose", "tokmon.memory.propose.v1"},
     {"memory.accept", "tokmon.memory.accept.v1"},
     {"memory.reject", "tokmon.memory.reject.v1"},
     {"memory.invalidate", "tokmon.memory.invalidate.v1"},
     {"rag.reindex", "tokmon.rag.reindex.v1"}},
    {"photon.emit", "io.workspace.read", "index.write", "log.write"})) {}

Result<void> EnsoLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  cbor::Value::Array sources;
  cbor::Value::Array memories;
  cbor::Value::Array documents;
  std::set<std::string> invalidated;
  for (const auto& photon : photons.photons())
    if (photon.kind == "memory.invalidated")
      if (const auto* id = cbor::find(photon.payload, "memory_id"))
        invalidated.insert(std::string(id->as_string()));
  for (const auto& photon : photons.photons()) {
    const bool instruction = photon.kind == "instruction.observed";
    const bool skill = photon.kind == "skill.loaded";
    const bool memory = photon.kind == "memory.accepted" &&
        (!cbor::find(photon.payload, "memory_id") ||
         !invalidated.contains(std::string(cbor::find(photon.payload, "memory_id")->as_string())));
    const bool retrieved = photon.kind == "context.retrieved";
    if (instruction || skill || memory || retrieved) {
      sources.push_back(cbor::object({{"kind", photon.kind}, {"source", photon.id},
          {"hash", photon.hash}, {"classification", instruction || skill ? "instruction" : "data"},
          {"trust", instruction ? "local-explicit" : skill ? "mounted" : "untrusted-data"},
          {"sensitivity", cbor::find(photon.payload, "sensitivity")
              ? *cbor::find(photon.payload, "sensitivity") : cbor::Value("normal")},
          {"content", photon.payload}}));
    }
    if (memory) memories.push_back(photon.payload);
    if (photon.kind == "rag.document-indexed" || photon.kind == "rag.document-tombstoned")
      documents.push_back(photon.payload);
  }
  if (auto result = surface.add("model.context", "enso.sources", sources, 30); !result)
    return result;
  if (auto result = surface.add("ui.context-sources", "enso.sources", std::move(sources), 10);
      !result) return result;
  if (auto result = surface.add("memory.catalog", "accepted", std::move(memories), 10); !result)
    return result;
  if (auto result = surface.add("rag.catalog", "documents", std::move(documents), 5); !result)
    return result;
  if (const auto* changed = photons.latest("workspace.changes-observed")) {
    if (const auto* root = cbor::find(changed->payload, "root"); root && !root->as_string().empty()) {
      auto act = propose(*changed, "rag.reindex", "tokmon.rag.reindex.v1",
                         manifest().id,
                         cbor::object({{"roots", cbor::Value::Array{
                             std::string(root->as_string())}}, {"incremental", true}}));
      act.idempotency_key = "enso-watch:" + changed->hash;
      if (auto result = surface.propose(std::move(act)); !result) return result;
    }
  }
  return {};
}

Result<RefractionResult> EnsoLens::refract(const PhotonWindow& photons, const Act& act,
                                            RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  std::vector<PhotonId> emitted;
  const auto append = [&](std::string kind, std::string schema,
                          cbor::Value payload) -> Result<void> {
    auto photon = beam.emit(std::move(kind), std::move(schema), std::move(payload));
    if (!photon) return tl::unexpected(photon.error());
    emitted.push_back(photon->id); return {};
  };

  if (act.kind == "skill.discover") {
    const auto* roots = cbor::find(act.parameters, "roots");
    if (!roots || !roots->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "skill.discover requires roots"));
    std::int64_t count = 0;
    for (const auto& value : *roots->as_array()) {
      std::error_code error;
      const auto root = std::filesystem::weakly_canonical(
          std::filesystem::path(value.as_string()), error);
      if (error || !std::filesystem::is_directory(root)) continue;
      for (std::filesystem::recursive_directory_iterator iterator(root,
               std::filesystem::directory_options::skip_permission_denied, error), end;
           iterator != end && count < 256; iterator.increment(error)) {
        if (error) { error.clear(); continue; }
        if (!iterator->is_regular_file() || iterator->path().filename() != "SKILL.md") continue;
        auto content = read_text(iterator->path());
        if (!content) continue;
        if (auto result = append("skill.discovered", "tokmon.skill.metadata.v1",
            skill_metadata(iterator->path(), *content)); !result)
          return tl::unexpected(result.error());
        ++count;
      }
    }
    if (auto result = append("skill.discovery-completed", "tokmon.skill.discovery.v1",
        cbor::object({{"count", count}, {"body_loaded", false}})); !result)
      return tl::unexpected(result.error());
  } else if (act.kind == "skill.load") {
    auto path = allowed_path(act.parameters);
    if (!path) return tl::unexpected(path.error());
    if (path->filename() != "SKILL.md")
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "skill.load only accepts SKILL.md"));
    auto content = read_text(*path);
    if (!content) return tl::unexpected(content.error());
    const auto info = parse_skill_info(*path, *content);
    const auto* task = cbor::find(act.parameters, "task");
    if (!task || task->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "skill.load requires the matching task"));
    if (!skill_matches_task(info, std::string(task->as_string())))
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "SKILL.md trigger does not match this task"));
    cbor::Value::Array references;
    cbor::Value::Array load_chain{cbor::object({
        {"path", path->generic_string()}, {"hash", sha256_hex(*content)}})};
    const std::regex links(R"(\[[^\]]*\]\(([^)]+)\))");
    for (std::sregex_iterator iterator(content->begin(), content->end(), links), end;
         iterator != end; ++iterator) {
      const auto link = (*iterator)[1].str();
      if (link.find("://") != std::string::npos || link.starts_with('#')) continue;
      std::error_code error;
      const auto root = std::filesystem::weakly_canonical(
          std::filesystem::path(cbor::find(act.parameters, "allowed_root")->as_string()), error);
      const auto referenced = std::filesystem::weakly_canonical(path->parent_path() / link, error);
      if (error || !within(root, referenced))
        return tl::unexpected(make_error(ErrorCode::permission_denied,
                                         "SKILL.md reference escapes allowed_root"));
      const bool exists = std::filesystem::is_regular_file(referenced);
      auto referenced_content = exists ? read_text(referenced) : Result<std::string>(
          tl::unexpected(make_error(ErrorCode::not_found, "skill reference is absent")));
      const auto reference_hash = referenced_content ? sha256_hex(*referenced_content) : "";
      references.push_back(cbor::object({{"path", referenced.generic_string()},
          {"exists", exists}, {"content_hash", reference_hash}}));
      if (exists) load_chain.push_back(cbor::object({
          {"path", referenced.generic_string()}, {"hash", reference_hash}}));
    }
    auto metadata = skill_metadata(*path, *content);
    if (auto result = append("skill.loaded", "tokmon.skill.loaded.v1", cbor::object({
        {"path", path->generic_string()}, {"content_hash", sha256_hex(*content)},
        {"content", *content}, {"references", std::move(references)},
        {"load_chain", std::move(load_chain)}, {"metadata", std::move(metadata)},
        {"version", info.version}, {"permissions", info.permissions},
        {"matched_task_hash", sha256_hex(task->as_string())},
        {"classification", "instruction"}, {"trust", "mounted"},
        {"source", "skill-root"}})); !result)
      return tl::unexpected(result.error());
  } else if (act.kind == "rag.reindex") {
    const auto* roots = cbor::find(act.parameters, "roots");
    if (roots && !roots->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "rag.reindex requires roots"));
    const auto* supplied_documents = cbor::find(act.parameters, "documents");
    if (supplied_documents && !supplied_documents->as_array())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "rag.reindex documents must be an array"));
    const auto chunk_size = static_cast<std::size_t>(std::clamp<std::int64_t>(
        cbor::find(act.parameters, "chunk_chars")
            ? cbor::find(act.parameters, "chunk_chars")->as_integer(2000) : 2000,
        256, 16'384));
    std::map<std::string, std::string, std::less<>> previous;
    for (const auto& photon : photons.photons()) {
      const auto* path = cbor::find(photon.payload, "path");
      const auto* revision = cbor::find(photon.payload, "revision");
      if (!path || !revision) continue;
      if (photon.kind == "rag.document-indexed")
        previous[std::string(path->as_string())] = std::string(revision->as_string());
      else if (photon.kind == "rag.document-tombstoned")
        previous.erase(std::string(path->as_string()));
    }
    std::set<std::string> current_paths;
    std::int64_t document_count = 0;
    std::int64_t chunk_count = 0;
    std::int64_t updated_documents = 0;
    const auto index_document = [&](const std::string& document,
                                    const std::string& content,
                                    const std::string& source) -> Result<void> {
      current_paths.insert(document);
      ++document_count;
      const auto revision = sha256_hex(content);
      if (const auto old = previous.find(document);
          old != previous.end() && old->second == revision) return {};
      if (auto result = append("rag.document-indexed", "tokmon.rag.document.v1",
          cbor::object({{"document", document}, {"path", document},
              {"revision", revision}, {"content_hash", revision}, {"source", source},
              {"bytes", static_cast<std::int64_t>(content.size())}})); !result)
        return result;
      for (std::size_t offset = 0; offset < content.size(); offset += chunk_size) {
        const auto text = content.substr(offset, chunk_size);
        const auto chunk_id = sha256_hex(document + ":" + revision + ":" +
                                          std::to_string(offset));
        if (auto result = append("rag.chunk-indexed", "tokmon.rag.chunk.v1",
            cbor::object({{"document", document}, {"path", document},
                {"revision", revision}, {"chunk", chunk_id},
                {"offset", static_cast<std::int64_t>(offset)}, {"hash", sha256_hex(text)},
                {"text", text}, {"embedding", embedding(text)}, {"source", source},
                {"parser", "text-v1"}, {"classification", "data"}})); !result)
          return result;
        ++chunk_count;
      }
      ++updated_documents;
      return {};
    };
    const cbor::Value::Array empty_roots;
    const auto& configured_roots = roots ? *roots->as_array() : empty_roots;
    std::vector<std::string> canonical_roots;
    for (const auto& root_value : configured_roots) {
      std::error_code error;
      const auto root = std::filesystem::weakly_canonical(
          std::filesystem::path(root_value.as_string()), error);
      if (error || !std::filesystem::is_directory(root)) continue;
      canonical_roots.push_back(root.generic_string());
      for (std::filesystem::recursive_directory_iterator iterator(root,
               std::filesystem::directory_options::skip_permission_denied, error), end;
           iterator != end && document_count < 4096; iterator.increment(error)) {
        if (error) { error.clear(); continue; }
        const auto path = iterator->path();
        if (iterator->is_directory() && (path.filename() == ".git" ||
            path.filename() == "node_modules" || path.filename() == "build")) {
          iterator.disable_recursion_pending(); continue;
        }
        if (!iterator->is_regular_file() || !indexable(path)) continue;
        auto content = read_text(path);
        if (!content) continue;
        const auto path_text = path.generic_string();
        if (auto result = index_document(path_text, *content, "cove"); !result)
          return tl::unexpected(result.error());
      }
    }
    if (supplied_documents) {
      for (const auto& document : *supplied_documents->as_array()) {
        const auto* identifier = cbor::find(document, "document");
        const auto* content = cbor::find(document, "content");
        const auto* source = cbor::find(document, "source");
        if (!identifier || !content || identifier->as_string().empty() ||
            !std::holds_alternative<std::string>(content->data))
          return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "RAG document requires document and content"));
        const auto source_name = source ? std::string(source->as_string()) : "external";
        if (source_name != "cove" && source_name != "chora" && source_name != "external")
          return tl::unexpected(make_error(ErrorCode::permission_denied,
                                           "RAG document source is not allowed"));
        if (auto result = index_document(std::string(identifier->as_string()),
                                         std::string(content->as_string()), source_name); !result)
          return tl::unexpected(result.error());
      }
    }
    for (const auto& [path, revision] : previous) {
      const bool managed_by_root = std::ranges::any_of(canonical_roots,
          [&](const auto& root) { return path == root || path.starts_with(root + "/"); });
      if (managed_by_root && !current_paths.contains(path))
        if (auto result = append("rag.document-tombstoned", "tokmon.rag.document.v1",
            cbor::object({{"document", path}, {"path", path}, {"revision", revision},
                          {"reason", "source-absent"}})); !result)
          return tl::unexpected(result.error());
    }
    if (auto result = append("rag.index-rebuilt", "tokmon.rag.index.v1", cbor::object({
        {"documents", document_count}, {"chunks", chunk_count},
        {"updated_documents", updated_documents}, {"index_is_fact_source", false},
        {"incremental", true}, {"embedding", "deterministic-hash-64"},
        {"retrieval", "hybrid-keyword-vector-rerank"}})); !result)
      return tl::unexpected(result.error());
  } else if (act.kind == "context.retrieve") {
    const auto* query_value = cbor::find(act.parameters, "query");
    if (!query_value || query_value->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "context.retrieve requires query"));
    struct Hit { double relevance; std::int64_t keyword; double vector; const Photon* photon; };
    std::vector<Hit> hits;
    const auto query = terms(query_value->as_string());
    const auto query_embedding = embedding(query_value->as_string());
    std::map<std::string, std::string, std::less<>> latest_revisions;
    for (const auto& photon : photons.photons()) {
      const auto* path = cbor::find(photon.payload, "path");
      const auto* revision = cbor::find(photon.payload, "revision");
      if (!path || !revision) continue;
      if (photon.kind == "rag.document-indexed")
        latest_revisions[std::string(path->as_string())] = std::string(revision->as_string());
      else if (photon.kind == "rag.document-tombstoned")
        latest_revisions.erase(std::string(path->as_string()));
    }
    const auto source_filter = cbor::find(act.parameters, "source");
    const auto path_prefix = cbor::find(act.parameters, "path_prefix");
    for (const auto& photon : photons.photons()) {
      if (photon.kind != "rag.chunk-indexed") continue;
      const auto* text = cbor::find(photon.payload, "text");
      const auto* path = cbor::find(photon.payload, "document");
      const auto* revision = cbor::find(photon.payload, "revision");
      const auto* source = cbor::find(photon.payload, "source");
      if (!text || !path || !revision || !source) continue;
      const auto current = latest_revisions.find(std::string(path->as_string()));
      if (current == latest_revisions.end() || current->second != revision->as_string()) continue;
      if (source_filter && source->as_string() != source_filter->as_string()) continue;
      if (path_prefix && !path->as_string().starts_with(path_prefix->as_string())) continue;
      const auto keyword = score(query, std::string(text->as_string()),
                                 std::string(path->as_string()));
      const auto* vector = cbor::find(photon.payload, "embedding");
      const auto semantic = vector ? cosine(query_embedding, *vector) : 0.0;
      const auto combined = static_cast<double>(keyword) + std::max(0.0, semantic) * 20.0;
      if (combined > 0.0) hits.push_back({combined, keyword, semantic, &photon});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
      return left.relevance != right.relevance ? left.relevance > right.relevance
                                               : left.photon->sequence > right.photon->sequence;
    });
    const auto top_k = static_cast<std::size_t>(std::clamp<std::int64_t>(
        cbor::find(act.parameters, "top_k")
            ? cbor::find(act.parameters, "top_k")->as_integer(8) : 8, 1, 50));
    cbor::Value::Array results;
    for (std::size_t index = 0; index < std::min(top_k, hits.size()); ++index) {
      const auto& payload = hits[index].photon->payload;
      results.push_back(cbor::object({{"score", hits[index].relevance},
          {"keyword_score", hits[index].keyword}, {"vector_score", hits[index].vector},
          {"document", *cbor::find(payload, "document")},
          {"path", *cbor::find(payload, "path")},
          {"revision", *cbor::find(payload, "revision")}, {"chunk", *cbor::find(payload, "chunk")},
          {"hash", *cbor::find(payload, "hash")}, {"text", *cbor::find(payload, "text")},
          {"source", *cbor::find(payload, "source")}, {"classification", "data"}}));
    }
    if (auto result = append("context.retrieved", "tokmon.context.result.v1",
        cbor::object({{"query_hash", sha256_hex(query_value->as_string())},
                      {"results", std::move(results)}, {"source_class", "data"}})); !result)
      return tl::unexpected(result.error());
  } else if (act.kind == "memory.propose") {
    const auto* content = cbor::find(act.parameters, "content");
    const auto* source = cbor::find(act.parameters, "source_photon");
    if (!content || !source || source->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "memory.propose requires content and an explicit source_photon"));
    const auto memory_id = sha256_hex(cbor::encode(act.parameters));
    if (auto result = append("memory.proposed", "tokmon.memory.proposal.v1",
        cbor::object({{"memory_id", memory_id},
          {"content", *content}, {"scope", cbor::find(act.parameters, "scope")
              ? *cbor::find(act.parameters, "scope") : cbor::Value("project")},
          {"source_photon", *source},
          {"confidence", cbor::find(act.parameters, "confidence")
              ? *cbor::find(act.parameters, "confidence") : cbor::Value(0.5)},
          {"expires_at", cbor::find(act.parameters, "expires_at")
              ? *cbor::find(act.parameters, "expires_at") : cbor::Value("")},
          {"sensitivity", cbor::find(act.parameters, "sensitivity")
              ? *cbor::find(act.parameters, "sensitivity") : cbor::Value("normal")},
          {"supersedes", cbor::find(act.parameters, "supersedes")
              ? *cbor::find(act.parameters, "supersedes") : cbor::Value("")},
          {"candidate_kind", cbor::find(act.parameters, "candidate_kind")
              ? *cbor::find(act.parameters, "candidate_kind") : cbor::Value("fact")},
          {"policy_required", true}, {"accepted", false}})); !result)
      return tl::unexpected(result.error());
  } else {
    const auto* memory_id = cbor::find(act.parameters, "memory_id");
    if (!memory_id || memory_id->as_string().empty())
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "memory decision requires memory_id"));
    const auto kind = act.kind == "memory.accept" ? "memory.accepted" :
        act.kind == "memory.reject" ? "memory.rejected" : "memory.invalidated";
    const Photon* proposal = nullptr;
    for (auto iterator = photons.photons().rbegin(); iterator != photons.photons().rend(); ++iterator)
      if (iterator->kind == "memory.proposed")
        if (const auto* id = cbor::find(iterator->payload, "memory_id");
            id && id->as_string() == memory_id->as_string()) {
          proposal = &*iterator;
          break;
        }
    if (act.kind == "memory.accept") {
      const auto* policy = cbor::find(act.parameters, "policy_photon");
      if (!proposal || !policy || policy->as_string().empty())
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            "memory.accept requires the proposal and Fallen policy evidence"));
      const auto* sensitivity = cbor::find(proposal->payload, "sensitivity");
      if (sensitivity && sensitivity->as_string() != "normal" &&
          (!cbor::find(act.parameters, "human_confirmed") ||
           !cbor::find(act.parameters, "human_confirmed")->as_bool()))
        return tl::unexpected(make_error(ErrorCode::permission_denied,
            "sensitive memory requires explicit human confirmation"));
    }
    const auto inherited = [&](const std::string_view field, cbor::Value fallback) {
      if (const auto* direct = cbor::find(act.parameters, field)) return *direct;
      if (proposal)
        if (const auto* stored = cbor::find(proposal->payload, field)) return *stored;
      return fallback;
    };
    if (auto result = append(kind, "tokmon.memory.result.v1", cbor::object({
        {"memory_id", std::string(memory_id->as_string())},
        {"content", inherited("content", cbor::Value(nullptr))},
        {"scope", inherited("scope", cbor::Value("project"))},
        {"source_photon", inherited("source_photon", cbor::Value(""))},
        {"confidence", inherited("confidence", cbor::Value(0.5))},
        {"expires_at", inherited("expires_at", cbor::Value(""))},
        {"sensitivity", inherited("sensitivity", cbor::Value("normal"))},
        {"supersedes", inherited("supersedes", cbor::Value(""))},
        {"policy_photon", inherited("policy_photon", cbor::Value(""))},
        {"human_confirmed", inherited("human_confirmed", cbor::Value(false))},
        {"append_only_version", true}})); !result)
      return tl::unexpected(result.error());
  }
  return RefractionResult{.status = RefractionStatus::completed,
                           .emitted = std::move(emitted),
                           .detail = "context operation completed"};
}

}  // namespace tokmon::builtin
