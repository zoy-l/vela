#pragma once

#include <llvm/Support/Error.h>

namespace llvm::orc {
class LLJIT;
}

namespace vela::compiler::jit {

[[nodiscard]] llvm::Error DefineRuntimeSymbols(llvm::orc::LLJIT& jit);

}  // namespace vela::compiler::jit
