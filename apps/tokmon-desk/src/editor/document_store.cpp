#include "editor/document_store.hpp"

#include <zep/gap_buffer.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace tokmon::desk {
namespace {

std::optional<std::string> read_all(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

struct EditRecord {
  std::size_t offset{0};
  std::string erased;
  std::string inserted;
};

bool valid_utf8(const std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first < 0x80) {
      ++index;
      continue;
    }
    int length = 0;
    char32_t codepoint = 0;
    if ((first & 0xe0u) == 0xc0u) { length = 2; codepoint = first & 0x1fu; }
    else if ((first & 0xf0u) == 0xe0u) { length = 3; codepoint = first & 0x0fu; }
    else if ((first & 0xf8u) == 0xf0u) { length = 4; codepoint = first & 0x07u; }
    else return false;
    if (index + static_cast<std::size_t>(length) > value.size())
      return false;
    for (int continuation = 1; continuation < length; ++continuation) {
      const auto byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0u) != 0x80u)
        return false;
      codepoint = (codepoint << 6u) | (byte & 0x3fu);
    }
    if ((length == 2 && codepoint < 0x80) ||
        (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return false;
    index += static_cast<std::size_t>(length);
  }
  return true;
}

LineEnding line_ending(const std::string_view value) {
  std::size_t lf = 0, crlf = 0, cr = 0;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '\r') {
      if (index + 1 < value.size() && value[index + 1] == '\n') {
        ++crlf;
        ++index;
      } else {
        ++cr;
      }
    } else if (value[index] == '\n') {
      ++lf;
    }
  }
  const auto kinds = static_cast<int>(lf > 0) + static_cast<int>(crlf > 0) +
                     static_cast<int>(cr > 0);
  if (kinds > 1) return LineEnding::mixed;
  if (crlf) return LineEnding::crlf;
  if (lf) return LineEnding::lf;
  if (cr) return LineEnding::cr;
  return LineEnding::none;
}

} // namespace

struct DocumentStore::Document {
  DocumentSnapshot snapshot;
  GapBuffer<std::uint8_t> buffer;
  std::vector<EditRecord> undo;
  std::vector<EditRecord> redo;

  void apply(const EditRecord& record, bool forward) {
    const auto& remove = forward ? record.erased : record.inserted;
    const auto& insert = forward ? record.inserted : record.erased;
    if (!remove.empty()) {
      const auto start = buffer.cbegin() + record.offset;
      buffer.erase(start, start + remove.size());
    }
    if (!insert.empty()) {
      buffer.insert(buffer.cbegin() + record.offset,
                    insert.begin(), insert.end());
    }
    snapshot.text = buffer.string();
    ++snapshot.version;
    snapshot.dirty = DocumentStore::content_hash(snapshot.text) != disk_hash();
  }

  [[nodiscard]] std::uint64_t disk_hash() const { return snapshot.disk_hash; }
};

DocumentStore::DocumentStore() = default;
DocumentStore::~DocumentStore() = default;

