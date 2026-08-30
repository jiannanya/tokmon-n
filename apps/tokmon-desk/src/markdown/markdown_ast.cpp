#include "markdown/markdown_ast.hpp"

#include <md4c.h>

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

std::string attribute(const MD_ATTRIBUTE& value) {
  return std::string(value.text ? value.text : "", value.size);
}

struct Builder {
  explicit Builder(const std::string_view input, const std::size_t offset)
      : begin(input.data()), end(input.data() + input.size()), base(offset),
        stack{0} {
    document.nodes.push_back({.kind = MarkdownNodeKind::document,
                              .source = {offset, offset + input.size()}});
    document.root = 0;
    document.source_bytes = offset + input.size();
  }

  const char* begin;
  const char* end;
  std::size_t base{0};
  std::size_t cursor{0};
  MarkdownDocument document;
  std::vector<std::size_t> stack;

  std::size_t absolute_cursor() const { return base + cursor; }

  std::size_t source_position(const char* pointer, const std::size_t size) {
    if (pointer >= begin && pointer <= end &&
        static_cast<std::size_t>(end - pointer) >= size) {
      cursor = static_cast<std::size_t>(pointer - begin);
      return base + cursor;
    }
    return absolute_cursor();
  }

  std::size_t push(MarkdownNode node) {
    node.parent = stack.back();
    node.source.byte_start = absolute_cursor();
    node.source.byte_end = node.source.byte_start;
    const auto index = document.nodes.size();
    document.nodes.push_back(std::move(node));
    document.nodes[stack.back()].children.push_back(index);
    stack.push_back(index);
    return index;
  }

  void pop() {
    if (stack.size() <= 1)
      return;
    document.nodes[stack.back()].source.byte_end = absolute_cursor();
    stack.pop_back();
  }

  void leaf(MarkdownNode node) {
    node.parent = stack.back();
    if (node.source.byte_end < node.source.byte_start)
      node.source.byte_end = node.source.byte_start;
    const auto index = document.nodes.size();
    document.nodes.push_back(std::move(node));
    document.nodes[stack.back()].children.push_back(index);
  }
};

int enter_block(const MD_BLOCKTYPE type, void* detail, void* userdata) {
  auto& builder = *static_cast<Builder*>(userdata);
  MarkdownNode node;
  switch (type) {
    case MD_BLOCK_DOC: return 0;
    case MD_BLOCK_QUOTE: node.kind = MarkdownNodeKind::block_quote; break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      node.kind = MarkdownNodeKind::list;
      node.metadata = type == MD_BLOCK_OL ? "ordered" : "unordered";
      break;
    case MD_BLOCK_LI: {
      node.kind = MarkdownNodeKind::list_item;
      if (detail) {
        const auto* item = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (item->is_task) {
          node.kind = MarkdownNodeKind::task_item;
          node.checked = item->task_mark == 'x' || item->task_mark == 'X';
        }
      }
      break;
    }
    case MD_BLOCK_HR:
      node.kind = MarkdownNodeKind::rule;
      node.source = {builder.absolute_cursor(), builder.absolute_cursor()};
      builder.leaf(std::move(node));
      return 0;
    case MD_BLOCK_H:
      node.kind = MarkdownNodeKind::heading;
      node.level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
      break;
    case MD_BLOCK_CODE:
      node.kind = MarkdownNodeKind::code_block;
      if (detail)
        node.metadata = attribute(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->info);
      if (node.metadata == "diff" || node.metadata == "patch")
        node.kind = MarkdownNodeKind::diff_block;
      else if (node.metadata == "tool-call" || node.metadata == "tool_call")
        node.kind = MarkdownNodeKind::tool_call;
      else if (node.metadata == "tool-result" || node.metadata == "tool_result")
        node.kind = MarkdownNodeKind::tool_result;
      break;
    case MD_BLOCK_TABLE: node.kind = MarkdownNodeKind::table; break;
    case MD_BLOCK_THEAD: node.kind = MarkdownNodeKind::table_head; break;
    case MD_BLOCK_TBODY: node.kind = MarkdownNodeKind::table_body; break;
    case MD_BLOCK_TR: node.kind = MarkdownNodeKind::table_row; break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: node.kind = MarkdownNodeKind::table_cell; break;
    default: node.kind = MarkdownNodeKind::paragraph; break;
  }
  builder.push(std::move(node));
  return 0;
}

