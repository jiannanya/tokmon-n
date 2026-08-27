#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/error.hpp"
#include "tokmon/photon.hpp"

struct sqlite3;

namespace tokmon {

class PhotonStore {
 public:
  using Observer = std::function<void(const Photon&)>;

  PhotonStore();
  ~PhotonStore();
  PhotonStore(const PhotonStore&) = delete;
  PhotonStore& operator=(const PhotonStore&) = delete;

  Result<void> open(const std::filesystem::path& database);
  Result<Photon> append(PhotonDraft draft);
  Result<std::vector<Photon>> read_ray(const RayId& ray,
                                       std::uint64_t after_sequence = 0,
                                       std::size_t limit = 4096) const;
  Result<std::vector<Photon>> read_all(std::uint64_t after_sequence = 0,
                                       std::size_t limit = 4096) const;
  Result<std::optional<Photon>> read_latest_kind(std::string_view kind) const;
  // Verifies only the immutable tail after the last durable checkpoint. The
  // first call performs a complete verification and establishes that anchor.
  Result<void> verify() const;
  Result<void> checkpoint() const;
  void subscribe(Observer observer);
  [[nodiscard]] std::filesystem::path path() const;

 private:
  Result<void> initialize_schema();
  Result<std::vector<Photon>> read_query(const char* sql, const std::string* ray,
                                         std::uint64_t after_sequence,
                                         std::size_t limit) const;
  mutable std::mutex mutex_;
  sqlite3* database_{nullptr};
  std::filesystem::path path_;
  std::vector<Observer> observers_;
};

}  // namespace tokmon