std::string DocumentStore::key(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  const auto value = (error ? path : canonical).u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::uint64_t DocumentStore::content_hash(std::string_view value) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::optional<DocumentSnapshot> DocumentStore::open(
    const std::filesystem::path& path, std::string& error) {
  std::error_code file_error;
  const auto bytes = std::filesystem::file_size(path, file_error);
  if (file_error || bytes > 64u * 1024u * 1024u) {
    error = file_error ? "cannot inspect document" :
                         "document exceeds the 64 MiB editable limit";
    return std::nullopt;
  }
  const auto data = read_all(path);
  if (!data) {
    error = "cannot open document";
    return std::nullopt;
  }
  if (data->find('\0') != std::string::npos) {
    error = "binary document is not editable";
    return std::nullopt;
  }
  const bool bom = data->size() >= 3 &&
      static_cast<unsigned char>((*data)[0]) == 0xef &&
      static_cast<unsigned char>((*data)[1]) == 0xbb &&
      static_cast<unsigned char>((*data)[2]) == 0xbf;
  if (!valid_utf8(std::string_view(*data).substr(bom ? 3 : 0))) {
    error = "document is not valid UTF-8";
    return std::nullopt;
  }
  auto document = std::make_unique<Document>();
  std::error_code canonical_error;
  auto canonical = std::filesystem::weakly_canonical(path, canonical_error);
  if (canonical_error)
    canonical = path;
  const auto permissions = std::filesystem::status(canonical, file_error).permissions();
  const bool writable = file_error ||
      (permissions & (std::filesystem::perms::owner_write |
                      std::filesystem::perms::group_write |
                      std::filesystem::perms::others_write)) !=
          std::filesystem::perms::none;
  document->snapshot = {.path = canonical, .text = *data, .version = 1,
                        .disk_hash = content_hash(*data),
                        .encoding = bom ? TextEncoding::utf8_bom
                                        : TextEncoding::utf8,
                        .line_ending = line_ending(*data),
                        .read_only = !writable,
                        .large_file = bytes > 8u * 1024u * 1024u};
  document->buffer.assign(data->begin(), data->end());
  auto result = document->snapshot;
  documents_.insert_or_assign(key(path), std::move(document));
  return result;
}

bool DocumentStore::adopt(DocumentSnapshot snapshot, std::string& error) {
  if (snapshot.path.empty() || snapshot.version == 0 ||
      snapshot.text.size() > 64u * 1024u * 1024u ||
      snapshot.text.find('\0') != std::string::npos ||
      !valid_utf8(std::string_view(snapshot.text).substr(
          snapshot.encoding == TextEncoding::utf8_bom ? 3 : 0))) {
    error = "invalid detached document snapshot";
    return false;
  }
  auto document = std::make_unique<Document>();
  document->buffer.assign(snapshot.text.begin(), snapshot.text.end());
  document->snapshot = std::move(snapshot);
  const auto document_key = key(document->snapshot.path);
  documents_.insert_or_assign(document_key, std::move(document));
  return true;
}

bool DocumentStore::edit(const std::filesystem::path& path,
                         const std::size_t offset,
                         const std::size_t erase_count,
                         std::string replacement,
                         const std::uint64_t expected_version,
                         std::string& error) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end()) {
    error = "document is not open";
    return false;
  }
  auto& document = *iterator->second;
  if (document.snapshot.read_only) {
    error = "document is read-only";
    return false;
  }
  if (document.snapshot.version != expected_version ||
      offset > document.snapshot.text.size()) {
    error = "stale document edit";
    return false;
  }
  const auto count = std::min(erase_count,
                              document.snapshot.text.size() - offset);
  EditRecord record{offset, document.snapshot.text.substr(offset, count),
                    std::move(replacement)};
  document.apply(record, true);
  document.undo.push_back(record);
  document.redo.clear();
  document.snapshot.can_undo = true;
  document.snapshot.can_redo = false;
  return true;
}

bool DocumentStore::undo(const std::filesystem::path& path,
                         const std::uint64_t expected_version,
                         std::string& error) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end() ||
      iterator->second->snapshot.version != expected_version) {
    error = "stale document undo";
    return false;
  }
  auto& document = *iterator->second;
  if (document.snapshot.read_only) {
    error = "document is read-only";
    return false;
  }
  if (document.undo.empty()) {
    error = "nothing to undo";
    return false;
  }
  auto record = std::move(document.undo.back());
  document.undo.pop_back();
  document.apply(record, false);
  document.redo.push_back(std::move(record));
  document.snapshot.can_undo = !document.undo.empty();
  document.snapshot.can_redo = true;
  return true;
}

