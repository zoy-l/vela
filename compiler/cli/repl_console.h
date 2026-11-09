#pragma once

#include <filesystem>

#include "base/streams.h"

namespace vela::tooling {

[[nodiscard]] int RunRepl(const std::filesystem::path& directory, Streams streams);

}  // namespace vela::tooling
