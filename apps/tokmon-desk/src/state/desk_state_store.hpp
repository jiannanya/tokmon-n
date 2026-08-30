#pragma once

#include "platform/desk_app_paths.hpp"

#include "tokmon/cbor.hpp"

#include <filesystem>
#include <string>

namespace tokmon::desk {

// Owns only Desktop-local state. Agent/model/session business state remains in
// the existing daemon and workspace configuration.
class DeskStateStore final {
 public:
  explicit DeskStateStore(DeskAppPaths paths);

  [[nodiscard]] const DeskAppPaths& paths() const noexcept { return paths_; }
  [[nodiscard]] tokmon::cbor::Value load_settings(std::string& warning) const;
  [[nodiscard]] bool save_settings(const tokmon::cbor::Value& values,
                                   std::string& error) const;
  [[nodiscard]] tokmon::cbor::Value load_navigation(std::string& warning) const;
  [[nodiscard]] bool save_navigation(const tokmon::cbor::Value& items,
                                     std::string& error) const;

 private:
  [[nodiscard]] tokmon::cbor::Value load_document(
      const std::filesystem::path& path, std::string_view payload_key,
      tokmon::cbor::Value fallback, std::string& warning) const;
  [[nodiscard]] bool save_document(const std::filesystem::path& path,
                                   std::string_view payload_key,
                                   const tokmon::cbor::Value& value,
                                   std::string& error) const;

  DeskAppPaths paths_;
};

class DeskInstanceLock final {
 public:
  DeskInstanceLock() = default;
  explicit DeskInstanceLock(const std::filesystem::path& lock_file);
  ~DeskInstanceLock();

  DeskInstanceLock(const DeskInstanceLock&) = delete;
  DeskInstanceLock& operator=(const DeskInstanceLock&) = delete;
  DeskInstanceLock(DeskInstanceLock&& other) noexcept;
  DeskInstanceLock& operator=(DeskInstanceLock&& other) noexcept;

  [[nodiscard]] bool acquired() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  void release() noexcept;
#if defined(_WIN32)
  void* handle_{reinterpret_cast<void*>(static_cast<std::intptr_t>(-1))};
#else
  int descriptor_{-1};
#endif
  std::string error_;
};

} // namespace tokmon::desk
