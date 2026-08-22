#include "tokmon/photon_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

#include "tokmon/hash.hpp"

namespace tokmon {
namespace {

Error sqlite_error(sqlite3* database, const std::string_view context) {
  return make_error(ErrorCode::storage_error,
                    std::string(context) + ": " +
                        (database ? sqlite3_errmsg(database) : "no database"));
}

Result<void> execute(sqlite3* database, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(database, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string detail = message ? message : sqlite3_errmsg(database);
    sqlite3_free(message);
    return tl::unexpected(make_error(ErrorCode::storage_error, std::move(detail)));
  }
  return {};
}

std::string hash_material(const Photon& photon) {
  auto encoded = cbor::encode(photon.payload);
  std::string material;
  material.reserve(256u + encoded.size());
  material.append(photon.previous_hash).push_back('\0');
  material.append(std::to_string(photon.sequence)).push_back('\0');
  material.append(photon.id).push_back('\0');
  material.append(photon.ray).push_back('\0');
  material.append(photon.parent.value_or("")).push_back('\0');
  material.append(photon.kind).push_back('\0');
  material.append(photon.schema).push_back('\0');
  material.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  material.push_back('\0');
  material.append(std::to_string(photon.epoch)).push_back('\0');
  material.append(std::to_string(photon.committed_at_ms)).push_back('\0');
  material.append(photon.caused_by_act);
  return material;
}

std::string column_text(sqlite3_stmt* statement, const int index) {
  const auto* text = sqlite3_column_text(statement, index);
  return text ? reinterpret_cast<const char*>(text) : std::string{};
}

Result<Photon> row_to_photon(sqlite3_stmt* statement) {
  Photon photon;
  photon.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
  photon.id = column_text(statement, 1);
  photon.ray = column_text(statement, 2);
  if (sqlite3_column_type(statement, 3) != SQLITE_NULL) photon.parent = column_text(statement, 3);
  photon.kind = column_text(statement, 4);
  photon.schema = column_text(statement, 5);
  const auto* payload = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 6));
  const auto payload_size = sqlite3_column_bytes(statement, 6);
  if (payload_size < 0 || (payload_size > 0 && payload == nullptr))
    return tl::unexpected(make_error(ErrorCode::storage_error, "invalid Photon payload blob"));
  auto decoded = cbor::decode(std::span(payload, static_cast<std::size_t>(payload_size)));
  if (!decoded) return tl::unexpected(decoded.error());
  photon.payload = std::move(*decoded);
  photon.epoch = static_cast<MountEpoch>(sqlite3_column_int64(statement, 7));
  photon.committed_at_ms = sqlite3_column_int64(statement, 8);
  photon.previous_hash = column_text(statement, 9);
  photon.hash = column_text(statement, 10);
  photon.caused_by_act = column_text(statement, 11);
  return photon;
}

constexpr const char* select_columns =
    "sequence,id,ray,parent,kind,schema,payload,epoch,committed_at_ms,"
    "previous_hash,hash,caused_by_act";

}  // namespace

PhotonStore::PhotonStore() = default;
PhotonStore::~PhotonStore() {
  std::scoped_lock lock(mutex_);
  if (database_) sqlite3_close_v2(database_);
}

