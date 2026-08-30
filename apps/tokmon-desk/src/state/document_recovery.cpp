#include "state/document_recovery.hpp"

#include "tokmon/cbor.hpp"
#include "tokmon/hash.hpp"
#include "tokmon/json.hpp"

#include <chrono>
#include <fstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tokmon::desk {
namespace {

constexpr std::uintmax_t kMaximumRecoveryBytes = 16u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumRecoveryStoreBytes = 256u * 1024u * 1024u;

std::uintmax_t directory_size(const std::filesystem::path& root) {
  std::uintmax_t result = 0;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error))
      result += iterator->file_size(error);
    error.clear();
    if (result > kMaximumRecoveryStoreBytes)
      break;
  }
  return result;
}

std::optional<std::filesystem::path> contained_path(
    const std::filesystem::path& path, const std::filesystem::path& workspace) {
  std::error_code error;
  const auto canonical_workspace = std::filesystem::weakly_canonical(workspace, error);
  if (error)
    return std::nullopt;
  const auto canonical_path = std::filesystem::weakly_canonical(path, error);
  if (error)
    return std::nullopt;
  const auto relative = std::filesystem::relative(
      canonical_path, canonical_workspace, error);
  if (error || relative.is_absolute() || relative.empty() ||
      *relative.begin() == "..")
    return std::nullopt;
  return canonical_path;
}

bool atomic_write(const std::filesystem::path& path, std::string_view contents,
                  std::string& error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "cannot create document recovery directory: " +
            filesystem_error.message();
    return false;
  }
  auto temporary = path;
  temporary += ".tmp-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "cannot create document recovery temporary file";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    DWORD written = 0;
    const auto amount = static_cast<DWORD>(std::min<std::size_t>(
        contents.size() - offset, 1024u * 1024u));
    if (!WriteFile(file, contents.data() + offset, amount, &written, nullptr) ||
        written == 0) {
      error = "cannot write document recovery file";
      CloseHandle(file);
      DeleteFileW(temporary.c_str());
      return false;
    }
    offset += written;
  }
  if (!FlushFileBuffers(file)) {
    error = "cannot flush document recovery file";
    CloseHandle(file);
    DeleteFileW(temporary.c_str());
    return false;
  }
  CloseHandle(file);
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "cannot atomically replace document recovery file";
    DeleteFileW(temporary.c_str());
    return false;
  }
#else
  const int descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    error = "cannot create document recovery temporary file";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const auto written = ::write(descriptor, contents.data() + offset,
                                 contents.size() - offset);
    if (written <= 0) {
      error = "cannot write document recovery file";
      ::close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    error = "cannot flush document recovery file";
    ::close(descriptor);
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
  ::close(descriptor);
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    error = "cannot atomically replace document recovery file: " +
            filesystem_error.message();
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
#endif
  return true;
}

} // namespace

DocumentRecoveryStore::DocumentRecoveryStore(std::filesystem::path root)
    : root_(std::move(root)) {}

std::filesystem::path DocumentRecoveryStore::recovery_path(
    const std::filesystem::path& path) const {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  if (error)
    canonical = std::filesystem::absolute(path, error).lexically_normal();
  const auto key = tokmon::sha256_hex(canonical.generic_string());
  return root_ / key.substr(0, 2) / (key + ".json");
}

