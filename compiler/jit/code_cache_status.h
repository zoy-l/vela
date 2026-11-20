#pragma once

#include <cstdint>

namespace vela::compiler::jit {

enum class CodeCacheStatus : std::uint8_t { Disabled, Miss, Hit, Invalid };

}  // namespace vela::compiler::jit
