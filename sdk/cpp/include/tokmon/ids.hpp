#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tokmon {

using PhotonId = std::string;
using RayId = std::string;
using ActId = std::string;
using LensId = std::string;
using BandId = std::string;
using PortName = std::string;
using FieldCellId = std::string;
using AssemblyId = std::string;
using GenerationId = std::uint64_t;
using MountEpoch = std::uint64_t;

[[nodiscard]] std::string make_id(std::string_view prefix);
[[nodiscard]] std::int64_t unix_time_ms();

}  // namespace tokmon
