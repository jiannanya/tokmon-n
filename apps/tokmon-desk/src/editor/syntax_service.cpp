#include "editor/syntax_service.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <cctype>

extern "C" const TSLanguage* tree_sitter_cpp(void);
extern "C" const TSLanguage* tree_sitter_rust(void);
extern "C" const TSLanguage* tree_sitter_javascript(void);
extern "C" const TSLanguage* tree_sitter_typescript(void);
extern "C" const TSLanguage* tree_sitter_tsx(void);
extern "C" const TSLanguage* tree_sitter_python(void);
extern "C" const TSLanguage* tree_sitter_json(void);
extern "C" const TSLanguage* tree_sitter_yaml(void);
extern "C" const TSLanguage* tree_sitter_toml(void);
extern "C" const TSLanguage* tree_sitter_markdown(void);
extern "C" const TSLanguage* tree_sitter_bash(void);
extern "C" const TSLanguage* tree_sitter_cmake(void);

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
      type.find("quoted") != std::string_view::npos ||
      type == "char_literal" || type == "string_scalar")
    return SyntaxKind::string;
  if (type.find("number") != std::string_view::npos ||
      type.find("integer") != std::string_view::npos ||
      type.find("float") != std::string_view::npos)
    return SyntaxKind::number;
  if (type.starts_with("preproc"))
    return SyntaxKind::preprocessor;
  if (type.find("heading") != std::string_view::npos ||
      type.find("marker") != std::string_view::npos)
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
      "virtual", "volatile", "while", "as", "async", "await", "def",
      "elif", "except", "finally", "fn", "from", "function", "impl",
      "import", "in", "interface", "let", "match", "mod", "none",
      "package", "pub", "self", "super", "trait", "type", "undefined",
      "use", "var", "with", "yield"};
  if (!named && keywords.contains(type))
    return SyntaxKind::keyword;
  return SyntaxKind::plain;
}

const TSLanguage* grammar(const SyntaxLanguage language) {
  switch (language) {
    case SyntaxLanguage::cpp: return tree_sitter_cpp();
    case SyntaxLanguage::rust: return tree_sitter_rust();
    case SyntaxLanguage::javascript: return tree_sitter_javascript();
    case SyntaxLanguage::typescript: return tree_sitter_typescript();
    case SyntaxLanguage::tsx: return tree_sitter_tsx();
    case SyntaxLanguage::python: return tree_sitter_python();
    case SyntaxLanguage::json: return tree_sitter_json();
    case SyntaxLanguage::yaml: return tree_sitter_yaml();
    case SyntaxLanguage::toml: return tree_sitter_toml();
    case SyntaxLanguage::markdown: return tree_sitter_markdown();
    case SyntaxLanguage::shell: return tree_sitter_bash();
    case SyntaxLanguage::cmake: return tree_sitter_cmake();
    default: return nullptr;
  }
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

bool identifier_character(const char value) {
  return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

std::vector<SyntaxSpan> lexical_spans(const std::string_view text,
                                      const SyntaxLanguage language) {
  static const std::unordered_set<std::string_view> common_keywords = {
      "as", "async", "await", "break", "case", "catch", "class",
      "const", "continue", "def", "do", "else", "enum", "export",
      "false", "fn", "for", "from", "function", "if", "import", "in",
      "interface", "let", "match", "mod", "new", "none", "null",
      "pub", "return", "self", "static", "struct", "super", "switch",
      "this", "throw", "trait", "true", "try", "type", "undefined",
      "use", "var", "while", "with", "yield"};
  std::vector<SyntaxSpan> result;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto start = index;
    const char current = text[index];
    const bool hash_comment = current == '#' &&
        (language == SyntaxLanguage::python || language == SyntaxLanguage::shell ||
         language == SyntaxLanguage::yaml || language == SyntaxLanguage::toml);
    const bool slash_comment = current == '/' && index + 1 < text.size() &&
        text[index + 1] == '/' &&
        (language == SyntaxLanguage::rust || language == SyntaxLanguage::javascript ||
         language == SyntaxLanguage::typescript);
    if (hash_comment || slash_comment) {
      index = text.find('\n', index);
      if (index == std::string_view::npos)
        index = text.size();
      result.push_back({start, index, SyntaxKind::comment});
      continue;
    }
    if (current == '/' && index + 1 < text.size() && text[index + 1] == '*' &&
        (language == SyntaxLanguage::rust || language == SyntaxLanguage::javascript ||
         language == SyntaxLanguage::typescript)) {
      const auto close = text.find("*/", index + 2);
      index = close == std::string_view::npos ? text.size() : close + 2;
      result.push_back({start, index, SyntaxKind::comment});
      continue;
    }
    if (current == '"' || current == '\'' ||
        (current == '`' && (language == SyntaxLanguage::javascript ||
                            language == SyntaxLanguage::typescript))) {
      const char quote = current;
      ++index;
      while (index < text.size()) {
        if (text[index] == '\\' && index + 1 < text.size()) {
          index += 2;
          continue;
        }
        if (text[index++] == quote)
          break;
      }
      result.push_back({start, index, SyntaxKind::string});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(current))) {
      ++index;
      while (index < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[index])) ||
              text[index] == '.' || text[index] == '_'))
        ++index;
      result.push_back({start, index, SyntaxKind::number});
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
      ++index;
      while (index < text.size() && identifier_character(text[index]))
        ++index;
      const auto token = text.substr(start, index - start);
      if (common_keywords.contains(token))
        result.push_back({start, index, SyntaxKind::keyword});
      continue;
    }
    if (language == SyntaxLanguage::markdown &&
        (current == '#' || current == '*' || current == '_' || current == '`')) {
      ++index;
      while (index < text.size() && text[index] == current)
        ++index;
      result.push_back({start, index, SyntaxKind::preprocessor});
      continue;
    }
    ++index;
  }
  return result;
}

} // namespace

