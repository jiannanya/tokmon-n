#include "state/desk_state_store.hpp"

#include "tokmon/json.hpp"

#include <chrono>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace tokmon::desk {
namespace {

constexpr std::int64_t kSchema = 1;
constexpr std::uintmax_t kMaximumStateBytes = 4u * 1024u * 1024u;

std::string system_error_message(const std::error_code& error) {
  return error ? error.message() : "unknown filesystem error";
}

bool atomic_write(const std::filesystem::path& destination,
                  const std::string& contents, std::string& error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(destination.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "cannot create Desktop state directory: " +
            system_error_message(filesystem_error);
    return false;
  }
  const auto temporary = destination.parent_path() /
      (destination.filename().string() + ".tmp-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#if defined(_WIN32)
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "cannot create temporary Desktop state file";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(contents.size() - offset, 1024u * 1024u));
    if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) ||
        written == 0) {
      error = "cannot write temporary Desktop state file";
      CloseHandle(file);
      DeleteFileW(temporary.c_str());
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(file)) {
    error = "cannot flush temporary Desktop state file";
    CloseHandle(file);
    DeleteFileW(temporary.c_str());
    return false;
  }
  CloseHandle(file);
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "cannot atomically replace Desktop state file";
    DeleteFileW(temporary.c_str());
    return false;
  }
#else
  const int descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    error = "cannot create temporary Desktop state file: " +
            std::string(std::strerror(errno));
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto written = ::write(descriptor, contents.data() + offset,
                                 contents.size() - offset);
    if (written <= 0) {
      error = "cannot write temporary Desktop state file: " +
              std::string(std::strerror(errno));
      ::close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    error = "cannot flush temporary Desktop state file: " +
            std::string(std::strerror(errno));
    ::close(descriptor);
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
  ::close(descriptor);
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    error = "cannot atomically replace Desktop state file: " +
            system_error_message(filesystem_error);
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
#endif
  return true;
}

void quarantine(const std::filesystem::path& source) {
  std::error_code error;
  if (!std::filesystem::exists(source, error))
    return;
  const auto suffix = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count());
  const auto destination = std::filesystem::path(
      source.string() + ".corrupt-" + suffix);
  std::filesystem::rename(source, destination, error);
  if (error) {
    error.clear();
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (!error)
      std::filesystem::remove(source, error);
  }
}

} // namespace

DeskStateStore::DeskStateStore(DeskAppPaths paths) : paths_(std::move(paths)) {}

tokmon::cbor::Value DeskStateStore::load_document(
    const std::filesystem::path& path, const std::string_view payload_key,
    tokmon::cbor::Value fallback, std::string& warning) const {
  warning.clear();
  std::error_code error;
  if (!std::filesystem::exists(path, error))
    return fallback;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumStateBytes) {
    warning = "Desktop state is unreadable or exceeds the size limit; it was quarantined";
    quarantine(path);
    return fallback;
  }
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  auto parsed = tokmon::json::parse(text);
  if (!parsed || !parsed->as_map()) {
    warning = "Desktop state JSON is invalid; it was quarantined";
    quarantine(path);
    return fallback;
  }
  const auto* schema = tokmon::cbor::find(*parsed, "schema");
  const auto* payload = tokmon::cbor::find(*parsed, payload_key);
  if (!schema || schema->as_integer() != kSchema || !payload) {
    warning = "Desktop state schema is unsupported; defaults were used";
    return fallback;
  }
  return *payload;
}

bool DeskStateStore::save_document(const std::filesystem::path& path,
                                   const std::string_view payload_key,
                                   const tokmon::cbor::Value& value,
                                   std::string& error) const {
  auto document = tokmon::cbor::object({{"schema", kSchema}});
  document.as_map()->insert_or_assign(std::string(payload_key), value);
  return atomic_write(path, tokmon::json::stringify(document, true) + "\n", error);
}

tokmon::cbor::Value DeskStateStore::load_settings(std::string& warning) const {
  return load_document(paths_.config / "settings.json", "values",
                       tokmon::cbor::Value::Map{}, warning);
}

bool DeskStateStore::save_settings(const tokmon::cbor::Value& values,
                                   std::string& error) const {
  if (!values.as_map()) {
    error = "Desktop settings must be an object";
    return false;
  }
  return save_document(paths_.config / "settings.json", "values", values, error);
}

tokmon::cbor::Value DeskStateStore::load_navigation(std::string& warning) const {
  return load_document(paths_.state / "ui-state" / "navigation.json", "items",
                       tokmon::cbor::Value::Array{}, warning);
}

bool DeskStateStore::save_navigation(const tokmon::cbor::Value& items,
                                     std::string& error) const {
  if (!items.as_array()) {
    error = "Desktop navigation must be an array";
    return false;
  }
  return save_document(paths_.state / "ui-state" / "navigation.json", "items",
                       items, error);
}

DeskInstanceLock::DeskInstanceLock(const std::filesystem::path& lock_file) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(lock_file.parent_path(), filesystem_error);
  if (filesystem_error) {
    error_ = "cannot create Desktop runtime directory: " +
             filesystem_error.message();
    return;
  }
#if defined(_WIN32)
  handle_ = CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE)
    error_ = "another tokmon-desk instance is already using this profile";
#else
  descriptor_ = ::open(lock_file.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
    error_ = "another tokmon-desk instance is already using this profile";
  }
#endif
}

DeskInstanceLock::~DeskInstanceLock() { release(); }

DeskInstanceLock::DeskInstanceLock(DeskInstanceLock&& other) noexcept {
  *this = std::move(other);
}

DeskInstanceLock& DeskInstanceLock::operator=(DeskInstanceLock&& other) noexcept {
  if (this == &other)
    return *this;
  release();
#if defined(_WIN32)
  handle_ = other.handle_;
  other.handle_ = INVALID_HANDLE_VALUE;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
  error_ = std::move(other.error_);
  return *this;
}

bool DeskInstanceLock::acquired() const noexcept {
#if defined(_WIN32)
  return handle_ != INVALID_HANDLE_VALUE;
#else
  return descriptor_ >= 0;
#endif
}

void DeskInstanceLock::release() noexcept {
#if defined(_WIN32)
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
#else
  if (descriptor_ >= 0) {
    (void)::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
    descriptor_ = -1;
  }
#endif
}

} // namespace tokmon::desk
