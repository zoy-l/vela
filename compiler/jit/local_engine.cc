#include <llvm/IR/DataLayout.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <utility>
#include "compiler/jit/engine.h"

#include "compiler/native/code_generator.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {

llvm::Expected<std::unique_ptr<CodeLinker>> CreateEngine() {
  auto layout = native::TargetDataLayout();
  if (!layout) {
    return llvm::make_error<llvm::StringError>(std::move(layout.error()),
                                               llvm::inconvertibleErrorCode());
  }
  const auto identity = runtime::NativeTargetIdentity();
  return CreateEngine(llvm::Triple(llvm::StringRef(identity.data(), identity.size())),
                      llvm::DataLayout(*layout));
}

}  // namespace vela::compiler::jit
