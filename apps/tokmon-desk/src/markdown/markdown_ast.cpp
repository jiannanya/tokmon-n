#include "markdown/markdown_ast.hpp"

#include <chmd/chmd.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tokmon::desk {
namespace {

constexpr MarkdownNodeId kFnvOffset = 1469598103934665603ull;
constexpr MarkdownNodeId kFnvPrime = 1099511628211ull;

void hash_bytes(MarkdownNodeId& hash, const void* bytes, const std::size_t size) {
  const auto* value = static_cast<const unsigned char*>(bytes);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= value[index];
    hash *= kFnvPrime;
  }
}

MarkdownNodeId stable_id(const MarkdownNode& node,
                         const MarkdownNodeId parent_id) {
  auto hash = kFnvOffset;
  hash_bytes(hash, &parent_id, sizeof(parent_id));
  hash_bytes(hash, &node.kind, sizeof(node.kind));
  hash_bytes(hash, &node.source.byte_start, sizeof(node.source.byte_start));
  hash_bytes(hash, node.metadata.data(), node.metadata.size());
  hash_bytes(hash, node.text.data(), node.text.size());
  return hash == 0 ? 1 : hash;
}

MarkdownSourceRange source_range(const chmd::Node& node,
                                 const std::size_t source_offset) {
  return {source_offset + node.source.begin, source_offset + node.source.end};
}

MarkdownNodeKind code_block_kind(const std::string_view info) {
  if (info == "diff" || info == "patch")
    return MarkdownNodeKind::diff_block;
  if (info == "tool-call" || info == "tool_call")
    return MarkdownNodeKind::tool_call;
  if (info == "tool-result" || info == "tool_result")
    return MarkdownNodeKind::tool_result;
  return MarkdownNodeKind::code_block;
}

std::size_t append_chmd_node(const chmd::Document& source,
                             const chmd::NodeId source_id,
                             MarkdownDocument& destination,
                             const std::size_t parent,
                             const std::size_t source_offset) {
  const auto& input = source.node(source_id);
  MarkdownNode output;
  output.parent = parent;
  output.source = source_range(input, source_offset);

  switch (input.type) {
    case chmd::NodeType::document:
      output.kind = MarkdownNodeKind::document;
      output.parent = no_markdown_parent;
      break;
    case chmd::NodeType::block_quote:
      output.kind = MarkdownNodeKind::block_quote;
      break;
    case chmd::NodeType::list:
      output.kind = MarkdownNodeKind::list;
      output.metadata = input.list_kind == chmd::ListKind::ordered
                            ? "ordered"
                            : "unordered";
      break;
    case chmd::NodeType::item:
      output.kind = input.task ? MarkdownNodeKind::task_item
                               : MarkdownNodeKind::list_item;
      output.checked = input.checked;
      break;
    case chmd::NodeType::thematic_break:
      output.kind = MarkdownNodeKind::rule;
      break;
    case chmd::NodeType::heading:
      output.kind = MarkdownNodeKind::heading;
      output.level = input.number;
      break;
    case chmd::NodeType::code_block:
      output.metadata = input.title;
      output.kind = code_block_kind(output.metadata);
      break;
    case chmd::NodeType::html_block:
      // Keep the prior Tokmon AST contract: block HTML is represented as a
      // paragraph containing escaped raw HTML rather than executable markup.
      output.kind = MarkdownNodeKind::paragraph;
      break;
    case chmd::NodeType::paragraph:
      output.kind = MarkdownNodeKind::paragraph;
      break;
    case chmd::NodeType::table:
      output.kind = MarkdownNodeKind::table;
      break;
    case chmd::NodeType::table_head:
      output.kind = MarkdownNodeKind::table_head;
      break;
    case chmd::NodeType::table_body:
      output.kind = MarkdownNodeKind::table_body;
      break;
    case chmd::NodeType::table_row:
      output.kind = MarkdownNodeKind::table_row;
      break;
    case chmd::NodeType::table_cell:
      output.kind = MarkdownNodeKind::table_cell;
      break;
    case chmd::NodeType::text:
      output.kind = MarkdownNodeKind::text;
      output.text = input.literal;
      break;
    case chmd::NodeType::soft_break:
      output.kind = MarkdownNodeKind::soft_break;
      break;
    case chmd::NodeType::line_break:
      output.kind = MarkdownNodeKind::hard_break;
      break;
    case chmd::NodeType::code:
      output.kind = MarkdownNodeKind::code;
      break;
    case chmd::NodeType::html_inline:
      output.kind = MarkdownNodeKind::raw_html;
      output.text = input.literal;
      break;
    case chmd::NodeType::emphasis:
      output.kind = MarkdownNodeKind::emphasis;
      break;
    case chmd::NodeType::strong:
      output.kind = MarkdownNodeKind::strong;
      break;
    case chmd::NodeType::strikethrough:
      output.kind = MarkdownNodeKind::strike;
      break;
    case chmd::NodeType::link:
      output.kind = MarkdownNodeKind::link;
      output.metadata = input.literal;
      output.title = input.title;
      if (output.metadata.starts_with("file://") ||
          output.metadata.starts_with("tokmon-file:"))
        output.kind = MarkdownNodeKind::file_reference;
      break;
    case chmd::NodeType::image:
      output.kind = MarkdownNodeKind::image;
      output.metadata = input.literal;
      output.title = input.title;
      break;
  }

  const auto index = destination.nodes.size();
  destination.nodes.push_back(std::move(output));
  if (parent != no_markdown_parent)
    destination.nodes[parent].children.push_back(index);

  if (input.type == chmd::NodeType::code_block ||
      input.type == chmd::NodeType::code ||
      input.type == chmd::NodeType::html_block) {
    MarkdownNode text;
    text.kind = input.type == chmd::NodeType::html_block
                    ? MarkdownNodeKind::raw_html
                    : MarkdownNodeKind::text;
    text.source = source_range(input, source_offset);
    text.parent = index;
    text.text = input.literal;
    const auto child = destination.nodes.size();
    destination.nodes.push_back(std::move(text));
    destination.nodes[index].children.push_back(child);
  } else {
    for (auto child = input.first_child; child != chmd::npos;
         child = source.node(child).next)
      (void)append_chmd_node(source, child, destination, index, source_offset);
  }
  return index;
}

