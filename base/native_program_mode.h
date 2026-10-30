#pragma once

#include <cstdint>

namespace vela {

enum class NativeProgramMode : std::uint8_t { Closed, Reloadable };
enum class NativeCodeTier : std::uint8_t { Baseline, Optimized };

}  // namespace vela
