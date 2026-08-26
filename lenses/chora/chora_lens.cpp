#include "lenses/chora/chora_lens.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "tokmon/hash.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#endif

namespace {

using Bytes = std::vector<std::uint8_t>;

tokmon::Result<std::filesystem::path> storage_root(
    const tokmon::cbor::Value& parameters) {
  const auto* field = tokmon::cbor::find(parameters, "storage_root");
  if (!field || field->as_string().empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                             "storage_root is required"));
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(field->as_string()), error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot create storage_root"));
  auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(field->as_string()), error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot canonicalize storage_root"));
  return root;
}

tokmon::Result<std::filesystem::path> storage_directory(
    const tokmon::cbor::Value& parameters, const std::string_view child) {
  auto root = storage_root(parameters);
  if (!root) return tl::unexpected(root.error());
  const auto directory = *root / child;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot create storage directory"));
  return directory;
}

tokmon::Result<Bytes> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot read storage object"));
  return Bytes(std::istreambuf_iterator<char>(input), {});
}

tokmon::Result<void> write_once(const std::filesystem::path& path,
                                const std::span<const std::uint8_t> bytes) {
  if (std::filesystem::exists(path)) {
    auto existing = read_bytes(path);
    if (!existing) return tl::unexpected(existing.error());
    if (*existing != Bytes(bytes.begin(), bytes.end()))
      return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::integrity_error,
                                               "immutable object hash collision"));
    return {};
  }
  const auto temporary = path.parent_path() /
      (path.filename().string() + ".tmp-" + tokmon::sha256_hex(path.string()).substr(0, 8));
  std::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output) {
    std::error_code ignored; std::filesystem::remove(temporary, ignored);
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot persist immutable storage object"));
  }
  output.close();
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error && !std::filesystem::exists(path))
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot publish immutable storage object"));
  if (error) std::filesystem::remove(temporary, error);
  return {};
}

tokmon::Result<Bytes> payload_bytes(const tokmon::cbor::Value& parameters) {
  const auto* content = tokmon::cbor::find(parameters, "content");
  if (!content)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                             "content is required"));
  if (const auto* binary = std::get_if<tokmon::cbor::Value::Bytes>(&content->data))
    return *binary;
  if (const auto* text = std::get_if<std::string>(&content->data))
    return Bytes(text->begin(), text->end());
  return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                           "content must be bytes or string"));
}

tokmon::Result<Bytes> protect(const Bytes& plaintext) {
#if defined(_WIN32)
  DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                  const_cast<BYTE*>(plaintext.data())};
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"Tokmon Chora Blob", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::storage_error,
                                             "DPAPI blob encryption failed"));
  Bytes result(output.pbData, output.pbData + output.cbData);
  LocalFree(output.pbData);
  return result;
#else
  (void)plaintext;
  return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::unsupported,
      "sensitive blobs require a configured envelope encryption backend"));
#endif
}

tokmon::cbor::Value manifest_entry(const std::filesystem::path& root,
                                   const std::filesystem::path& file,
                                   const Bytes& bytes) {
  return tokmon::cbor::object({
      {"path", file.lexically_relative(root).generic_string()},
      {"sha256", tokmon::sha256_hex(bytes)},
      {"bytes", static_cast<std::int64_t>(bytes.size())}});
}

}  // namespace

namespace tokmon::builtin {

ChoraLens::ChoraLens() : LensBase(make_manifest("chora", "Chora / 不可改写光感底片",
    {"fact.storage", "diagnostic.capacity", "storage.kv"}, {{"*", "*"}},
    {{"photon.export", "tokmon.photon.export.v1"},
     {"kv.put", "tokmon.kv.put.v1"}, {"kv.delete", "tokmon.kv.delete.v1"},
     {"blob.put", "tokmon.blob.put.v1"}, {"blob.verify", "tokmon.blob.verify.v1"},
     {"checkpoint.build", "tokmon.checkpoint.build.v1"},
     {"archive.seal", "tokmon.archive.seal.v1"},
     {"backup.create", "tokmon.backup.create.v1"},
     {"backup.restore", "tokmon.backup.restore.v1"}},
    {"photon.emit", "blob.write", "artifact.write", "backup.io", "log.write"})) {}

Result<void> ChoraLens::view(const OpticalInput& photons, WavefrontBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* tail = photons.latest();
  cbor::Value::Map current_kv;
  std::int64_t blob_count = 0;
  std::int64_t blob_bytes = 0;
  for (const auto& photon : photons.photons()) {
    if (photon.kind == "kv.version-created") {
      const auto* key = cbor::find(photon.payload, "key");
      if (!key) continue;
      if (cbor::find(photon.payload, "tombstone") &&
          cbor::find(photon.payload, "tombstone")->as_bool())
        current_kv.erase(std::string(key->as_string()));
      else current_kv[std::string(key->as_string())] = photon.payload;
    }
    if (photon.kind == "blob.stored") {
      ++blob_count;
      if (const auto* bytes = cbor::find(photon.payload, "bytes")) blob_bytes += bytes->as_integer();
    }
  }
  if (auto result = identify(surface, "fact.storage", cbor::object({
      {"tail_sequence", tail ? static_cast<std::int64_t>(tail->sequence) : 0},
      {"window_photons", static_cast<std::int64_t>(photons.photons().size())},
      {"append_only", true}, {"single_writer", true}, {"wal", true},
      {"blob_count", blob_count}, {"blob_bytes", blob_bytes}})); !result) return result;
  if (auto result = surface.add("storage.kv", "current", std::move(current_kv), 5); !result)
    return result;
  return surface.add("diagnostic.capacity", "active-window", cbor::object({
      {"photon_count", static_cast<std::int64_t>(photons.photons().size())},
      {"derived_objects_rebuildable", true}, {"writer_token", "Nyxia-owned"}}), 0);
}

Result<RefractionResult> ChoraLens::refract(const PhotonWindow& photons, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};