std::string collect_text(const MarkdownDocument& document,
                         const std::size_t index) {
  const auto& node = document.nodes[index];
  std::string result = node.text;
  for (const auto child : node.children)
    result += collect_text(document, child);
  return result;
}

void finalize_node(MarkdownDocument& document, const std::size_t index,
                   const MarkdownNodeId parent_id) {
  auto& node = document.nodes[index];
  node.id = stable_id(node, parent_id);
  for (const auto child : node.children)
    finalize_node(document, child, node.id);
}

void normalize_top_level_ranges(MarkdownDocument& document,
                                const std::size_t source_end) {
  auto& root = document.nodes[document.root];
  for (std::size_t child = 0; child < root.children.size(); ++child) {
    auto& node = document.nodes[root.children[child]];
    const auto next = child + 1 < root.children.size()
        ? document.nodes[root.children[child + 1]].source.byte_start
        : source_end;
    node.source.byte_end = std::max(node.source.byte_end, next);
  }
  for (std::size_t index = 0; index < document.nodes.size(); ++index) {
    auto& node = document.nodes[index];
    if (node.kind != MarkdownNodeKind::block_quote)
      continue;
    const auto content = collect_text(document, index);
    if (!content.starts_with("[!"))
      continue;
    const auto close = content.find(']');
    if (close == std::string::npos || close > 32)
      continue;
    node.kind = MarkdownNodeKind::callout;
    node.metadata = content.substr(2, close - 2);
  }
}

