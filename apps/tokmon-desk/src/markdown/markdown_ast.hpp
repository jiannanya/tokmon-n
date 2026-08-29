#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tokmon::desk {

enum class MarkdownNodeKind {
  document,
  paragraph,
  heading,
  block_quote,
  list,
  list_item,
  code_block,
  table,
  table_head,
  table_body,
  table_row,
  table_cell,
  emphasis,
  strong,
  strike,
  link,
  code,
  text,
  soft_break,
  hard_break,
  rule,
};

struct MarkdownNode {
  MarkdownNodeKind kind{MarkdownNodeKind::text};
  std::string text;
  std::string metadata;
  unsigned level{0};
  std::vector<std::size_t> children;
};

struct MarkdownDocument {
  std::vector<MarkdownNode> nodes;
  std::size_t root{0};
};

class MarkdownParser final {
 public:
  [[nodiscard]] MarkdownDocument parse(std::string_view markdown) const;
};

[[nodiscard]] std::string markdown_to_safe_rml(const MarkdownDocument& document);

} // namespace tokmon::desk
