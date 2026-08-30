#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace tokmon::desk {

using MarkdownNodeId = std::uint64_t;
inline constexpr std::size_t no_markdown_parent =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t markdown_rml_node_limit = 4096;
inline constexpr std::size_t markdown_rml_text_limit = 1024u * 1024u;

enum class MarkdownNodeKind {
  document,
  paragraph,
  heading,
  block_quote,
  callout,
  list,
  list_item,
  task_item,
  code_block,
  diff_block,
  tool_call,
  tool_result,
  table,
  table_head,
  table_body,
  table_row,
  table_cell,
  emphasis,
  strong,
  strike,
  link,
  image,
  file_reference,
  code,
  text,
  raw_html,
  soft_break,
  hard_break,
  rule,
};

struct MarkdownSourceRange {
  std::size_t byte_start{0};
  std::size_t byte_end{0};
};

struct MarkdownNode {
  MarkdownNodeId id{0};
  MarkdownNodeKind kind{MarkdownNodeKind::text};
  MarkdownSourceRange source;
  std::size_t parent{no_markdown_parent};
  std::string text;
  std::string metadata;
  std::string title;
  unsigned level{0};
  bool checked{false};
  std::vector<std::size_t> children;
};

struct MarkdownDocument {
  std::vector<MarkdownNode> nodes;
  std::size_t root{0};
  std::size_t source_bytes{0};
};

struct MarkdownCopyBlock {
  std::string id;
  std::string text;
};

struct MarkdownRmlResult {
  std::string rml;
  std::vector<MarkdownCopyBlock> code_blocks;
};

class MarkdownParser final {
 public:
  [[nodiscard]] MarkdownDocument parse(
      std::string_view markdown, std::size_t source_offset = 0) const;
};

// Appends preserve completed top-level nodes before reparsed_from(); only the
// potentially open tail is tokenized again.
class MarkdownStream final {
 public:
  explicit MarkdownStream(MarkdownParser parser = {});

  void reset(std::string markdown = {});
  void append(std::string_view chunk);
  [[nodiscard]] const MarkdownDocument& document() const noexcept;
  [[nodiscard]] std::string_view source() const noexcept;
  [[nodiscard]] std::size_t reparsed_from() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;

 private:
  void rebuild(std::size_t restart);

  MarkdownParser parser_;
  std::string source_;
  MarkdownDocument document_;
  std::size_t reparsed_from_{0};
  std::uint64_t generation_{0};
};

[[nodiscard]] std::string markdown_to_safe_rml(const MarkdownDocument& document);
// Produces the same safe markup while adding unobtrusive copy buttons to
// fenced code/tool blocks. The button payload is an opaque id; source text is
// kept outside the DOM so large snippets do not get duplicated in attributes.
[[nodiscard]] MarkdownRmlResult markdown_to_safe_rml_with_copy(
    const MarkdownDocument& document, std::string_view id_prefix);
[[nodiscard]] bool markdown_safe_external_url(std::string_view value);

} // namespace tokmon::desk
