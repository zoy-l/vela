#pragma once

#include <llvm/Support/Error.h>

namespace vela::compiler::jit {
class CodeLinker;

[[nodiscard]] llvm::Error DefineRuntimeSymbols(CodeLinker& jit);

}  // namespace vela::compiler::jit
