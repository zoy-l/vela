#pragma once

#include <span>
#include <string>
#include <vector>

#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "compiler/ir/ir.h"

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace vela::compiler::native {

struct NativeCallPlan;
struct FunctionDeclarations;

struct NativeTargetGroup final {
  std::vector<llvm::Function*> functions;
  llvm::Function* entry = nullptr;
};

void CloneWideVersion(llvm::Function& generic,
                      llvm::Function& wide,
                      llvm::ValueToValueMapTy& values);
llvm::Value* WideTargetCondition(llvm::IRBuilder<>& builder, llvm::Value* target);

std::vector<NativeTargetGroup> PlanTargetVersions(const ir::Program& program,
                                                  const NativeCallPlan& calls,
                                                  const FunctionDeclarations& declarations);

void EmitTargetVersions(llvm::Module& module,
                        std::span<const NativeTargetGroup> groups,
                        std::span<std::vector<std::string>> symbols);

}  // namespace vela::compiler::native