std::string escape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string lower_scheme(std::string_view value) {
  while (!value.empty() && std::isspace(
             static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  const auto colon = value.find(':');
  if (colon == std::string_view::npos)
    return {};
  std::string scheme(value.substr(0, colon));
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return scheme;
}

struct RmlWriter {
  std::string output;
  std::string copy_prefix;
  std::vector<MarkdownCopyBlock>* copy_blocks{nullptr};
  std::size_t text_bytes{0};
  std::size_t nodes{0};
  bool truncated{false};

  void markup(const std::string_view value) { output.append(value); }
  void escaped_text(const std::string_view value) {
    for (const char ch : value) {
      const std::string_view encoded = ch == '&' ? "&amp;" :
          ch == '<' ? "&lt;" : ch == '>' ? "&gt;" :
          ch == '"' ? "&quot;" : ch == '\'' ? "&#39;" :
          std::string_view(&ch, 1);
      if (text_bytes + encoded.size() > markdown_rml_text_limit) {
        truncated = true;
        break;
      }
      output.append(encoded);
      text_bytes += encoded.size();
    }
  }
};

void append_plain_text(const MarkdownDocument& document,
                       const std::size_t index, std::string& output) {
  if (index >= document.nodes.size())
    return;
  const auto& node = document.nodes[index];
  if (node.kind == MarkdownNodeKind::text ||
      node.kind == MarkdownNodeKind::raw_html) {
    output += node.text;
    return;
  }
  if (node.kind == MarkdownNodeKind::soft_break ||
      node.kind == MarkdownNodeKind::hard_break) {
    output.push_back('\n');
    return;
  }
  for (const auto child : node.children)
    append_plain_text(document, child, output);
}

void render_node(const MarkdownDocument& document, const std::size_t index,
                 RmlWriter& out) {
  if (out.truncated || index >= document.nodes.size() ||
      ++out.nodes > markdown_rml_node_limit) {
    out.truncated = true;
    return;
  }
  const auto& node = document.nodes[index];
  const char* open = "";
  const char* close = "";
  switch (node.kind) {
    case MarkdownNodeKind::document: break;
    case MarkdownNodeKind::paragraph: open = "<p>"; close = "</p>"; break;
    case MarkdownNodeKind::heading:
      out.markup("<h" + std::to_string(std::clamp(node.level, 1u, 6u)) + ">");
      close = nullptr;
      break;
    case MarkdownNodeKind::block_quote:
      open = "<blockquote>"; close = "</blockquote>"; break;
    case MarkdownNodeKind::callout:
      out.markup("<blockquote class=\"callout\" data-kind=\"" +
                 escape(node.metadata) + "\">");
      close = "</blockquote>";
      break;
    case MarkdownNodeKind::list:
      open = node.metadata == "ordered" ? "<ol>" : "<ul>";
      close = node.metadata == "ordered" ? "</ol>" : "</ul>";
      break;
    case MarkdownNodeKind::list_item: open = "<li>"; close = "</li>"; break;
    case MarkdownNodeKind::task_item:
      out.markup(std::string("<li class=\"task-item\"><span class=\"task-check\">") +
                 (node.checked ? "✓" : "○") + "</span>");
      close = "</li>";
      break;
    case MarkdownNodeKind::code_block:
    case MarkdownNodeKind::diff_block:
    case MarkdownNodeKind::tool_call:
    case MarkdownNodeKind::tool_result:
      if (out.copy_blocks) {
        MarkdownCopyBlock block;
        block.id = out.copy_prefix + "-code-" +
            std::to_string(out.copy_blocks->size());
        append_plain_text(document, index, block.text);
        out.copy_blocks->push_back(block);
        out.markup("<div class=\"code-block-wrap\"><button class=\"markdown-copy code-copy\" data-copy-markdown=\"" +
                   escape(block.id) + "\">复制代码</button>");
      }
      out.markup(std::string("<pre class=\"code-block") +
                 (node.kind == MarkdownNodeKind::diff_block ? " diff-block" : "") +
                 (node.kind == MarkdownNodeKind::tool_call ? " tool-call" : "") +
                 (node.kind == MarkdownNodeKind::tool_result ? " tool-result" : "") +
                 "\" data-language=\"" + escape(node.metadata) + "\">");
      // RmlUi's HTML element factory gives nested <code> an independent inline
      // text box.  Inside a scrollable <pre> this can retain the default dark
      // foreground and disappear against the dark code-block background.  The
      // preformatted element already provides every semantic/layout guarantee
      // we need, so keep source text directly in <pre> and put the language on
      // that element.
      close = out.copy_blocks ? "</pre></div>" : "</pre>";
      break;
    case MarkdownNodeKind::table: open = "<table>"; close = "</table>"; break;
    case MarkdownNodeKind::table_head: open = "<thead>"; close = "</thead>"; break;
    case MarkdownNodeKind::table_body: open = "<tbody>"; close = "</tbody>"; break;
    case MarkdownNodeKind::table_row: open = "<tr>"; close = "</tr>"; break;
    case MarkdownNodeKind::table_cell: open = "<td>"; close = "</td>"; break;
    case MarkdownNodeKind::emphasis: open = "<em>"; close = "</em>"; break;
    case MarkdownNodeKind::strong: open = "<strong>"; close = "</strong>"; break;
    case MarkdownNodeKind::strike: open = "<del>"; close = "</del>"; break;
    case MarkdownNodeKind::link:
      out.markup("<span class=\"safe-link\"");
      if (markdown_safe_external_url(node.metadata))
        out.markup(" data-href=\"" + escape(node.metadata) + "\"");
      out.markup(">");
      close = "</span>";
      break;
    case MarkdownNodeKind::file_reference:
      out.markup("<span class=\"file-reference\" data-file=\"" +
                 escape(node.metadata) + "\">");
      close = "</span>";
      break;
    case MarkdownNodeKind::image:
      out.markup("<span class=\"markdown-image-placeholder\">[图片: ");
      close = "]</span>";
      break;
    case MarkdownNodeKind::code: open = "<code>"; close = "</code>"; break;
    case MarkdownNodeKind::text:
    case MarkdownNodeKind::raw_html: out.escaped_text(node.text); return;
    case MarkdownNodeKind::soft_break: out.markup("\n"); return;
    case MarkdownNodeKind::hard_break: out.markup("<br/>"); return;
    case MarkdownNodeKind::rule: out.markup("<hr/>"); return;
  }
  out.markup(open);
  for (const auto child : node.children) {
    render_node(document, child, out);
    if (out.truncated)
      break;
  }
  if (node.kind == MarkdownNodeKind::heading)
    out.markup("</h" + std::to_string(std::clamp(node.level, 1u, 6u)) + ">");
  else if (close)
    out.markup(close);
}

std::size_t safe_restart(const std::string_view source) {
  std::size_t restart = 0;
  std::size_t fence_start = 0;
  char fence_character = 0;
  std::size_t fence_size = 0;
  std::size_t line_start = 0;
  while (line_start < source.size()) {
    auto line_end = source.find('\n', line_start);
    if (line_end == std::string_view::npos)
      line_end = source.size();
    const auto line = source.substr(line_start, line_end - line_start);
    std::size_t indent = 0;
    while (indent < line.size() && indent < 4 && line[indent] == ' ')
      ++indent;
    if (indent < line.size() && (line[indent] == '`' || line[indent] == '~')) {
      const char marker = line[indent];
      std::size_t count = 0;
      while (indent + count < line.size() && line[indent + count] == marker)
        ++count;
      if (count >= 3) {
        if (!fence_character) {
          fence_character = marker;
          fence_size = count;
          fence_start = line_start;
        } else if (fence_character == marker && count >= fence_size) {
          fence_character = 0;
          fence_size = 0;
          restart = line_end < source.size() ? line_end + 1 : line_end;
        }
      }
    }
    if (!fence_character && line.empty())
      restart = line_end < source.size() ? line_end + 1 : line_end;
    line_start = line_end < source.size() ? line_end + 1 : source.size();
  }
  return fence_character ? fence_start : restart;
}

std::size_t copy_subtree(const MarkdownDocument& source,
                         const std::size_t source_index,
                         MarkdownDocument& destination,
                         const std::size_t parent) {
  auto node = source.nodes[source_index];
  node.parent = parent;
  node.children.clear();
  const auto index = destination.nodes.size();
  destination.nodes.push_back(std::move(node));
  for (const auto child : source.nodes[source_index].children) {
    // Recursion can reallocate destination.nodes. Compute the child first and
    // reacquire the parent by index before mutating its child vector.
    const auto copied = copy_subtree(source, child, destination, index);
    destination.nodes[index].children.push_back(copied);
  }
  return index;
}

} // namespace

MarkdownDocument MarkdownParser::parse(const std::string_view markdown,
                                       const std::size_t source_offset) const {
  MarkdownDocument document;
  document.source_bytes = source_offset + markdown.size();
  auto parsed = chmd::Parser().parse(markdown);
  if (parsed) {
    document.nodes.reserve(parsed.document.size());
    document.root = append_chmd_node(parsed.document, parsed.document.root(),
                                     document, no_markdown_parent,
                                     source_offset);
  } else {
    document.nodes.push_back({.kind = MarkdownNodeKind::document,
                              .source = {source_offset,
                                         source_offset + markdown.size()},
                              .parent = no_markdown_parent});
    document.root = 0;
  }
  document.nodes[document.root].source = {
      source_offset, source_offset + markdown.size()};
  normalize_top_level_ranges(document, source_offset + markdown.size());
  finalize_node(document, document.root, 0);
  return document;
}

MarkdownStream::MarkdownStream(MarkdownParser parser)
    : parser_(std::move(parser)) {
  reset();
}

void MarkdownStream::reset(std::string markdown) {
  source_ = std::move(markdown);
  reparsed_from_ = 0;
  document_ = parser_.parse(source_);
  ++generation_;
}

void MarkdownStream::append(const std::string_view chunk) {
  if (chunk.empty())
    return;
  const auto restart = safe_restart(source_);
  source_.append(chunk);
  rebuild(restart);
}

void MarkdownStream::rebuild(const std::size_t restart) {
  reparsed_from_ = std::min(restart, source_.size());
  auto tail = parser_.parse(std::string_view(source_).substr(reparsed_from_),
                            reparsed_from_);
  MarkdownDocument merged;
  merged.source_bytes = source_.size();
  merged.nodes.push_back({.kind = MarkdownNodeKind::document,
                          .source = {0, source_.size()}});
  merged.root = 0;
  if (!document_.nodes.empty()) {
    for (const auto child : document_.nodes[document_.root].children) {
      if (document_.nodes[child].source.byte_end > reparsed_from_)
        break;
      const auto copied = copy_subtree(document_, child, merged, 0);
      merged.nodes[0].children.push_back(copied);
    }
  }
  for (const auto child : tail.nodes[tail.root].children) {
    const auto copied = copy_subtree(tail, child, merged, 0);
    merged.nodes[0].children.push_back(copied);
  }
  finalize_node(merged, merged.root, 0);
  document_ = std::move(merged);
  ++generation_;
}

const MarkdownDocument& MarkdownStream::document() const noexcept {
  return document_;
}

std::string_view MarkdownStream::source() const noexcept { return source_; }
std::size_t MarkdownStream::reparsed_from() const noexcept { return reparsed_from_; }
std::uint64_t MarkdownStream::generation() const noexcept { return generation_; }

bool markdown_safe_external_url(const std::string_view value) {
  const auto scheme = lower_scheme(value);
  return scheme.empty() || scheme == "http" || scheme == "https" ||
         scheme == "mailto";
}

std::string markdown_to_safe_rml(const MarkdownDocument& document) {
  if (document.nodes.empty() || document.root >= document.nodes.size())
    return {};
  RmlWriter out;
  out.output.reserve(std::min(document.source_bytes,
                              markdown_rml_text_limit) + 4096);
  render_node(document, document.root, out);
  if (out.truncated)
    out.markup("<p class=\"markdown-truncated\">内容过长，已限制渲染范围</p>");
  return std::move(out.output);
}

MarkdownRmlResult markdown_to_safe_rml_with_copy(
    const MarkdownDocument& document, const std::string_view id_prefix) {
  MarkdownRmlResult result;
  if (document.nodes.empty() || document.root >= document.nodes.size())
    return result;
  RmlWriter out;
  out.output.reserve(std::min(document.source_bytes,
                              markdown_rml_text_limit) + 4096);
  out.copy_prefix = std::string(id_prefix);
  out.copy_blocks = &result.code_blocks;
  render_node(document, document.root, out);
  if (out.truncated)
    out.markup("<p class=\"markdown-truncated\">内容过长，已限制渲染范围</p>");
  result.rml = std::move(out.output);
  return result;
}

} // namespace tokmon::desk
