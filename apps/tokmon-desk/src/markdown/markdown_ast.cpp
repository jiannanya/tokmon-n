#include "markdown/markdown_ast.hpp"

#include <md4c.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tokmon::desk {
namespace {

struct Builder {
  MarkdownDocument document{{MarkdownNode{MarkdownNodeKind::document}}, 0};
  std::vector<std::size_t> stack{0};

  std::size_t push(MarkdownNode node) {
    const auto index = document.nodes.size();
    document.nodes.push_back(std::move(node));
    document.nodes[stack.back()].children.push_back(index);
    stack.push_back(index);
    return index;
  }
  void pop() {
    if (stack.size() > 1)
      stack.pop_back();
  }
  void leaf(MarkdownNode node) {
    const auto index = document.nodes.size();
    document.nodes.push_back(std::move(node));
    document.nodes[stack.back()].children.push_back(index);
  }
};

int enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
  auto& builder = *static_cast<Builder*>(userdata);
  MarkdownNode node;
  switch (type) {
    case MD_BLOCK_DOC: return 0;
    case MD_BLOCK_QUOTE: node.kind = MarkdownNodeKind::block_quote; break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL: node.kind = MarkdownNodeKind::list; break;
    case MD_BLOCK_LI: node.kind = MarkdownNodeKind::list_item; break;
    case MD_BLOCK_HR: builder.leaf({MarkdownNodeKind::rule}); return 0;
    case MD_BLOCK_H:
      node.kind = MarkdownNodeKind::heading;
      node.level = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
      break;
    case MD_BLOCK_CODE:
      node.kind = MarkdownNodeKind::code_block;
      if (detail) {
        const auto& info = static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->info;
        node.metadata.assign(info.text ? info.text : "", info.size);
      }
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

int leave_block(MD_BLOCKTYPE type, void*, void* userdata) {
  if (type != MD_BLOCK_DOC && type != MD_BLOCK_HR)
    static_cast<Builder*>(userdata)->pop();
  return 0;
}

int enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
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
        const auto& href = static_cast<MD_SPAN_A_DETAIL*>(detail)->href;
        node.metadata.assign(href.text ? href.text : "", href.size);
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

int text_callback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
  auto& builder = *static_cast<Builder*>(userdata);
  if (type == MD_TEXT_SOFTBR) {
    builder.leaf({MarkdownNodeKind::soft_break});
  } else if (type == MD_TEXT_BR) {
    builder.leaf({MarkdownNodeKind::hard_break});
  } else if (type != MD_TEXT_NULLCHAR) {
    builder.leaf({MarkdownNodeKind::text, std::string(text, size)});
  }
  return 0;
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
      default: out += ch; break;
    }
  }
  return out;
}

bool safe_link(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  const auto colon = value.find(':');
  if (colon == std::string_view::npos)
    return true;
  std::string scheme(value.substr(0, colon));
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return scheme == "http" || scheme == "https" || scheme == "mailto";
}

void render_node(const MarkdownDocument& document, std::size_t index, std::ostringstream& out) {
  const auto& node = document.nodes[index];
  const char* open = "";
  const char* close = "";
  switch (node.kind) {
    case MarkdownNodeKind::document: break;
    case MarkdownNodeKind::paragraph: open = "<p>"; close = "</p>"; break;
    case MarkdownNodeKind::heading:
      out << "<h" << (node.level ? node.level : 1) << ">";
      close = nullptr;
      break;
    case MarkdownNodeKind::block_quote: open = "<blockquote>"; close = "</blockquote>"; break;
    case MarkdownNodeKind::list: open = "<ul>"; close = "</ul>"; break;
    case MarkdownNodeKind::list_item: open = "<li>"; close = "</li>"; break;
    case MarkdownNodeKind::code_block:
      out << "<pre class=\"code-block\"><code data-language=\"" << escape(node.metadata) << "\">";
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
      out << "<span class=\"safe-link\"";
      if (safe_link(node.metadata))
        out << " data-href=\"" << escape(node.metadata) << "\"";
      out << ">";
      close = "</span>";
      break;
    case MarkdownNodeKind::code: open = "<code>"; close = "</code>"; break;
    case MarkdownNodeKind::text: out << escape(node.text); return;
    case MarkdownNodeKind::soft_break: out << "\n"; return;
    case MarkdownNodeKind::hard_break: out << "<br/>"; return;
    case MarkdownNodeKind::rule: out << "<hr/>"; return;
  }
  out << open;
  for (const auto child : node.children)
    render_node(document, child, out);
  if (node.kind == MarkdownNodeKind::heading) {
    out << "</h" << (node.level ? node.level : 1) << ">";
  } else if (close) {
    out << close;
  }
}

} // namespace

MarkdownDocument MarkdownParser::parse(std::string_view markdown) const {
  Builder builder;
  MD_PARSER parser{};
  parser.abi_version = 0;
  parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                 MD_FLAG_PERMISSIVEAUTOLINKS;
  parser.enter_block = enter_block;
  parser.leave_block = leave_block;
  parser.enter_span = enter_span;
  parser.leave_span = leave_span;
  parser.text = text_callback;
  md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser, &builder);
  return std::move(builder.document);
}

std::string markdown_to_safe_rml(const MarkdownDocument& document) {
  std::ostringstream out;
  render_node(document, document.root, out);
  return out.str();
}

} // namespace tokmon::desk
