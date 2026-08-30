#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tokmon::desk {

enum class SyntaxKind {
  plain,
  comment,
  keyword,
  type,
  string,
  number,
  preprocessor,
};

enum class SyntaxLanguage {
  plain,
  cpp,
  rust,
  javascript,
  typescript,
  tsx,
  python,
  json,
  yaml,
  toml,
  markdown,
  shell,
  cmake,
};

[[nodiscard]] SyntaxLanguage syntax_language_for_path(std::string_view path);
[[nodiscard]] std::string_view syntax_language_name(SyntaxLanguage language);

struct SyntaxSpan {
  std::size_t byte_start{0};
  std::size_t byte_end{0};
  SyntaxKind kind{SyntaxKind::plain};
};

// Tree-sitter is fully contained by this adapter. Reparse uses a computed
// TSInputEdit and the previous tree, while consumers only see stable byte
// ranges suitable for CodeSurface's visible-line renderer.
class SyntaxService final {
public:
  SyntaxService();
  ~SyntaxService();
  SyntaxService(const SyntaxService&) = delete;
  SyntaxService& operator=(const SyntaxService&) = delete;

  [[nodiscard]] bool update_cpp(std::string text, std::string& error);
  [[nodiscard]] bool update(SyntaxLanguage language, std::string text,
                            std::string& error);
  [[nodiscard]] std::vector<SyntaxSpan> spans(
      std::size_t byte_start = 0,
      std::size_t byte_end = static_cast<std::size_t>(-1)) const;
  [[nodiscard]] std::string_view text() const noexcept;
  [[nodiscard]] SyntaxLanguage language() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tokmon::desk