struct SyntaxService::Impl {
  TSParser* parser{nullptr};
  TSTree* tree{nullptr};
  std::string text;
  SyntaxLanguage language{SyntaxLanguage::plain};

  ~Impl() {
    if (tree)
      ts_tree_delete(tree);
    if (parser)
      ts_parser_delete(parser);
  }
};

SyntaxService::SyntaxService() : impl_(std::make_unique<Impl>()) {
  impl_->parser = ts_parser_new();
}

SyntaxService::~SyntaxService() = default;

bool SyntaxService::update_cpp(std::string text, std::string& error) {
  return update(SyntaxLanguage::cpp, std::move(text), error);
}

bool SyntaxService::update(const SyntaxLanguage language, std::string text,
                           std::string& error) {
  error.clear();
  const auto previous_language = impl_->language;
  const auto* selected_grammar = grammar(language);
  if (!selected_grammar) {
    if (impl_->tree) {
      ts_tree_delete(impl_->tree);
      impl_->tree = nullptr;
    }
    impl_->language = language;
    impl_->text = std::move(text);
    return true;
  }
  if (!impl_->parser) {
    error = "tree-sitter parser is unavailable";
    return false;
  }
  if (previous_language != language) {
    if (impl_->tree) {
      ts_tree_delete(impl_->tree);
      impl_->tree = nullptr;
    }
    impl_->text.clear();
  }
  if (!ts_parser_set_language(impl_->parser, selected_grammar)) {
    error = "tree-sitter grammar ABI is incompatible";
    return false;
  }
  impl_->language = language;
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

SyntaxLanguage SyntaxService::language() const noexcept { return impl_->language; }

SyntaxLanguage syntax_language_for_path(const std::string_view path) {
  auto extension = path.substr(path.find_last_of('.') == std::string_view::npos
      ? path.size() : path.find_last_of('.'));
  std::string lowered(extension);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](const unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if (lowered == ".c" || lowered == ".cc" || lowered == ".cpp" ||
      lowered == ".cxx" || lowered == ".h" || lowered == ".hh" ||
      lowered == ".hpp") return SyntaxLanguage::cpp;
  if (lowered == ".rs") return SyntaxLanguage::rust;
  if (lowered == ".js" || lowered == ".mjs" || lowered == ".jsx")
    return SyntaxLanguage::javascript;
  if (lowered == ".ts") return SyntaxLanguage::typescript;
  if (lowered == ".tsx") return SyntaxLanguage::tsx;
  if (lowered == ".py") return SyntaxLanguage::python;
  if (lowered == ".json" || lowered == ".jsonc") return SyntaxLanguage::json;
  if (lowered == ".yaml" || lowered == ".yml") return SyntaxLanguage::yaml;
  if (lowered == ".toml") return SyntaxLanguage::toml;
  if (lowered == ".md" || lowered == ".markdown") return SyntaxLanguage::markdown;
  if (lowered == ".sh" || lowered == ".bash" || lowered == ".zsh" ||
      lowered == ".fish" || lowered == ".ps1") return SyntaxLanguage::shell;
  const auto slash = path.find_last_of("/\\");
  const auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
  if (name == "CMakeLists.txt" || lowered == ".cmake") return SyntaxLanguage::cmake;
  return SyntaxLanguage::plain;
}

std::string_view syntax_language_name(const SyntaxLanguage language) {
  switch (language) {
    case SyntaxLanguage::cpp: return "C/C++";
    case SyntaxLanguage::rust: return "Rust";
    case SyntaxLanguage::javascript: return "JavaScript";
    case SyntaxLanguage::typescript: return "TypeScript";
    case SyntaxLanguage::tsx: return "TSX";
    case SyntaxLanguage::python: return "Python";
    case SyntaxLanguage::json: return "JSON";
    case SyntaxLanguage::yaml: return "YAML";
    case SyntaxLanguage::toml: return "TOML";
    case SyntaxLanguage::markdown: return "Markdown";
    case SyntaxLanguage::shell: return "Shell";
    case SyntaxLanguage::cmake: return "CMake";
    default: return "纯文本";
  }
}

} // namespace tokmon::desk
