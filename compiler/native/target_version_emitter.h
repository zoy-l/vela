#pragma once

#include <span>
#include <string>
#include <vector>

#include "compiler/ir/ir.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace vela::compiler::native {

struct GeneratedFunction;
struct NativeCallPlan;

struct NativeTargetGroup final {
  std::vector<llvm::Function*> functions;
  llvm::Function* entry = nullptr;
};

std::vector<NativeTargetGroup> PlanTargetVersions(const ir::Program& program,
                                                  const NativeCallPlan& calls,
                                                  std::span<const GeneratedFunction> baseline,
                                                  std::span<const GeneratedFunction> optimized,
                                                  std::span<const GeneratedFunction> specialized);

void EmitTargetVersions(llvm::Module& module,
                        std::span<const NativeTargetGroup> groups,
                        std::span<std::vector<std::string>> symbols);

}  // namespace vela::compiler::native
