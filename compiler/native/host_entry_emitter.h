#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "compiler/ir/ir.h"
#include "compiler/native/function_emitter.h"

namespace vela::compiler::native {

struct HostEntryPlan final {
  std::vector<std::uint8_t> needed;
  std::vector<std::vector<std::uint32_t>> globals;
};

[[nodiscard]] HostEntryPlan DeclareHostEntries(llvm::Module& module,
                                               const ir::Program& program,
                                               std::span<GeneratedFunction> baseline,
                                               std::span<GeneratedFunction> optimized);
void FinalizeHostEntries(llvm::Module& module,
                         const ir::Program& program,
                         const HostEntryPlan& plan,
                         std::span<GeneratedFunction> functions);

}  // namespace vela::compiler::native
