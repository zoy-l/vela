#include "compiler/test_support/native_pass_test_support.h"

#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>

namespace vela::compiler::native::test {

std::unique_ptr<llvm::Module> ParseModule(std::string_view text, llvm::LLVMContext& context) {
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(text, diagnostic, context);
  if (module == nullptr) {
    ADD_FAILURE() << diagnostic.getMessage().str();
  }
  return module;
}

void WithFunctionLoops(llvm::Module& module,
                       llvm::function_ref<void(llvm::Function&, const llvm::LoopInfo&)> verify) {
  for (auto& function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    llvm::DominatorTree dominators(function);
    const llvm::LoopInfo loops(dominators);
    verify(function, loops);
  }
}

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
