#include "ui_projection.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
