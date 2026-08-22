#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "tokmon/lens.hpp"

namespace tokmon {

[[nodiscard]] std::shared_ptr<ILens> make_builtin_lens(std::string_view short_id);
[[nodiscard]] LensManifest builtin_lens_manifest(std::string_view short_id);
[[nodiscard]] std::vector<std::string> official_lens_order();

}  // namespace tokmon
