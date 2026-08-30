#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace tokmon::desk {

enum class TextEncoding { utf8, utf8_bom };
enum class LineEnding { none, lf, crlf, cr, mixed };

struct DocumentSnapshot {
  std::filesystem::path path;
  std::string text;
  std::uint64_t version{0};
  std::uint64_t disk_hash{0};
  bool dirty{false};
  bool external_conflict{false};
  bool can_undo{false};
  bool can_redo{false};
  TextEncoding encoding{TextEncoding::utf8};
  LineEnding line_ending{LineEnding::none};
  bool read_only{false};
  bool large_file{false};
};

// Zep's gap-buffer implementation is isolated behind this stable document
// model. UI and services only exchange immutable snapshots and versioned edits.
class DocumentStore final {
public:
  DocumentStore();
  ~DocumentStore();
  DocumentStore(const DocumentStore&) = delete;
  DocumentStore& operator=(const DocumentStore&) = delete;

  [[nodiscard]] std::optional<DocumentSnapshot> open(
      const std::filesystem::path& path, std::string& error);
  [[nodiscard]] bool adopt(DocumentSnapshot snapshot, std::string& error);
  [[nodiscard]] bool edit(const std::filesystem::path& path,
                          std::size_t offset, std::size_t erase_count,
                          std::string replacement,
                          std::uint64_t expected_version,
                          std::string& error);
  [[nodiscard]] bool undo(const std::filesystem::path& path,
                          std::uint64_t expected_version,
                          std::string& error);
  [[nodiscard]] bool redo(const std::filesystem::path& path,
                          std::uint64_t expected_version,
                          std::string& error);
  [[nodiscard]] bool save(const std::filesystem::path& path,
                          std::uint64_t expected_version,
                          std::string& error);
  [[nodiscard]] bool reload(const std::filesystem::path& path,
                            bool discard_local_changes,
                            std::string& error);
  [[nodiscard]] std::optional<DocumentSnapshot> snapshot(
      const std::filesystem::path& path) const;
  void observe_external_change(const std::filesystem::path& path);

  [[nodiscard]] static std::uint64_t content_hash(
      std::string_view value) noexcept;

private:
  struct Document;
  std::unordered_map<std::string, std::unique_ptr<Document>> documents_;
  [[nodiscard]] static std::string key(const std::filesystem::path& path);
};

} // namespace tokmon::desk
