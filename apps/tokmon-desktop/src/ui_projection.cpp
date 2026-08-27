#include "ui_projection.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <md4c.h>

#include "platform_utils.hpp"

namespace tokmon::desktop {

slint::SharedString time_label(const std::int64_t unix_ms) {
  const auto time = static_cast<std::time_t>(unix_ms / 1000);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  std::ostringstream stream;
  stream << std::put_time(&local, "%H:%M");
  return slint::SharedString(stream.str());
}

TimelineItem timeline_item(const tokmon::Photon &photon) {
  TimelineItem item;
  item.time = time_label(photon.committed_at_ms);
  item.kind = display_string(photon.kind);
  item.title = display_string(photon.kind);
  item.detail = display_string(tokmon::cbor::diagnostic(photon.payload));
  item.progress = -1;
  if (photon.kind == "act.failed" || photon.kind == "act.rejected")
    item.tone = "danger";
  else if (photon.kind == "act.completed" ||
           photon.kind == "assistant.message" || photon.kind == "tool.result" ||
           photon.kind == "ray.darkened")
    item.tone = "success";
  else if (photon.kind.starts_with("act."))
    item.tone = "warning";
  else
    item.tone = "neutral";
  if (photon.kind == "worker.progress") {
    if (const auto *progress = tokmon::cbor::find(photon.payload, "percent"))
      item.progress = static_cast<int>(progress->as_integer());
  }
  return item;
}

std::string payload_text(const tokmon::cbor::Value &payload,
                         const std::string_view key) {
  const auto *value = tokmon::cbor::find(payload, key);
  if (!value)
    return {};
  if (std::holds_alternative<std::string>(value->data))
    return std::string(value->as_string());
  return tokmon::cbor::diagnostic(*value);
}

std::string bounded_detail(std::string value, const std::size_t capacity) {
  if (value.size() <= capacity)
    return value;
  value.resize(capacity);
  return display_utf8(value) + "…";
}

std::string joined_detail(std::initializer_list<std::string> parts) {
  std::string result;
  for (auto &part : parts) {
    if (part.empty())
      continue;
    if (!result.empty())
      result.append(" · ");
    result.append(part);
  }
  return bounded_detail(std::move(result));
}

std::string act_field(const tokmon::Photon &photon,
                      const std::string_view key) {
  const auto *act = tokmon::cbor::find(photon.payload, "act");
  return act ? payload_text(*act, key) : std::string{};
}

std::string attempt_detail(const tokmon::Photon &photon) {
  const auto attempt = payload_text(photon.payload, "attempt");
  return joined_detail(
      {payload_text(photon.payload, "provider"),
       payload_text(photon.payload, "model"),
       attempt.empty() ? std::string{} : "第 " + attempt + " 次"});
}

std::optional<TimelineItem>
conversation_workflow_item(const tokmon::Photon &photon) {
  TimelineItem item;
  item.time = time_label(photon.committed_at_ms);
  item.kind = display_string(photon.kind);
  item.progress = -1;
  item.tone = "neutral";
  std::string title;
  std::string detail;

  if (photon.kind == "model.tool-call") {
    const auto tool = payload_text(photon.payload, "tool");
    const auto *arguments = tokmon::cbor::find(photon.payload, "arguments");
    title = tool == "write_file"    ? "Agent 准备写入文件"
            : tool == "read_file"   ? "Agent 准备回读验证"
            : tool == "run_command" ? "Agent 准备运行验证命令"
            : tool == "calculate"   ? "Agent 准备计算"
                                    : "Agent 调用工具：" + tool;
    detail = arguments
                 ? bounded_detail(tokmon::cbor::diagnostic(*arguments), 320)
                 : tool;
    item.tone = "warning";
  } else if (photon.kind == "model.failed") {
    title = "Agent 无法继续处理";
    detail = payload_text(photon.payload, "error");
    item.tone = "danger";
  } else if (photon.kind == "act.started") {
    return std::nullopt;
  } else if (photon.kind == "act.completed") {
    return std::nullopt;
  } else if (photon.kind == "act.failed" || photon.kind == "act.rejected") {
    const auto kind = act_field(photon, "kind");
    if (kind == "model.call")
      return std::nullopt;
    title = photon.kind == "act.rejected" ? "工具执行被拒绝" : "工具执行失败";
    if (!kind.empty())
      title.append("：" + kind);
    detail = joined_detail(
        {act_field(photon, "target"), payload_text(photon.payload, "error")});
    item.tone = "danger";
  } else if (photon.kind == "tool.result") {
    title = "Agent 已获得工具结果";
    detail = joined_detail({payload_text(photon.payload, "tool"),
                            payload_text(photon.payload, "result")});
    item.tone = "success";
  } else if (photon.kind == "assistant.message") {
    return std::nullopt;
  } else if (photon.kind == "fs.read-completed" || photon.kind == "fs.read") {
    title = "Agent 已回读文件";
    detail = payload_text(photon.payload, "path");
    if (const auto content = payload_text(photon.payload, "content");
        !content.empty())
      detail = joined_detail({detail, "内容：" + bounded_detail(content, 160)});
    item.tone = "success";
  } else if (photon.kind == "fs.changed" || photon.kind == "fs.written" ||
             photon.kind == "fs.created") {
    const auto operation = payload_text(photon.payload, "operation");
    title = operation == "create" || photon.kind == "fs.created"
                ? "Agent 已创建文件"
                : "Agent 已写入文件";
    detail = joined_detail(
        {payload_text(photon.payload, "path"),
         payload_text(photon.payload, "bytes").empty()
             ? std::string{}
             : payload_text(photon.payload, "bytes") + " bytes",
         tokmon::cbor::find(photon.payload, "write_verified") &&
                 tokmon::cbor::find(photon.payload, "write_verified")->as_bool()
             ? "已回读校验"
             : std::string{}});
    item.tone = "success";
  } else if (photon.kind == "fs.deleted") {
    title = "删除文件";
    detail = payload_text(photon.payload, "path");
    item.tone = "warning";
  } else if (photon.kind == "process.started") {
    title = "Agent 正在运行命令";
    detail = payload_text(photon.payload, "argv");
    item.tone = "warning";
  } else if (photon.kind == "process.stdout" ||
             photon.kind == "process.stderr" ||
             photon.kind == "process.output") {
    title = "命令输出";
    detail = bounded_detail(payload_text(photon.payload, "text"));
    item.tone = photon.kind == "process.stderr" ? "warning" : "neutral";
  } else if (photon.kind == "process.exited" || photon.kind == "process.exit") {
    const auto code = payload_text(photon.payload, "exit_code");
    title = code == "0" || code.empty() ? "Agent 已完成命令验证"
                                        : "Agent 命令执行失败 (" + code + ")";
    detail = payload_text(photon.payload, "summary");
    item.tone = code == "0" || code.empty() ? "success" : "danger";
  } else if (photon.kind == "worker.progress") {
    title = "正在执行任务";
    detail = payload_text(photon.payload, "status");
    item.tone = "warning";
    if (const auto *progress = tokmon::cbor::find(photon.payload, "percent"))
      item.progress = static_cast<int>(progress->as_integer());
  } else if (photon.kind == "workflow.defined") {
    title = "透镜工作流已定义";
    detail = payload_text(photon.payload, "name");
  } else if (photon.kind == "workflow.step-dispatched") {
    title = "工作流步骤开始";
    detail = joined_detail({payload_text(photon.payload, "node_id"),
                            payload_text(photon.payload, "kind")});
    item.tone = "warning";
  } else if (photon.kind == "workflow.step-completed") {
    title = "工作流步骤完成";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "success";
  } else if (photon.kind == "workflow.step-failed" ||
             photon.kind == "workflow.compensation-failed") {
    title = photon.kind == "workflow.step-failed" ? "工作流步骤失败"
                                                  : "补偿行动失败";
    detail = joined_detail({payload_text(photon.payload, "node_id"),
                            payload_text(photon.payload, "error")});
    item.tone = "danger";
  } else if (photon.kind == "workflow.step-skipped") {
    title = "工作流步骤已跳过";
    detail = payload_text(photon.payload, "node_id");
  } else if (photon.kind == "workflow.step-retry-requested") {
    title = "工作流步骤准备重试";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "warning";
  } else if (photon.kind == "workflow.compensation-dispatched") {
    title = "补偿行动开始";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "warning";
  } else if (photon.kind == "workflow.compensation-completed") {
    title = "补偿行动完成";
    detail = payload_text(photon.payload, "node_id");
    item.tone = "success";
  } else if (photon.kind == "workflow.paused" ||
             photon.kind == "workflow.resumed" ||
             photon.kind == "workflow.cancelled") {
    title = photon.kind == "workflow.paused"    ? "工作流已暂停"
            : photon.kind == "workflow.resumed" ? "工作流已恢复"
                                                : "工作流已取消";
    detail = payload_text(photon.payload, "node_id");
    item.tone = photon.kind == "workflow.resumed"     ? "success"
                : photon.kind == "workflow.cancelled" ? "danger"
                                                      : "warning";
  } else {
    return std::nullopt;
  }

  item.title = display_string(title);
  item.detail = display_string(detail);
  return item;
}

std::vector<TimelineItem>
conversation_workflow_from(const std::vector<tokmon::Photon> &photons,
                           std::string *thought_text) {
  std::uint64_t turn_start = 0;
  for (auto iterator = photons.rbegin(); iterator != photons.rend(); ++iterator)
    if (iterator->kind == "user.input" || iterator->kind == "user.message") {
      turn_start = iterator->sequence;
      break;
    }
  std::vector<TimelineItem> result;
  const tokmon::Photon *assistant = nullptr;
  const tokmon::Photon *failure = nullptr;
  const tokmon::Photon *latest_tool_result = nullptr;
  int tool_calls = 0;
  int verified_actions = 0;
  std::string reasoning_text;
  for (const auto &photon : photons) {
    if (photon.sequence < turn_start)
      continue;
    // Reasoning chunks feed the dedicated thought-process card above the
    // assistant reply instead of the workflow step timeline.
    if (photon.kind == "model.reasoning-chunk") {
      reasoning_text.append(payload_text(photon.payload, "text"));
      continue;
    }
    if (photon.kind == "assistant.message")
      assistant = &photon;
    if (photon.kind == "model.tool-call")
      ++tool_calls;
    if (photon.kind == "tool.result" || photon.kind == "fs.changed" ||
        photon.kind == "fs.read-completed" || photon.kind == "process.exited") {
      latest_tool_result = &photon;
      ++verified_actions;
    }
    if (photon.kind == "model.failed" || photon.kind == "act.failed")
      failure = &photon;
    if (auto item = conversation_workflow_item(photon))
      result.push_back(std::move(*item));
  }
  if (thought_text)
    *thought_text = bounded_detail(std::move(reasoning_text), 8'192u);
  const bool verified_complete =
      assistant && tool_calls > 0 && latest_tool_result &&
      assistant->sequence > latest_tool_result->sequence;
  if (verified_complete) {
    TimelineItem done;
    done.time = time_label(assistant->committed_at_ms);
    done.kind = "task.completed";
    done.title = "任务已完成";
    done.detail = display_string(
        "已完成 " + std::to_string(verified_actions) +
        " 个可验证行动并给出最终结果；完整证据请在「轨迹」页查看");
    done.tone = "success";
    done.progress = -1;
    result.push_back(std::move(done));
  } else if (assistant && tool_calls == 0) {
    TimelineItem reply;
    reply.time = time_label(assistant->committed_at_ms);
    reply.kind = "agent.reply-only";
    reply.title = "Agent 已给出回复，但未执行工具";
    reply.detail =
        "未检测到可验证的文件、命令或计算行动；本回合不标记为任务完成";
    reply.tone = "warning";
    reply.progress = -1;
    result.push_back(std::move(reply));
  } else if (failure) {
    TimelineItem failed;
    failed.time = time_label(failure->committed_at_ms);
    failed.kind = "task.failed";
    failed.title = "任务执行失败";
    failed.detail =
        "已完成既定重试仍未成功；完整错误与重试轨迹请在「轨迹」页查看";
    failed.tone = "danger";
    failed.progress = -1;
    result.push_back(std::move(failed));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Markdown -> ChatBlock projection (md4c based)
//
// Every markdown block becomes one UI row. Inline spans are re-serialized
// into the StyledText markup subset (bold/emphasis/strike/inline-code/link),
// the richest per-line styling the Slint renderer can express today.
// Tables degrade to a fixed-width grid rendered as a monospace code box.
// ---------------------------------------------------------------------------

namespace {

std::string decode_entity(std::string_view entity) {
  // entity arrives without '&' and ';'. Cover the handful HTML produces in
  // model output; unknown entities fall back to their bare name.
  if (entity == "amp")
    return "&";
  if (entity == "lt")
    return "<";
  if (entity == "gt")
    return ">";
  if (entity == "quot")
    return "\"";
  if (entity == "apos")
    return "'";
  return std::string(entity);
}

std::string sanitize_href(std::string_view href) {
  std::string out;
  out.reserve(href.size());
  for (const char c : href) {
    switch (c) {
    case '(':
      out += "%28";
      break;
    case ')':
      out += "%29";
      break;
    case ' ':
      out += "%20";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

bool html_tag_allowed(std::string_view tag) {
  // Only <u> is among the HTML extensions StyledText documents;
  // unknown tags would make from_markdown bail to plain text.
  return tag == "<u>" || tag == "</u>";
}

struct MdTableRow {
  std::vector<std::string> cells;
};

class MarkdownBlocksBuilder final {
public:
  std::vector<ChatBlock> take() {
    detach_row();
    if (in_table_)
      flush_table();
    for (auto &block : blocks_)
      finish_block(block);
    return std::move(blocks_);
  }

  // Converts the serialized markup into a runtime StyledText value. The
  // @markdown() macro only interpolates literals, so host-side conversion is
  // the documented path (slint issue #11158 / milestone 1.17 API).
  static void finish_block(ChatBlock &block) {
    const std::string source(block.title);
    auto parsed = slint::StyledText::from_markdown(source);
    if (!parsed)
      parsed = slint::StyledText::from_plain_text(source);
    block.rich = std::move(*parsed);
  }

  void enter_block(MD_BLOCKTYPE type, void *detail) {
    switch (type) {
    case MD_BLOCK_P:
      if (!row_ && !in_table_)
        new_paragraph();
      break;
    case MD_BLOCK_H: {
      detach_row();
      auto &row = new_paragraph();
      in_heading_ = true;
      if (detail)
        heading_level_ = static_cast<int>(
            reinterpret_cast<MD_BLOCK_H_DETAIL *>(detail)->level);
      row.kind = display_string("heading");
      row.depth = heading_level_;
      break;
    }
    case MD_BLOCK_UL:
      close_for_structure();
      list_ordered_.push_back(false);
      ordered_next_.push_back(1);
      break;
    case MD_BLOCK_OL:
      close_for_structure();
      list_ordered_.push_back(true);
      ordered_next_.push_back(
          detail ? static_cast<long long>(
                       reinterpret_cast<MD_BLOCK_OL_DETAIL *>(detail)->start)
                 : 1);
      break;
    case MD_BLOCK_LI: {
      const bool ordered = !list_ordered_.empty() && list_ordered_.back();
      ChatBlock &row =
          list_item(ordered ? "ordered" : "bullet");
      long long &next = ordered_next_.back();
      if (ordered) {
        row.ordinal = display_string(std::to_string(next) + ".");
        ++next;
      }
      fresh_li_ = true;
      break;
    }
    case MD_BLOCK_CODE:
      close_for_structure();
      in_code_block_ = true;
      code_buffer_.clear();
      break;
    case MD_BLOCK_QUOTE:
      ++quote_depth_;
      detach_row();
      break;
    case MD_BLOCK_HR:
      close_for_structure();
      blocks_.emplace_back();
      blocks_.back().kind = display_string("rule");
      detach_row();
      break;
    case MD_BLOCK_TABLE:
      close_for_structure();
      in_table_ = true;
      md_table_rows_.clear();
      md_table_cols_ =
          detail ? static_cast<size_t>(
                       reinterpret_cast<MD_BLOCK_TABLE_DETAIL *>(detail)
                           ->col_count)
                 : size_t{1};
      break;
    case MD_BLOCK_TR:
      md_table_rows_.emplace_back();
      break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      end_cell();
      md_active_cell_.emplace();
      break;
    default:
      // MD_BLOCK_DOC / THEAD / TBODY are structural only; HTML blocks drop.
      break;
    }
  }

  void leave_block(MD_BLOCKTYPE type, void *detail) {
    switch (type) {
    case MD_BLOCK_P:
    case MD_BLOCK_LI:
      detach_row();
      break;
    case MD_BLOCK_H:
      in_heading_ = false;
      detach_row();
      break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      list_ordered_.pop_back();
      ordered_next_.pop_back();
      detach_row();
      break;
    case MD_BLOCK_CODE:
      while (!code_buffer_.empty() &&
             (code_buffer_.back() == '\n' || code_buffer_.back() == '\r'))
        code_buffer_.pop_back();
      if (!code_buffer_.empty()) {
        auto &row = blocks_.emplace_back();
        row.kind = display_string("code");
        row.depth = 0;
        row.title = display_string(code_buffer_);
      }
      in_code_block_ = false;
      code_buffer_.clear();
      detach_row();
      break;
    case MD_BLOCK_QUOTE:
      quote_depth_ = std::max(0, quote_depth_ - 1);
      detach_row();
      break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      end_cell();
      break;
    case MD_BLOCK_TABLE:
      flush_table();
      in_table_ = false;
      break;
    default:
      break;
    }
    (void)detail;
  }

  void enter_span(MD_SPANTYPE type, void *detail) {
    switch (type) {
    case MD_SPAN_CODE:
      emit("`");
      break;
    case MD_SPAN_STRONG:
      emit("**");
      break;
    case MD_SPAN_EM:
      emit("*");
      break;
    case MD_SPAN_DEL:
      emit("~~");
      break;
    case MD_SPAN_U:
      emit("<u>");
      break;
    case MD_SPAN_A:
      md_links_.push_back(sanitize_href(
          span_href(reinterpret_cast<MD_SPAN_A_DETAIL *>(detail))));
      if (!md_active_cell_)
        emit("[");
      break;
    default:
      // IMG renders below as plain alt text via its inner spans; LATEXMATH
      // and WIKILINK stay disabled by dialect flags.
      break;
    }
  }

  void leave_span(MD_SPANTYPE type, void *detail) {
    switch (type) {
    case MD_SPAN_CODE:
      emit("`");
      break;
    case MD_SPAN_STRONG:
      emit("**");
      break;
    case MD_SPAN_EM:
      emit("*");
      break;
    case MD_SPAN_DEL:
      emit("~~");
      break;
    case MD_SPAN_U:
      emit("</u>");
      break;
    case MD_SPAN_A:
      if (!md_active_cell_ && !md_links_.empty())
        emit("](" + md_links_.back() + ")");
      if (!md_links_.empty())
        md_links_.pop_back();
      break;
    default:
      break;
    }
    (void)detail;
  }

  void text(MD_TEXTTYPE type, const MD_CHAR *data, MD_SIZE size) {
    const std::string_view raw(data, size);
    if (in_code_block_) {
      if (type == MD_TEXT_CODE || type == MD_TEXT_NORMAL ||
          type == MD_TEXT_NULLCHAR || type == MD_TEXT_HTML ||
          type == MD_TEXT_ENTITY)
        code_buffer_ += std::string(raw);
      return;
    }
    if (md_active_cell_) {
      append_cell(raw, type);
      return;
    }
    switch (type) {
    case MD_TEXT_NORMAL: {
      std::string_view body = raw;
      if (fresh_li_) {
        fresh_li_ = false;
        if (body.starts_with("[ ] ")) {
          emit("\xE2\x96\xA1 ");
          body.remove_prefix(4);
        } else if (body.starts_with("[x] ") ||
                   body.starts_with("[X] ")) {
          emit("\xE2\x9C\x93 ");
          body.remove_prefix(4);
        }
      }
      append(body);
      break;
    }
    case MD_TEXT_NULLCHAR:
      emit("\xEF\xBF\xBD");
      break;
    case MD_TEXT_ENTITY: {
      const auto inner = raw.size() >= 2 && raw.front() == '&' &&
                                 raw.back() == ';'
                             ? raw.substr(1, raw.size() - 2)
                             : raw;
      append(decode_entity(inner));
      break;
    }
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
      emit("\n");
      break;
    case MD_TEXT_CODE: {
      // Inline code span contents.
      std::string piece(raw);
      piece.erase(std::remove(piece.begin(), piece.end(), '`'), piece.end());
      emit("`");
      append(piece);
      emit("`");
      break;
    }
    case MD_TEXT_HTML:
      if (html_tag_allowed(raw))
        append(raw);
      break;
    default:
      break;
    }
  }

private:
  static constexpr int kQuoteIndentUnits = 2;

  void append(std::string_view value) {
    if (!value.empty() && row_)
      row_->title += display_string(value);
  }

  ChatBlock &new_paragraph() {
    ChatBlock block;
    block.kind = display_string(
        list_ordered_.empty() && quote_depth_ == 0 ? "paragraph" : "paragraph");
    block.depth = static_cast<int>(list_ordered_.size()) +
                  kQuoteIndentUnits * quote_depth_;
    blocks_.push_back(std::move(block));
    row_ = &blocks_.back();
    return *row_;
  }

  ChatBlock &list_item(const char *kind) {
    ChatBlock block;
    block.kind = display_string(kind);
    block.depth = static_cast<int>(list_ordered_.size()) +
                  kQuoteIndentUnits * quote_depth_;
    blocks_.push_back(std::move(block));
    row_ = &blocks_.back();
    return *row_;
  }

  void detach_row() { row_ = nullptr; }

  void close_for_structure() { detach_row(); }

  void emit(std::string_view value) {
    if (md_active_cell_ || in_code_block_)
      return;
    if (!row_) {
      ChatBlock block;
      block.kind = display_string("paragraph");
      block.depth = static_cast<int>(list_ordered_.size()) +
                    kQuoteIndentUnits * quote_depth_;
      blocks_.push_back(std::move(block));
      row_ = &blocks_.back();
    }
    if (!value.empty() && row_)
      row_->title += display_string(value);
  }

  static std::string_view span_href(const MD_SPAN_A_DETAIL *detail) {
    if (!detail)
      return {};
    return {detail->href.text, detail->href.size};
  }

  void end_cell() {
    if (!md_active_cell_)
      return;
    if (md_table_rows_.empty())
      md_table_rows_.emplace_back();
    auto &cells = md_table_rows_.back().cells;
    cells.emplace_back();
    cells.back().swap(*md_active_cell_);
    while (!cells.back().empty() && cells.back().back() == ' ')
      cells.back().pop_back();
    md_active_cell_.reset();
  }

  void append_cell(std::string_view raw, MD_TEXTTYPE type) {
    std::string &target = *md_active_cell_;
    switch (type) {
    case MD_TEXT_NORMAL:
      target += std::string(raw);
      break;
    case MD_TEXT_ENTITY: {
      const auto inner = raw.size() >= 2 && raw.front() == '&' &&
                                 raw.back() == ';'
                             ? raw.substr(1, raw.size() - 2)
                             : raw;
      target += decode_entity(inner);
      break;
    }
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR:
      target += " ";
      break;
    case MD_TEXT_CODE: {
      std::string piece(raw);
      piece.erase(std::remove(piece.begin(), piece.end(), '`'), piece.end());
      target += std::move(piece);
      break;
    }
    default:
      break;
    }
  }

  void flush_table() {
    size_t columns = md_table_cols_;
    for (const auto &table_row : md_table_rows_)
      columns = std::max(columns, table_row.cells.size());
    std::vector<size_t> widths(columns, size_t{3});
    for (const auto &table_row : md_table_rows_)
      for (size_t index = 0; index < table_row.cells.size(); ++index)
        widths[index] =
            std::max(widths[index], table_row.cells[index].size());
    std::string grid;
    for (size_t r = 0; r < md_table_rows_.size(); ++r) {
      const auto &cells = md_table_rows_[r].cells;
      for (size_t c = 0; c < columns; ++c) {
        static const std::string kFallback;
        const std::string &value = c < cells.size() ? cells[c] : kFallback;
        if (c)
          grid += " | ";
        grid += value;
        grid.append(widths[c] > value.size() ? widths[c] - value.size() : 0,
                    ' ');
      }
      grid += "\n";
      if (r == 0)
        for (size_t c = 0; c < columns; ++c) {
          grid.append(widths[c], '-');
          grid += c + 1 == columns ? "-\n" : "-+-";
        }
    }
    while (!grid.empty() && grid.back() == '\n')
      grid.pop_back();
    if (!grid.empty()) {
      auto &row = blocks_.emplace_back();
      row.kind = display_string("code");
      row.depth = 0;
      row.title = display_string(grid);
    }
    detach_row();
    md_table_rows_.clear();
  }

  std::vector<ChatBlock> blocks_;
  ChatBlock *row_ = nullptr;

  bool fresh_li_ = false;
  bool in_heading_ = false;
  int heading_level_ = 1;
  bool in_code_block_ = false;
  std::string code_buffer_;

  int quote_depth_ = 0;
  std::vector<bool> list_ordered_;
  std::vector<long long> ordered_next_;

  bool in_table_ = false;
  size_t md_table_cols_ = 0;
  std::vector<MdTableRow> md_table_rows_;
  std::optional<std::string> md_active_cell_;
  std::vector<std::string> md_links_;
};

int markdown_enter_block_thunk(MD_BLOCKTYPE type, void *detail,
                              void *userdata) {
  static_cast<MarkdownBlocksBuilder *>(userdata)->enter_block(type, detail);
  return 0;
}

int markdown_leave_block_thunk(MD_BLOCKTYPE type, void *detail,
                              void *userdata) {
  static_cast<MarkdownBlocksBuilder *>(userdata)->leave_block(type, detail);
  return 0;
}

int markdown_enter_span_thunk(MD_SPANTYPE type, void *detail,
                              void *userdata) {
  static_cast<MarkdownBlocksBuilder *>(userdata)->enter_span(type, detail);
  return 0;
}

int markdown_leave_span_thunk(MD_SPANTYPE type, void *detail,
                              void *userdata) {
  static_cast<MarkdownBlocksBuilder *>(userdata)->leave_span(type, detail);
  return 0;
}

int markdown_text_thunk(MD_TEXTTYPE type, const MD_CHAR *data, MD_SIZE size,
                        void *userdata) {
  static_cast<MarkdownBlocksBuilder *>(userdata)->text(type, data, size);
  return 0;
}

} // namespace

std::vector<ChatBlock> chat_blocks_from(const std::string &markdown) {
  MarkdownBlocksBuilder builder;
  MD_PARSER parser{};
  parser.abi_version = 0;
  parser.flags = MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_TABLES |
                 MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                 MD_FLAG_PERMISSIVEURLAUTOLINKS;
  parser.enter_block = &markdown_enter_block_thunk;
  parser.leave_block = &markdown_leave_block_thunk;
  parser.enter_span = &markdown_enter_span_thunk;
  parser.leave_span = &markdown_leave_span_thunk;
  parser.text = &markdown_text_thunk;
  if (md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
               &parser, &builder) != 0)
    return {};
  return builder.take();
}

std::string duration_label(const std::int64_t milliseconds) {
    if (milliseconds < 1'000)
    return std::to_string(std::max<std::int64_t>(0, milliseconds)) + "ms";
  const auto seconds = milliseconds / 1'000;
  if (seconds < 60)
    return std::to_string(seconds) + "." +
           std::to_string((milliseconds % 1'000) / 100) + "s";
  const auto minutes = seconds / 60;
  return std::to_string(minutes) + "m " + std::to_string(seconds % 60) + "s";
}

TraceSummary trace_summary_from(const std::vector<tokmon::Photon> &photons) {
  TraceSummary summary;
  if (!photons.empty())
    summary.duration = duration_label(std::max<std::int64_t>(
        0, photons.back().committed_at_ms - photons.front().committed_at_ms));
  std::int64_t turn_start_ms = 0;
  std::uint64_t turn_start_sequence = 0;
  std::uint64_t latest_call = 0;
  std::uint64_t latest_result = 0;
  std::uint64_t latest_assistant = 0;
  std::uint64_t latest_failure = 0;
  for (const auto &photon : photons) {
    if (photon.kind == "user.input" || photon.kind == "user.message") {
      ++summary.turns;
      turn_start_ms = photon.committed_at_ms;
      turn_start_sequence = photon.sequence;
      latest_call = latest_result = latest_assistant = latest_failure = 0;
    }
    if (photon.kind == "model.dispatched")
      ++summary.calls;
    if (photon.sequence >= turn_start_sequence &&
        photon.kind == "model.tool-call")
      latest_call = photon.sequence;
    if (photon.sequence >= turn_start_sequence &&
        (photon.kind == "tool.result" || photon.kind == "fs.changed" ||
         photon.kind == "fs.read-completed" || photon.kind == "process.exited"))
      latest_result = photon.sequence;
    if (photon.sequence >= turn_start_sequence &&
        photon.kind == "assistant.message")
      latest_assistant = photon.sequence;
    if (photon.sequence >= turn_start_sequence &&
        (photon.kind == "model.failed" || photon.kind == "act.failed" ||
         photon.kind == "act.rejected"))
      latest_failure = photon.sequence;
    if (photon.kind == "model.usage") {
      if (const auto *value =
              tokmon::cbor::find(photon.payload, "input_tokens"))
        summary.input_tokens += value->as_integer();
      if (const auto *value =
              tokmon::cbor::find(photon.payload, "output_tokens"))
        summary.output_tokens += value->as_integer();
    }
    if (photon.kind.starts_with("model.") ||
        photon.kind == "assistant.message") {
      const auto provider = payload_text(photon.payload, "provider");
      const auto model = payload_text(photon.payload, "model");
      if (!provider.empty())
        summary.provider = provider;
      if (!model.empty())
        summary.model = model;
    }
  }
  if (latest_failure > std::max(latest_assistant, latest_result))
    summary.result = "执行失败";
  else if (latest_call > 0 && latest_result > latest_call &&
           latest_assistant > latest_result)
    summary.result = "已完成";
  else if (latest_assistant > 0)
    summary.result = latest_call == 0 ? "已回复（未执行工具）" : "等待工具结果";
  else if (turn_start_sequence > 0)
    summary.result = "执行中";
  if (turn_start_ms > 0 && !photons.empty())
    summary.turn_duration = duration_label(std::max<std::int64_t>(
        0, photons.back().committed_at_ms - turn_start_ms));
  return summary;
}

std::vector<CodeLine>
code_lines_from(const std::vector<tokmon::Photon> &photons) {
  std::string content;
  for (auto iterator = photons.rbegin(); iterator != photons.rend();
       ++iterator) {
    if (iterator->kind != "fs.read" && iterator->kind != "fs.written" &&
        iterator->kind != "fs.created" &&
        iterator->kind != "artifact.previewed")
      continue;
    const auto *field = tokmon::cbor::find(iterator->payload, "content");
    if (!field)
      field = tokmon::cbor::find(iterator->payload, "text");
    if (field && std::holds_alternative<std::string>(field->data)) {
      content = std::string(field->as_string());
      break;
    }
  }
  if (content.empty())
    content = "// 当前会话尚无文件变更。\n"
              "// 真实工具创建或修改文件后，内容会投影到此处。";
  std::vector<CodeLine> result;
  std::istringstream input(content);
  std::string text;
  for (std::size_t index = 0; std::getline(input, text) && index < 20'000u;
       ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u);
    line.text = display_string(text);
    const auto first = text.find_first_not_of(" \t");
    line.tone =
        first != std::string::npos && text[first] == '#' ? "comment" : "normal";
    result.push_back(std::move(line));
  }
  return result;
}

SessionFile session_file_from_photon(const tokmon::Photon &photon) {
  SessionFile file;
  const auto *field = tokmon::cbor::find(photon.payload, "path");
  file.path = field ? std::string(field->as_string()) : std::string{};
  file.name = path_basename_utf8(file.path);
  if (const auto *content = tokmon::cbor::find(photon.payload, "content");
      content && std::holds_alternative<std::string>(content->data))
    file.content = std::string(content->as_string());
  else if (const auto *text = tokmon::cbor::find(photon.payload, "text");
           text && std::holds_alternative<std::string>(text->data) &&
           (photon.kind == "fs.written" || photon.kind == "fs.created" ||
            photon.kind == "artifact.previewed"))
    file.content = std::string(text->as_string());
  file.written = photon.kind == "fs.written" || photon.kind == "fs.created";
  return file;
}

std::vector<CodeLine> code_lines_from_text(const std::string &content) {
  std::vector<CodeLine> result;
  std::istringstream input(content);
  std::string text;
  for (std::size_t index = 0; std::getline(input, text) && index < 20'000u;
       ++index) {
    CodeLine line;
    line.number = static_cast<int>(index + 1u);
    line.text = display_string(text);
    const auto first = text.find_first_not_of(" \t");
    line.tone =
        first != std::string::npos && text[first] == '#' ? "comment" : "normal";
    result.push_back(std::move(line));
  }
  if (result.empty()) {
    CodeLine single;
    single.number = 1;
    single.text =
        display_string(content.empty() ? "// 当前会话尚无文件投影。" : content);
    single.tone = "normal";
    result.push_back(std::move(single));
  }
  return result;
}

} // namespace tokmon::desktop
