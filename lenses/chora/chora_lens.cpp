#include "lenses/chora/chora_lens.hpp"

#include <filesystem>
#include <fstream>

#include "tokmon/hash.hpp"

namespace {

tokmon::Result<std::filesystem::path> storage_directory(
    const tokmon::cbor::Value& parameters, const std::string_view child) {
  const auto* field = tokmon::cbor::find(parameters, "storage_root");
  if (!field || field->as_string().empty())
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::schema_mismatch,
                                             "storage_root is required"));
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(
      std::filesystem::path(field->as_string()), error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot canonicalize storage_root"));
  const auto directory = root / child;
  std::filesystem::create_directories(directory, error);
  if (error)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot create storage directory"));
  return directory;
}

tokmon::Result<void> write_once(const std::filesystem::path& path,
                                const std::span<const std::uint8_t> bytes) {
  if (std::filesystem::exists(path)) return {};
  std::ofstream output(path, std::ios::binary | std::ios::out);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output)
    return tl::unexpected(tokmon::make_error(tokmon::ErrorCode::io_error,
                                             "cannot persist immutable storage object"));
  return {};
}

}  // namespace

namespace tokmon::builtin {

ChoraLens::ChoraLens() : LensBase(make_manifest("chora", "Chora / 不可改写光感底片",
    {"fact.storage", "diagnostic.capacity"},
    {{"*", "*"}},
    {{"photon.export", "tokmon.photon.export.v1"},
     {"blob.put", "tokmon.blob.put.v1"},
     {"checkpoint.build", "tokmon.checkpoint.build.v1"},
     {"archive.seal", "tokmon.archive.seal.v1"}},
    {"photon.emit", "blob.write", "artifact.write", "log.write"})) {}

Result<void> ChoraLens::view(const PhotonWindow& photons, SurfaceBuilder& surface) {
  if (auto status = ready(); !status) return status;
  const auto* tail = photons.latest();
  if (auto result = identify(surface, "fact.storage", cbor::object({
      {"tail_sequence", tail ? static_cast<std::int64_t>(tail->sequence) : 0},
      {"window_photons", static_cast<std::int64_t>(photons.photons().size())},
      {"append_only", true}, {"single_writer", true}, {"wal", true}})); !result)
    return result;
  return surface.add("diagnostic.capacity", "active-window", cbor::object({
      {"photon_count", static_cast<std::int64_t>(photons.photons().size())},
      {"derived_objects_rebuildable", true}, {"writer_token", "Nyxia-owned"}}), 0);
}

Result<RefractionResult> ChoraLens::refract(const PhotonWindow& photons, const Act& act,
                                             RefractionBeam& beam) {
  if (!accepts(act)) return RefractionResult{.status = RefractionStatus::passed};
  std::vector<std::uint8_t> bytes;
  std::string directory_name;
  std::string kind;
  if (act.kind == "blob.put") {
    const auto* content = cbor::find(act.parameters, "content");
    if (!content)
      return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                       "blob.put content is required"));
    if (const auto* binary = std::get_if<cbor::Value::Bytes>(&content->data)) bytes = *binary;
    else if (const auto* text = std::get_if<std::string>(&content->data))
      bytes.assign(text->begin(), text->end());
    else return tl::unexpected(make_error(ErrorCode::schema_mismatch,
                                           "blob.put content must be bytes or string"));
    directory_name = "blobs"; kind = "blob.stored";
  } else {
    bytes = cbor::encode(to_cbor(photons));
    if (act.kind == "photon.export") {
      directory_name = "exports"; kind = "photon.exported";
    } else if (act.kind == "checkpoint.build") {
      directory_name = "checkpoints"; kind = "checkpoint.built";
    } else {
      directory_name = "segments"; kind = "archive.sealed";
    }
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
      {"fact_source", false}}));
}

}  // namespace tokmon::builtin
