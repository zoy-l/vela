#include "compiler/test_support/native_pass_test_support.h"

#include <algorithm>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
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

void WithFunctionLoops(llvm::Function& function,
                       llvm::function_ref<void(llvm::Function&, const llvm::LoopInfo&)> verify) {
  llvm::DominatorTree dominators(function);
  const llvm::LoopInfo loops(dominators);
  verify(function, loops);
}

void WithFunctionLoops(llvm::Module& module,
                       llvm::function_ref<void(llvm::Function&, const llvm::LoopInfo&)> verify) {
  for (auto& function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    WithFunctionLoops(function, verify);
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

unsigned CountLoopsCalling(llvm::Module& module, std::string_view symbol) {
  unsigned count = 0;
  WithFunctionLoops(module, [&](llvm::Function&, const llvm::LoopInfo& loops) {
    for (const auto* loop : loops) {
      const bool polls = std::ranges::any_of(loop->blocks(), [symbol](const auto* block) {
        return std::ranges::any_of(*block, [symbol](const auto& instruction) {
          const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
          const auto* callee = call != nullptr ? call->getCalledFunction() : nullptr;
          return callee != nullptr &&
                 callee->getName() == llvm::StringRef(symbol.data(), symbol.size());
        });
      });
      count += static_cast<unsigned>(polls);
    }
  });
  return count;
}

}  // namespace vela::compiler::native::test
