#include "tokmon/slash_commands.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tokmon {
namespace {

std::string lower(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

std::string trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return std::string(value);
}

Result<std::vector<std::string>> split_arguments(std::string_view value) {
  std::vector<std::string> result;
  std::string current;
  char quote = 0;
  bool escaped = false;
  for (const char character : value) {
    if (escaped) {
      current.push_back(character);
      escaped = false;
      continue;
    }
    if (character == '\\') {
      escaped = true;
      continue;
    }
    if (quote != 0) {
      if (character == quote) quote = 0;
      else current.push_back(character);
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        result.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (escaped) current.push_back('\\');
  if (quote != 0)
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "slash command contains an unclosed quote"));
  if (!current.empty()) result.push_back(std::move(current));
  return result;
}

}  // namespace

const std::vector<SlashCommandDescriptor>& slash_command_catalog() {
  static const std::vector<SlashCommandDescriptor> commands{
      {"help", {"commands"}, "/help [command]", "显示命令目录或某条命令的用法", "帮助"},
      {"clear", {"new", "reset"}, "/clear", "开始新会话；既有光子不会删除", "会话"},
      {"exit", {"quit"}, "/exit", "退出当前 CLI 或 Desktop", "会话"},
      {"status", {}, "/status", "显示守护进程、光路、模型与当前会话状态", "会话"},
      {"history", {}, "/history [ray-id]", "查看只追加的会话光子历史", "会话"},
      {"resume", {}, "/resume <ray-id>", "把指定光线设为当前会话", "会话"},
      {"rename", {}, "/rename <名称>", "为当前会话追加一个新标题事实", "会话", true},
      {"branch", {}, "/branch [名称]", "从当前因果尾创建分支，不改写原光线", "会话", true},
      {"rewind", {}, "/rewind <sequence>", "从历史序号创建新分支，不撤销原光子", "会话", true},
      {"copy", {}, "/copy [数量]", "复制最近的助手回复", "会话", true},
      {"export", {}, "/export [文件]", "导出当前会话的 Markdown 副本", "会话", true},
      {"context", {}, "/context [all]", "显示当前上下文组成与预算", "上下文", true},
      {"compact", {}, "/compact [关注点]", "通过 Textus 生成可追溯压缩摘要", "上下文", true},
      {"model", {}, "/model [平台-id]", "查看或切换模型平台", "运行时"},
      {"effort", {}, "/effort [low|medium|high|max]", "查看或设置当前推理强度", "运行时"},
      {"permissions", {}, "/permissions [full|restricted|read-only]", "查看或设置当前访问模式", "运行时"},
      {"config", {"settings"}, "/config", "打开或显示项目级 .tokmon YAML 配置", "运行时"},
      {"usage", {"cost", "stats"}, "/usage", "汇总当前会话的模型用量与光子统计", "运行时"},
      {"plan", {}, "/plan <任务>", "让智能体生成并执行可审计计划", "智能体"},
      {"tasks", {}, "/tasks", "查看当前任务与步骤投影", "智能体"},
      {"agents", {}, "/agents", "查看当前父子光线与智能体状态", "智能体"},
      {"fork", {"subtask"}, "/fork <任务>", "通过 Aya 派生受预算约束的子光线", "智能体", true},
      {"diff", {}, "/diff", "通过 Cove 查看工作区 Git 变更", "工程"},
      {"review", {}, "/review [目标]", "沿当前光路执行代码审查", "工程"},
      {"security-review", {}, "/security-review [目标]", "沿当前光路执行安全审查", "工程"},
      {"doctor", {"checkup"}, "/doctor", "验证存储、光路、运行时与连接健康", "诊断"},
      {"debug", {}, "/debug [问题]", "收集诊断并让智能体分析问题", "诊断"},
      {"init", {}, "/init", "检查并初始化项目级 .tokmon 约定", "工程"},
      {"lenses", {}, "/lenses [reconcile]", "查看或协调当前透镜光路", "透镜"},
      {"lens", {}, "/lens <list|reconcile>", "执行必要的单透镜管理入口", "透镜"},
      {"skills", {}, "/skills [discover]", "查看或通过 Enso 发现技能", "透镜"},
      {"mcp", {}, "/mcp", "查看 Iris 外部能力连接投影", "透镜"},
      {"memory", {}, "/memory", "查看已接受、可追溯的记忆事实", "透镜"},
  };
  return commands;
}

