#pragma once

#include <cstdint>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::compiler {
enum class NativeCodePlacement : std::uint8_t;
}

namespace vela::compiler::native {

struct GlobalAccessFacts final {
  std::vector<std::uint8_t> readonly_slots;
  std::vector<std::vector<std::uint32_t>> initialized_on_return;
  std::vector<std::vector<std::uint8_t>> initialized_accesses;
};

[[nodiscard]] GlobalAccessFacts AnalyzeGlobalAccess(const ir::Program& program,
                                                    NativeCodePlacement placement);

}  // namespace vela::compiler::native
