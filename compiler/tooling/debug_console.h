#pragma once

#include <iosfwd>

#include "runtime/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::tooling {

[[nodiscard]] auto RunDebugConsole(const vm::FunctionHandle& function,
                                   runtime::Isolate& isolate,
                                   std::istream& input,
                                   std::ostream& output) -> vm::ExecutionResult;

}  // namespace vela::tooling
