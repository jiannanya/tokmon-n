#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon/error.hpp"

namespace tokmon {

struct SlashCommandDescriptor {
  std::string name;
  std::vector<std::string> aliases;
  std::string usage;
  std::string summary;
  std::string category;
  bool requires_ray{false};
};

struct ParsedSlashCommand {
  const SlashCommandDescriptor* descriptor{nullptr};
  std::string invoked_name;
  std::string raw_arguments;
  std::vector<std::string> arguments;
};

// The catalog is the single command source used by Snow, the CLI and Termon.
[[nodiscard]] const std::vector<SlashCommandDescriptor>& slash_command_catalog();
[[nodiscard]] const SlashCommandDescriptor* find_slash_command(std::string_view name);
[[nodiscard]] bool is_slash_command(std::string_view text) noexcept;
[[nodiscard]] Result<ParsedSlashCommand> parse_slash_command(std::string_view text);
[[nodiscard]] std::vector<const SlashCommandDescriptor*> match_slash_commands(
    std::string_view query, std::size_t limit = 12);
[[nodiscard]] std::string slash_command_help(
    const SlashCommandDescriptor& command);
[[nodiscard]] std::string slash_command_help();

}  // namespace tokmon