Result<void> PhotonStore::open(const std::filesystem::path& database) {
  std::scoped_lock lock(mutex_);
  if (database_ != nullptr)
    return tl::unexpected(make_error(ErrorCode::invalid_state,
                                     "PhotonStore is already open"));
  std::error_code error;
  std::filesystem::create_directories(database.parent_path(), error);
  if (error)
    return tl::unexpected(make_error(ErrorCode::io_error,
                                     "cannot create data directory: " + error.message()));
  const auto utf8 = database.u8string();
  const std::string path(utf8.begin(), utf8.end());
  if (sqlite3_open_v2(path.c_str(), &database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const auto failure = sqlite_error(database_, "open Photon database");
    if (database_) sqlite3_close_v2(database_);
    database_ = nullptr;
    return tl::unexpected(failure);
  }
  sqlite3_busy_timeout(database_, 5000);
  path_ = database;
  return initialize_schema();
}

Result<void> PhotonStore::initialize_schema() {
  return execute(database_, R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS photons(
  sequence INTEGER PRIMARY KEY,
  id TEXT NOT NULL UNIQUE,
  ray TEXT NOT NULL,
  parent TEXT,
  kind TEXT NOT NULL,
  schema TEXT NOT NULL,
  payload BLOB NOT NULL,
  epoch INTEGER NOT NULL,
  committed_at_ms INTEGER NOT NULL,
  previous_hash TEXT NOT NULL,
  hash TEXT NOT NULL UNIQUE,
  caused_by_act TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS photons_ray_sequence ON photons(ray, sequence);
CREATE INDEX IF NOT EXISTS photons_kind_sequence ON photons(kind, sequence);
CREATE TRIGGER IF NOT EXISTS photons_no_update
BEFORE UPDATE ON photons BEGIN SELECT RAISE(ABORT, 'committed photons are immutable'); END;
CREATE TRIGGER IF NOT EXISTS photons_no_delete
BEFORE DELETE ON photons BEGIN SELECT RAISE(ABORT, 'committed photons cannot be deleted'); END;
)SQL");
}

Result<Photon> PhotonStore::append(PhotonDraft draft) {
  std::vector<Observer> observers;
  Photon photon;
  {
    std::unique_lock lock(mutex_);
    if (!database_)
      return tl::unexpected(make_error(ErrorCode::invalid_state,
                                       "PhotonStore is not open"));
    if (draft.ray.empty() || draft.kind.empty() || draft.schema.empty())
      return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                       "Photon ray, kind and schema are required"));
    auto begin = execute(database_, "BEGIN IMMEDIATE;");
    if (!begin) return tl::unexpected(begin.error());
    bool committed = false;
    const auto rollback = [&] { if (!committed) sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    sqlite3_stmt* state = nullptr;
    const char* state_sql =
        "SELECT COALESCE(MAX(sequence),0), COALESCE((SELECT hash FROM photons "
        "ORDER BY sequence DESC LIMIT 1),''), COALESCE((SELECT id FROM photons "
        "WHERE ray=? ORDER BY sequence DESC LIMIT 1),'') FROM photons";
    if (sqlite3_prepare_v2(database_, state_sql, -1, &state, nullptr) != SQLITE_OK) {
      rollback(); return tl::unexpected(sqlite_error(database_, "prepare append state"));
    }
    sqlite3_bind_text(state, 1, draft.ray.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(state) != SQLITE_ROW) {
      sqlite3_finalize(state); rollback();
      return tl::unexpected(sqlite_error(database_, "read append state"));
    }
    photon.sequence = static_cast<std::uint64_t>(sqlite3_column_int64(state, 0)) + 1u;
    photon.previous_hash = column_text(state, 1);
    const auto last_ray_photon = column_text(state, 2);
    sqlite3_finalize(state);

    photon.id = make_id("photon"); photon.ray = std::move(draft.ray);
    photon.parent = draft.parent ? std::move(draft.parent) :
        (last_ray_photon.empty() ? std::optional<PhotonId>{} : std::optional<PhotonId>{last_ray_photon});
    photon.kind = std::move(draft.kind); photon.schema = std::move(draft.schema);
    photon.payload = std::move(draft.payload); photon.epoch = draft.epoch;
    photon.committed_at_ms = unix_time_ms(); photon.caused_by_act = std::move(draft.caused_by_act);
    photon.hash = sha256_hex(hash_material(photon));
    const auto payload = cbor::encode(photon.payload);

    sqlite3_stmt* insert = nullptr;
    const char* insert_sql =
        "INSERT INTO photons(sequence,id,ray,parent,kind,schema,payload,epoch,"
        "committed_at_ms,previous_hash,hash,caused_by_act) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(database_, insert_sql, -1, &insert, nullptr) != SQLITE_OK) {
      rollback(); return tl::unexpected(sqlite_error(database_, "prepare Photon append"));
    }
    sqlite3_bind_int64(insert, 1, static_cast<sqlite3_int64>(photon.sequence));
    sqlite3_bind_text(insert, 2, photon.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 3, photon.ray.c_str(), -1, SQLITE_TRANSIENT);
    if (photon.parent) sqlite3_bind_text(insert, 4, photon.parent->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert, 4);
    sqlite3_bind_text(insert, 5, photon.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 6, photon.schema.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(insert, 7, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 8, static_cast<sqlite3_int64>(photon.epoch));
    sqlite3_bind_int64(insert, 9, photon.committed_at_ms);
    sqlite3_bind_text(insert, 10, photon.previous_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 11, photon.hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 12, photon.caused_by_act.c_str(), -1, SQLITE_TRANSIENT);
    const auto result = sqlite3_step(insert);
    sqlite3_finalize(insert);
    if (result != SQLITE_DONE) {
      rollback(); return tl::unexpected(sqlite_error(database_, "append Photon"));
    }
    auto commit = execute(database_, "COMMIT;");
    if (!commit) { rollback(); return tl::unexpected(commit.error()); }
    committed = true;
    observers = observers_;
  }
  for (const auto& observer : observers) observer(photon);
  return photon;
}

Result<std::vector<Photon>> PhotonStore::read_ray(const RayId& ray,
                                                   const std::uint64_t after_sequence,
                                                   const std::size_t limit) const {
  const std::string sql = std::string("SELECT ") + select_columns +
      " FROM photons WHERE ray=? AND sequence>? ORDER BY sequence LIMIT ?";
  return read_query(sql.c_str(), &ray, after_sequence, limit);
}

Result<std::vector<Photon>> PhotonStore::read_all(const std::uint64_t after_sequence,
                                                   const std::size_t limit) const {
  const std::string sql = std::string("SELECT ") + select_columns +
      " FROM photons WHERE sequence>? ORDER BY sequence LIMIT ?";
  return read_query(sql.c_str(), nullptr, after_sequence, limit);
}

Result<std::vector<Photon>> PhotonStore::read_query(const char* sql, const std::string* ray,
                                                     const std::uint64_t after_sequence,
                                                     const std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  if (!database_)
    return tl::unexpected(make_error(ErrorCode::invalid_state, "PhotonStore is not open"));
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK)
    return tl::unexpected(sqlite_error(database_, "prepare Photon query"));
  int parameter = 1;
  if (ray) sqlite3_bind_text(statement, parameter++, ray->c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, parameter++, static_cast<sqlite3_int64>(after_sequence));
  sqlite3_bind_int64(statement, parameter, static_cast<sqlite3_int64>(
      std::min(limit, static_cast<std::size_t>(100'000))));
  std::vector<Photon> photons;
  for (;;) {
    const auto step = sqlite3_step(statement);
    if (step == SQLITE_DONE) break;
    if (step != SQLITE_ROW) {
      const auto error = sqlite_error(database_, "read Photon query");
      sqlite3_finalize(statement); return tl::unexpected(error);
    }
    auto photon = row_to_photon(statement);
    if (!photon) { sqlite3_finalize(statement); return tl::unexpected(photon.error()); }
    photons.push_back(std::move(*photon));
  }
  sqlite3_finalize(statement);
  return photons;
}

Result<void> PhotonStore::verify() const {
  auto photons = read_all(0, 100'000);
  if (!photons) return tl::unexpected(photons.error());
  std::string previous;
  std::uint64_t expected_sequence = 1;
  for (const auto& photon : *photons) {
    if (photon.sequence != expected_sequence++)
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Photon sequence gap or reuse"));
    if (photon.previous_hash != previous)
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Photon previous hash mismatch"));
    if (photon.hash != sha256_hex(hash_material(photon)))
      return tl::unexpected(make_error(ErrorCode::integrity_error,
                                       "Photon content hash mismatch"));
    previous = photon.hash;
  }
  return {};
}

Result<void> PhotonStore::checkpoint() const {
  std::scoped_lock lock(mutex_);
  if (!database_)
    return tl::unexpected(make_error(ErrorCode::invalid_state, "PhotonStore is not open"));
  if (sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_PASSIVE,
                                nullptr, nullptr) != SQLITE_OK)
    return tl::unexpected(sqlite_error(database_, "checkpoint Photon database"));
  return {};
}

void PhotonStore::subscribe(Observer observer) {
  std::scoped_lock lock(mutex_);
  observers_.push_back(std::move(observer));
}

std::filesystem::path PhotonStore::path() const {
  std::scoped_lock lock(mutex_);
  return path_;
}

}  // namespace tokmon

