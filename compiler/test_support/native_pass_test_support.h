#pragma once

#include <memory>
#include <string_view>

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/IR/PassManager.h>

namespace llvm {
class LLVMContext;
class LoopInfo;
class Module;
}  // namespace llvm

namespace vela::compiler::native::test {

std::unique_ptr<llvm::Module> ParseModule(std::string_view text, llvm::LLVMContext& context);
void WithFunctionLoops(llvm::Module& module,
                       llvm::function_ref<void(llvm::Function&, const llvm::LoopInfo&)> verify);
void RunFunctionPasses(llvm::Function& function, void (*add_passes)(llvm::FunctionPassManager&));

}  // namespace vela::compiler::native::test
