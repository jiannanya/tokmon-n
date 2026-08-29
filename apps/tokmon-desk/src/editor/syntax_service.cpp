#include "editor/syntax_service.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace tokmon::desk {
namespace {

TSPoint point_at(std::string_view text, std::size_t offset) {
  TSPoint point{};
  offset = std::min(offset, text.size());
  for (std::size_t index = 0; index < offset; ++index) {
    if (text[index] == '\n') {
      ++point.row;
      point.column = 0;
    } else {
      ++point.column;
    }
  }
  return point;
}

SyntaxKind classify(std::string_view type, bool named) {
  if (type.find("comment") != std::string_view::npos)
    return SyntaxKind::comment;
  if (type.find("string") != std::string_view::npos ||
      type == "char_literal")
    return SyntaxKind::string;
  if (type.find("number") != std::string_view::npos)
    return SyntaxKind::number;
  if (type.starts_with("preproc"))
    return SyntaxKind::preprocessor;
  if (type == "primitive_type" || type == "type_identifier" ||
      type == "sized_type_specifier")
    return SyntaxKind::type;
  static const std::unordered_set<std::string_view> keywords = {
      "alignas", "alignof", "auto", "bool", "break", "case", "catch",
      "class", "concept", "const", "consteval", "constexpr", "constinit",
      "continue", "co_await", "co_return", "co_yield", "decltype",
      "default", "delete", "do", "else", "enum", "explicit", "export",
      "extern", "false", "for", "friend", "goto", "if", "inline",
      "namespace", "new", "noexcept", "nullptr", "operator", "private",
      "protected", "public", "requires", "return", "sizeof", "static",
      "static_assert", "struct", "switch", "template", "this", "thread_local",
      "throw", "true", "try", "typedef", "typename", "union", "using",
      "virtual", "volatile", "while"};
  if (!named && keywords.contains(type))
    return SyntaxKind::keyword;
  return SyntaxKind::plain;
}

void collect(TSNode node, std::vector<SyntaxSpan>& result,
             std::size_t begin, std::size_t end) {
  const auto node_begin = static_cast<std::size_t>(ts_node_start_byte(node));
  const auto node_end = static_cast<std::size_t>(ts_node_end_byte(node));
  if (node_end <= begin || node_begin >= end)
    return;
  const std::string_view type(ts_node_type(node));
  const auto kind = classify(type, ts_node_is_named(node));
  if (kind != SyntaxKind::plain) {
    result.push_back({node_begin, node_end, kind});
    return;
  }
  const auto child_count = ts_node_child_count(node);
  for (std::uint32_t index = 0; index < child_count; ++index)
    collect(ts_node_child(node, index), result, begin, end);
}

} // namespace

struct SyntaxService::Impl {
  TSParser* parser{nullptr};
  TSTree* tree{nullptr};
  std::string text;

  ~Impl() {
    if (tree)
      ts_tree_delete(tree);
    if (parser)
      ts_parser_delete(parser);
  }
};

SyntaxService::SyntaxService() : impl_(std::make_unique<Impl>()) {
  impl_->parser = ts_parser_new();
  if (impl_->parser)
    (void)ts_parser_set_language(impl_->parser, tree_sitter_cpp());
}

SyntaxService::~SyntaxService() = default;

bool SyntaxService::update_cpp(std::string text, std::string& error) {
  if (!impl_->parser) {
    error = "tree-sitter parser is unavailable";
    return false;
  }
  if (impl_->tree) {
    std::size_t prefix = 0;
    while (prefix < impl_->text.size() && prefix < text.size() &&
           impl_->text[prefix] == text[prefix])
      ++prefix;
    std::size_t old_suffix = impl_->text.size();
    std::size_t new_suffix = text.size();
    while (old_suffix > prefix && new_suffix > prefix &&
           impl_->text[old_suffix - 1] == text[new_suffix - 1]) {
      --old_suffix;
      --new_suffix;
    }
    TSInputEdit edit{};
    edit.start_byte = static_cast<std::uint32_t>(prefix);
    edit.old_end_byte = static_cast<std::uint32_t>(old_suffix);
    edit.new_end_byte = static_cast<std::uint32_t>(new_suffix);
    edit.start_point = point_at(impl_->text, prefix);
    edit.old_end_point = point_at(impl_->text, old_suffix);
    edit.new_end_point = point_at(text, new_suffix);
    ts_tree_edit(impl_->tree, &edit);
  }
  auto* updated = ts_parser_parse_string(
      impl_->parser, impl_->tree, text.data(),
      static_cast<std::uint32_t>(text.size()));
  if (!updated) {
    error = "tree-sitter could not parse the document";
    return false;
  }
  if (impl_->tree)
    ts_tree_delete(impl_->tree);
  impl_->tree = updated;
  impl_->text = std::move(text);
  return true;
}

std::vector<SyntaxSpan> SyntaxService::spans(
    const std::size_t byte_start, const std::size_t byte_end) const {
  std::vector<SyntaxSpan> result;
  if (!impl_->tree)
    return result;
  const auto end = std::min(byte_end, impl_->text.size());
  collect(ts_tree_root_node(impl_->tree), result,
          std::min(byte_start, end), end);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.byte_start < right.byte_start;
  });
  return result;
}

std::string_view SyntaxService::text() const noexcept { return impl_->text; }

} // namespace tokmon::desk