int leave_block(const MD_BLOCKTYPE type, void*, void* userdata) {
  if (type != MD_BLOCK_DOC && type != MD_BLOCK_HR)
    static_cast<Builder*>(userdata)->pop();
  return 0;
}

int enter_span(const MD_SPANTYPE type, void* detail, void* userdata) {
  auto& builder = *static_cast<Builder*>(userdata);
  MarkdownNode node;
  switch (type) {
    case MD_SPAN_EM: node.kind = MarkdownNodeKind::emphasis; break;
    case MD_SPAN_STRONG: node.kind = MarkdownNodeKind::strong; break;
    case MD_SPAN_DEL: node.kind = MarkdownNodeKind::strike; break;
    case MD_SPAN_CODE: node.kind = MarkdownNodeKind::code; break;
    case MD_SPAN_A:
      node.kind = MarkdownNodeKind::link;
      if (detail) {
        const auto* link = static_cast<MD_SPAN_A_DETAIL*>(detail);
        node.metadata = attribute(link->href);
        node.title = attribute(link->title);
      }
      if (node.metadata.starts_with("file://") ||
          node.metadata.starts_with("tokmon-file:"))
        node.kind = MarkdownNodeKind::file_reference;
      break;
    case MD_SPAN_IMG:
      node.kind = MarkdownNodeKind::image;
      if (detail) {
        const auto* image = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        node.metadata = attribute(image->src);
        node.title = attribute(image->title);
      }
      break;
    default: node.kind = MarkdownNodeKind::text; break;
  }
  builder.push(std::move(node));
  return 0;
}

int leave_span(MD_SPANTYPE, void*, void* userdata) {
  static_cast<Builder*>(userdata)->pop();
  return 0;
}

int text_callback(const MD_TEXTTYPE type, const MD_CHAR* value,
                  const MD_SIZE size, void* userdata) {
  auto& builder = *static_cast<Builder*>(userdata);
  const auto start = builder.source_position(value, size);
  const auto finish = start + size;
  builder.cursor = std::max(builder.cursor,
      start >= builder.base ? finish - builder.base : builder.cursor);
  if (type == MD_TEXT_SOFTBR) {
    builder.leaf({.kind = MarkdownNodeKind::soft_break,
                  .source = {start, finish}});
  } else if (type == MD_TEXT_BR) {
    builder.leaf({.kind = MarkdownNodeKind::hard_break,
                  .source = {start, finish}});
  } else if (type != MD_TEXT_NULLCHAR) {
    builder.leaf({.kind = type == MD_TEXT_HTML ? MarkdownNodeKind::raw_html
                                               : MarkdownNodeKind::text,
                  .source = {start, finish},
                  .text = std::string(value, size)});
  }
  return 0;
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
      out.markup(std::string("<pre class=\"code-block") +
                 (node.kind == MarkdownNodeKind::diff_block ? " diff-block" : "") +
                 (node.kind == MarkdownNodeKind::tool_call ? " tool-call" : "") +
                 (node.kind == MarkdownNodeKind::tool_result ? " tool-result" : "") +
                 "\"><code data-language=\"" + escape(node.metadata) + "\">");
      close = "</code></pre>";
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
  Builder builder(markdown, source_offset);
  MD_PARSER parser{};
  parser.abi_version = 0;
  parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                 MD_FLAG_PERMISSIVEAUTOLINKS;
  parser.enter_block = enter_block;
  parser.leave_block = leave_block;
  parser.enter_span = enter_span;
  parser.leave_span = leave_span;
  parser.text = text_callback;
  (void)md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
                 &parser, &builder);
  normalize_top_level_ranges(builder.document, source_offset + markdown.size());
  finalize_node(builder.document, builder.document.root, 0);
  return std::move(builder.document);
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

} // namespace tokmon::desk