bool DocumentRecoveryStore::save(const DocumentSnapshot& snapshot,
                                 const std::filesystem::path& workspace,
                                 std::string& error) const {
  error.clear();
  const auto canonical = contained_path(snapshot.path, workspace);
  if (!canonical) {
    error = "document recovery path escapes workspace";
    return false;
  }
  if (!snapshot.dirty)
    return clear(*canonical, error);
  if (snapshot.text.size() > kMaximumRecoveryBytes) {
    error = "document is too large for crash recovery";
    return false;
  }
  const auto value = tokmon::cbor::object({
      {"schema", 1}, {"path", canonical->generic_string()},
      {"workspace", std::filesystem::weakly_canonical(workspace).generic_string()},
      {"disk_hash", static_cast<std::int64_t>(snapshot.disk_hash)},
      {"version", static_cast<std::int64_t>(snapshot.version)},
      {"text", snapshot.text}});
  const auto encoded = tokmon::json::stringify(value, false) + "\n";
  const auto destination = recovery_path(*canonical);
  std::error_code filesystem_error;
  const auto replaced_bytes = std::filesystem::exists(destination, filesystem_error)
      ? std::filesystem::file_size(destination, filesystem_error) : 0;
  const auto current_bytes = directory_size(root_);
  if (current_bytes - std::min(current_bytes, replaced_bytes) + encoded.size() >
      kMaximumRecoveryStoreBytes) {
    error = "document recovery store quota exceeded";
    return false;
  }
  return atomic_write(destination, encoded, error);
}

std::optional<DocumentRecoveryEntry> DocumentRecoveryStore::load(
    const std::filesystem::path& path, const std::filesystem::path& workspace,
    std::string& error) const {
  error.clear();
  const auto canonical = contained_path(path, workspace);
  if (!canonical) {
    error = "document recovery path escapes workspace";
    return std::nullopt;
  }
  const auto file = recovery_path(*canonical);
  std::error_code filesystem_error;
  if (!std::filesystem::exists(file, filesystem_error))
    return std::nullopt;
  if (std::filesystem::file_size(file, filesystem_error) >
      kMaximumRecoveryBytes + 64u * 1024u) {
    error = "document recovery file exceeds its size limit";
    return std::nullopt;
  }
  std::ifstream input(file, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  const auto parsed = tokmon::json::parse(text);
  if (!parsed || !parsed->as_map() ||
      !tokmon::cbor::find(*parsed, "schema") ||
      tokmon::cbor::find(*parsed, "schema")->as_integer() != 1) {
    error = "document recovery JSON is invalid";
    return std::nullopt;
  }
  const auto recovered_path = std::filesystem::path(
      std::string(tokmon::cbor::find(*parsed, "path")
                      ? tokmon::cbor::find(*parsed, "path")->as_string()
                      : std::string_view{}));
  const auto recovered_workspace = std::filesystem::path(
      std::string(tokmon::cbor::find(*parsed, "workspace")
                      ? tokmon::cbor::find(*parsed, "workspace")->as_string()
                      : std::string_view{}));
  std::error_code compare_error;
  if (std::filesystem::weakly_canonical(recovered_path, compare_error) != *canonical ||
      std::filesystem::weakly_canonical(recovered_workspace, compare_error) !=
          std::filesystem::weakly_canonical(workspace, compare_error)) {
    error = "document recovery identity does not match this workspace";
    return std::nullopt;
  }
  const auto* recovered_text = tokmon::cbor::find(*parsed, "text");
  if (!recovered_text) {
    error = "document recovery content is missing";
    return std::nullopt;
  }
  return DocumentRecoveryEntry{
      .path = *canonical,
      .workspace = std::filesystem::weakly_canonical(workspace),
      .disk_hash = static_cast<std::uint64_t>(
          tokmon::cbor::find(*parsed, "disk_hash")
              ? tokmon::cbor::find(*parsed, "disk_hash")->as_integer() : 0),
      .version = static_cast<std::uint64_t>(
          tokmon::cbor::find(*parsed, "version")
              ? tokmon::cbor::find(*parsed, "version")->as_integer() : 0),
      .text = std::string(recovered_text->as_string())};
}

bool DocumentRecoveryStore::clear(const std::filesystem::path& path,
                                  std::string& error) const {
  error.clear();
  std::error_code filesystem_error;
  const auto file = recovery_path(path);
  if (!std::filesystem::exists(file, filesystem_error))
    return true;
  if (!std::filesystem::remove(file, filesystem_error)) {
    error = filesystem_error ? filesystem_error.message()
                             : "cannot remove document recovery file";
    return false;
  }
  return true;
}

} // namespace tokmon::desk
