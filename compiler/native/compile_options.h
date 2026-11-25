#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "base/native_program_mode.h"

namespace vela::compiler {

enum class NativeOptimization : std::uint8_t { None, Baseline, Release };
enum class NativeCodePlacement : std::uint8_t { Linked, Dynamic };

[[nodiscard]] constexpr bool IsClosedLinkedProgram(NativeProgramMode mode,
                                                   NativeCodePlacement placement) {
  return mode == NativeProgramMode::Closed && placement == NativeCodePlacement::Linked;
}

[[nodiscard]] constexpr std::string_view NativeOptimizationName(NativeOptimization optimization) {
  switch (optimization) {
    case NativeOptimization::None:
      return "O0";
    case NativeOptimization::Baseline:
      return "O1";
    case NativeOptimization::Release:
      return "O3";
  }
  std::unreachable();
}

}  // namespace vela::compiler