const SlashCommandDescriptor* find_slash_command(std::string_view name) {
  if (name.starts_with('/')) name.remove_prefix(1);
  const auto wanted = lower(name);
  for (const auto& command : slash_command_catalog()) {
    if (command.name == wanted) return &command;
    if (std::ranges::find(command.aliases, wanted) != command.aliases.end()) return &command;
  }
  return nullptr;
}

bool is_slash_command(const std::string_view text) noexcept {
  return !text.empty() && text.front() == '/';
}

Result<ParsedSlashCommand> parse_slash_command(const std::string_view text) {
  if (!is_slash_command(text))
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "slash command must start with '/'"));
  const auto separator = text.find_first_of(" \t\r\n");
  const auto invoked = lower(text.substr(1, separator == std::string_view::npos
      ? text.size() - 1 : separator - 1));
  if (invoked.empty())
    return tl::unexpected(make_error(ErrorCode::invalid_argument,
                                     "slash command name is required"));
  const auto* descriptor = find_slash_command(invoked);
  if (!descriptor)
    return tl::unexpected(make_error(ErrorCode::not_found,
                                     "unknown slash command /" + invoked));
  const auto raw = separator == std::string_view::npos
      ? std::string{} : trim(text.substr(separator + 1));
  auto arguments = split_arguments(raw);
  if (!arguments) return tl::unexpected(arguments.error());
  return ParsedSlashCommand{.descriptor = descriptor, .invoked_name = invoked,
                            .raw_arguments = raw, .arguments = std::move(*arguments)};
}

std::vector<const SlashCommandDescriptor*> match_slash_commands(
    std::string_view query, const std::size_t limit) {
  if (query.starts_with('/')) query.remove_prefix(1);
  if (const auto separator = query.find_first_of(" \t\r\n");
      separator != std::string_view::npos) query = query.substr(0, separator);
  const auto wanted = lower(query);
  struct Match { const SlashCommandDescriptor* command; int score; };
  std::vector<Match> matches;
  for (const auto& command : slash_command_catalog()) {
    int score = wanted.empty() ? 10 : -1;
    if (command.name.starts_with(wanted)) score = 0;
    else if (command.name.find(wanted) != std::string::npos) score = 2;
    else for (const auto& alias : command.aliases) {
      if (alias.starts_with(wanted)) { score = 1; break; }
      if (alias.find(wanted) != std::string::npos) score = 3;
    }
    if (score >= 0) matches.push_back(Match{&command, score});
  }
  std::stable_sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
    return left.score < right.score ||
        (left.score == right.score && left.command->name < right.command->name);
  });
  std::vector<const SlashCommandDescriptor*> result;
  for (const auto& match : matches) {
    if (result.size() == limit) break;
    result.push_back(match.command);
  }
  return result;
}

std::string slash_command_help(const SlashCommandDescriptor& command) {
  std::ostringstream output;
  output << command.usage << "\n" << command.summary;
  if (!command.aliases.empty()) {
    output << "\n别名：";
    for (std::size_t index = 0; index < command.aliases.size(); ++index) {
      if (index != 0) output << "、";
      output << '/' << command.aliases[index];
    }
  }
  return output.str();
}

std::string slash_command_help() {
  std::ostringstream output;
  std::string category;
  for (const auto& command : slash_command_catalog()) {
    if (command.category != category) {
      category = command.category;
      if (output.tellp() > 0) output << '\n';
      output << category << "：\n";
    }
    output << "  " << command.usage << " — " << command.summary << '\n';
  }
  return output.str();
}

}  // namespace tokmon
