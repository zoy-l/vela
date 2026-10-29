#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::compiler::native {

struct NativeRootPlan final {
  std::size_t count = 0;
  std::vector<std::optional<std::size_t>> slots;
  std::vector<std::vector<ir::RegisterId>> stores;
  std::vector<std::vector<ir::RegisterId>> clears;
  std::vector<std::vector<ir::RegisterId>> suspended;
  std::vector<std::vector<std::size_t>> rematerialized;
  std::vector<std::uint8_t> safepoints;
};

struct NativeFunctionMetrics final {
  std::uint64_t shadow_slots = 0;
  std::uint64_t frame_bytes = 0;
  std::uint64_t root_stores = 0;
};

[[nodiscard]] NativeRootPlan PlanNativeRoots(const ir::Function& function,
                                             std::span<const ir::Function> functions,
                                             NativeProgramMode mode);

}  // namespace vela::compiler::native
