#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tokmon.h"
#include "tokmon/tokmon.hpp"

namespace tokmon::desktop {

struct TraceSummary final {
  std::string duration{"0ms"};
  std::string turn_duration{"0ms"};
  int turns{0};
  int calls{0};
  std::int64_t input_tokens{0};
  std::int64_t output_tokens{0};
  std::string provider{"-"};
  std::string model{"-"};
  std::string result{"等待输入"};
};

struct SessionFile final {
  std::string name;
  std::string path;
  std::string content;
  bool written{false};
};

slint::SharedString time_label(std::int64_t unix_ms);
TimelineItem timeline_item(const tokmon::Photon &photon);
std::string bounded_detail(std::string value, std::size_t capacity = 220u);
std::string act_field(const tokmon::Photon &photon, std::string_view key);
std::vector<TimelineItem>
conversation_workflow_from(const std::vector<tokmon::Photon> &photons,
                           std::string *thought_text = nullptr);
TraceSummary trace_summary_from(const std::vector<tokmon::Photon> &photons);
std::vector<CodeLine>
code_lines_from(const std::vector<tokmon::Photon> &photons);
SessionFile session_file_from_photon(const tokmon::Photon &photon);
std::vector<CodeLine> code_lines_from_text(const std::string &content);

} // namespace tokmon::desktop
