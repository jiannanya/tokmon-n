#pragma once

#include <filesystem>

#include "tokmon/lens.hpp"

namespace tokmon {

[[nodiscard]] Result<LensManifest> load_lens_manifest(
    const std::filesystem::path& path);

}  // namespace tokmon
