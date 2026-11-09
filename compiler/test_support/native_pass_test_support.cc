#include "compiler/test_support/native_pass_test_support.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

namespace vela::compiler::native::test {

void RunFunctionPasses(llvm::Function& function, void (*add_passes)(llvm::FunctionPassManager&)) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(calls);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, calls, modules);
  llvm::FunctionPassManager passes;
  add_passes(passes);
  passes.run(function, functions);
}

}  // namespace vela::compiler::native::test