bool DocumentStore::redo(const std::filesystem::path& path,
                         const std::uint64_t expected_version,
                         std::string& error) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end() ||
      iterator->second->snapshot.version != expected_version) {
    error = "stale document redo";
    return false;
  }
  auto& document = *iterator->second;
  if (document.snapshot.read_only) {
    error = "document is read-only";
    return false;
  }
  if (document.redo.empty()) {
    error = "nothing to redo";
    return false;
  }
  auto record = std::move(document.redo.back());
  document.redo.pop_back();
  document.apply(record, true);
  document.undo.push_back(std::move(record));
  document.snapshot.can_undo = true;
  document.snapshot.can_redo = !document.redo.empty();
  return true;
}

bool DocumentStore::save(const std::filesystem::path& path,
                         const std::uint64_t expected_version,
                         std::string& error) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end()) {
    error = "document is not open";
    return false;
  }
  auto& document = *iterator->second;
  if (document.snapshot.read_only) {
    error = "document is read-only";
    return false;
  }
  if (document.snapshot.version != expected_version) {
    error = "stale document save";
    return false;
  }
  if (const auto current = read_all(document.snapshot.path);
      current && content_hash(*current) != document.snapshot.disk_hash) {
    document.snapshot.external_conflict = true;
    error = "document changed on disk";
    return false;
  }
  auto temporary = document.snapshot.path;
  temporary += ".tokmon-desk.tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !(output << document.snapshot.text)) {
      error = "cannot write temporary document";
      return false;
    }
    output.flush();
    if (!output) {
      error = "cannot flush temporary document";
      return false;
    }
  }
  std::error_code replace_error;
#if defined(_WIN32)
  const auto target = document.snapshot.path.wstring();
  const auto source = temporary.wstring();
  if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr,
                    REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) &&
      !MoveFileExW(source.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    replace_error = std::error_code(static_cast<int>(GetLastError()),
                                    std::system_category());
  }
#else
  std::filesystem::rename(temporary, document.snapshot.path, replace_error);
#endif
  if (replace_error) {
    std::filesystem::remove(temporary);
    error = "cannot atomically replace document: " + replace_error.message();
    return false;
  }
  document.snapshot.disk_hash = content_hash(document.snapshot.text);
  document.snapshot.dirty = false;
  document.snapshot.external_conflict = false;
  return true;
}

bool DocumentStore::reload(const std::filesystem::path& path,
                           const bool discard_local_changes,
                           std::string& error) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end()) {
    error = "document is not open";
    return false;
  }
  auto& document = *iterator->second;
  if (document.snapshot.dirty && !discard_local_changes) {
    document.snapshot.external_conflict = true;
    error = "document has unsaved local changes";
    return false;
  }
  const auto data = read_all(document.snapshot.path);
  if (!data) {
    error = "document no longer exists";
    return false;
  }
  document.buffer.assign(data->begin(), data->end());
  document.snapshot.text = *data;
  document.snapshot.disk_hash = content_hash(*data);
  document.snapshot.encoding = data->starts_with("\xef\xbb\xbf")
      ? TextEncoding::utf8_bom : TextEncoding::utf8;
  document.snapshot.line_ending = line_ending(*data);
  ++document.snapshot.version;
  document.snapshot.dirty = false;
  document.snapshot.external_conflict = false;
  document.snapshot.can_undo = false;
  document.snapshot.can_redo = false;
  document.undo.clear();
  document.redo.clear();
  return true;
}

std::optional<DocumentSnapshot> DocumentStore::snapshot(
    const std::filesystem::path& path) const {
  const auto iterator = documents_.find(key(path));
  return iterator == documents_.end()
             ? std::nullopt
             : std::optional<DocumentSnapshot>(iterator->second->snapshot);
}

void DocumentStore::observe_external_change(
    const std::filesystem::path& path) {
  const auto iterator = documents_.find(key(path));
  if (iterator == documents_.end())
    return;
  auto& document = *iterator->second;
  const auto current = read_all(document.snapshot.path);
  if (!current || content_hash(*current) != document.snapshot.disk_hash) {
    if (document.snapshot.dirty)
      document.snapshot.external_conflict = true;
    else {
      std::string ignored;
      (void)reload(path, true, ignored);
    }
  }
}

} // namespace tokmon::desk