  if (act.kind == "kv.put" || act.kind == "kv.delete") {
    const auto* key = cbor::find(act.parameters, "key");
    if (!key || key->as_string().empty() || key->as_string().size() > 512u)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "KV key must contain 1..512 UTF-8 bytes"));
    const auto* value = cbor::find(act.parameters, "value");
    if (act.kind == "kv.put" && !value)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch, "kv.put requires value"));
    auto record = cbor::object({{"key", std::string(key->as_string())},
        {"value", value ? *value : cbor::Value(nullptr)},
        {"tombstone", act.kind == "kv.delete"},
        {"previous_version", cbor::find(act.parameters, "previous_version")
            ? *cbor::find(act.parameters, "previous_version") : cbor::Value("")},
        {"act_id", act.id}, {"epoch", static_cast<std::int64_t>(act.epoch)}});
    const auto bytes = cbor::encode(record);
    const auto version = sha256_hex(bytes);
    auto directory = storage_directory(act.parameters, "kv");
    if (!directory) return tl::unexpected(directory.error());
    if (auto written = write_once(*directory / (version + ".cbor"), bytes); !written)
      return tl::unexpected(written.error());
    return emit(beam, "kv.version-created", "tokmon.kv.version.v1", cbor::object({
        {"key", std::string(key->as_string())}, {"version", version},
        {"previous_version", cbor::find(act.parameters, "previous_version")
            ? *cbor::find(act.parameters, "previous_version") : cbor::Value("")},
        {"tombstone", act.kind == "kv.delete"}, {"immutable", true}}));
  }

  if (act.kind == "blob.put") {
    auto clear = payload_bytes(act.parameters);
    if (!clear) return tl::unexpected(clear.error());
    const auto content_hash = sha256_hex(*clear);
    const bool sensitive = cbor::find(act.parameters, "sensitive") &&
        cbor::find(act.parameters, "sensitive")->as_bool();
    Bytes stored = *clear;
    std::string encryption = "none";
    if (sensitive) {
      auto encrypted = protect(*clear);
      std::fill(clear->begin(), clear->end(), 0);
      if (!encrypted) return tl::unexpected(encrypted.error());
      stored = std::move(*encrypted);
      encryption = "os-envelope";
    }
    auto directory = storage_directory(act.parameters, "blobs");
    if (!directory) return tl::unexpected(directory.error());
    const auto stored_hash = sha256_hex(stored);
    const auto path = *directory / (stored_hash + ".blob");
    if (auto written = write_once(path, stored); !written)
      return tl::unexpected(written.error());
    return emit(beam, "blob.stored", "tokmon.storage.result.v1", cbor::object({
        {"sha256", content_hash}, {"stored_sha256", stored_hash},
        {"path", path.generic_string()},
        {"bytes", static_cast<std::int64_t>(stored.size())}, {"immutable", true},
        {"encrypted", sensitive}, {"encryption", encryption}, {"fact_source", false}}));
  }

  if (act.kind == "blob.verify") {
    const auto* path = cbor::find(act.parameters, "path");
    const auto* digest = cbor::find(act.parameters, "stored_sha256");
    if (!path || !digest || digest->as_string().size() != 64u)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "blob.verify requires path and stored_sha256"));
    auto root = storage_root(act.parameters);
    if (!root) return tl::unexpected(root.error());
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(
        std::filesystem::path(path->as_string()), error);
    if (error || resolved.lexically_relative(*root).empty() ||
        *resolved.lexically_relative(*root).begin() == "..")
      return tl::unexpected(make_error(ErrorCode::permission_denied,
                                       "blob path escapes storage_root"));
    auto bytes = read_bytes(resolved);
    if (!bytes) return tl::unexpected(bytes.error());
    const auto actual = sha256_hex(*bytes);
    if (actual != digest->as_string())
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "blob integrity verification failed"));
    return emit(beam, "blob.verified", "tokmon.storage.verify.v1",
                cbor::object({{"stored_sha256", actual}, {"valid", true}}));
  }

  if (act.kind == "backup.restore") {
    const auto* backup = cbor::find(act.parameters, "backup_path");
    const auto* expected = cbor::find(act.parameters, "sha256");
    if (!backup || !expected)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "backup.restore requires backup_path and sha256"));
    auto bytes = read_bytes(std::filesystem::path(backup->as_string()));
    if (!bytes) return tl::unexpected(bytes.error());
    if (sha256_hex(*bytes) != expected->as_string())
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "backup manifest hash mismatch"));
    auto decoded = cbor::decode(*bytes);
    const auto* files = decoded ? cbor::find(*decoded, "files") : nullptr;
    auto root = storage_root(act.parameters);
    if (!decoded || !files || !files->as_array() || !root)
      return tl::unexpected(decoded ? (root ? make_error(ErrorCode::schema_mismatch,
          "backup files are invalid") : root.error()) : decoded.error());
    std::int64_t restored = 0;
    for (const auto& item : *files->as_array()) {
      const auto* relative = cbor::find(item, "path");
      const auto* content = cbor::find(item, "content");
      if (!relative || !content || !std::holds_alternative<cbor::Value::Bytes>(content->data))
        return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                         "backup entry is invalid"));
      const auto candidate = (*root / std::filesystem::path(relative->as_string())).lexically_normal();
      const auto rel = candidate.lexically_relative(*root);
      if (rel.empty() || *rel.begin() == "..")
        return tl::unexpected(make_error(ErrorCode::permission_denied,
                                         "backup entry escapes storage_root"));
      std::filesystem::create_directories(candidate.parent_path());
      if (auto written = write_once(candidate, std::get<cbor::Value::Bytes>(content->data));
          !written) return tl::unexpected(written.error());
      ++restored;
    }
    return emit(beam, "backup.restored", "tokmon.backup.result.v1",
                cbor::object({{"sha256", std::string(expected->as_string())},
                              {"restored_files", restored}, {"overwritten", false}}));
  }

  Bytes bytes;
  std::string directory_name;
  std::string kind;
  cbor::Value::Array manifest;
  if (act.kind == "backup.create") {
    auto root = storage_root(act.parameters);
    if (!root) return tl::unexpected(root.error());
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(
             *root, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error)) {
      if (error) return tl::unexpected(make_error(ErrorCode::io_error,
                                                   "cannot enumerate backup input"));
      if (!iterator->is_regular_file(error) || error ||
          iterator->path().parent_path().filename() == "backups") continue;
      auto content = read_bytes(iterator->path());
      if (!content) return tl::unexpected(content.error());
      auto entry = manifest_entry(*root, iterator->path(), *content);
      (*entry.as_map())["content"] = cbor::Value(std::move(*content));
      manifest.push_back(std::move(entry));
    }
    std::ranges::sort(manifest, [](const auto& left, const auto& right) {
      return cbor::find(left, "path")->as_string() < cbor::find(right, "path")->as_string();
    });
    bytes = cbor::encode(cbor::object({{"version", 1}, {"files", std::move(manifest)},
        {"tail", photons.latest() ? static_cast<std::int64_t>(photons.latest()->sequence) : 0}}));
    directory_name = "backups"; kind = "backup.created";
  } else {
    bytes = cbor::encode(to_cbor(photons));
    if (act.kind == "photon.export") { directory_name = "exports"; kind = "photon.exported"; }
    else if (act.kind == "checkpoint.build") {
      directory_name = "checkpoints"; kind = "checkpoint.built";
    } else { directory_name = "segments"; kind = "archive.sealed"; }
  }
  auto directory = storage_directory(act.parameters, directory_name);
  if (!directory) return tl::unexpected(directory.error());
  const auto digest = sha256_hex(bytes);
  const auto path = *directory / (digest + ".cbor");
  if (auto written = write_once(path, bytes); !written)
    return tl::unexpected(written.error());
  return emit(beam, kind, "tokmon.storage.result.v1", cbor::object({
      {"sha256", digest}, {"path", path.generic_string()},
      {"bytes", static_cast<std::int64_t>(bytes.size())}, {"immutable", true},
      {"tail_sequence", photons.latest() ?
          static_cast<std::int64_t>(photons.latest()->sequence) : 0},
      {"fact_source", false}}));
}

}  // namespace tokmon::builtin
