#pragma once

#include <cstdint>
#include <string_view>

#include "base/native_program_mode.h"

namespace vela::compiler {

enum class NativeOptimization : std::uint8_t { None, Release };
enum class NativeCodePlacement : std::uint8_t { Linked, Dynamic };

[[nodiscard]] constexpr bool IsClosedLinkedProgram(NativeProgramMode mode,
                                                   NativeCodePlacement placement) {
  return mode == NativeProgramMode::Closed && placement == NativeCodePlacement::Linked;
}

[[nodiscard]] constexpr std::string_view NativeOptimizationName(NativeOptimization optimization) {
  return optimization == NativeOptimization::Release ? "O3" : "O0";
}

}  // namespace vela::compiler
