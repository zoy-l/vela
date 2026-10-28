#include "compiler/native_linker.h"

#include <span>
#include <string>
#include <utility>

#include "base/layout_description.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "runtime/native_function.h"

namespace vela::compiler {

frontend::Diagnostics CheckNativeBindings(const ir::Program& program,
                                          const runtime::Isolate& isolate) {
  frontend::Diagnostics diagnostics;
  for (const auto& function : program.functions) {
    if (function.native_symbol.empty()) {
      continue;
    }
    const auto binding = isolate.FindNative(function.native_symbol);
    const auto& signature = function.signature;
    if (!signature) {
      std::unreachable();
    }
    std::string message;
    if (binding == nullptr) {
      message = "unregistered native function '" + function.native_symbol + "'";
    } else if (!binding->IsSignatureCompatible(*signature, program.layouts.types)) {
      message = "external declaration does not match native signature for '" +
                function.native_symbol + "'";
    } else {
      continue;
    }
    diagnostics.push_back({
        .location = function.location,
        .message = std::move(message),
        .source_path = program.source_paths.empty()
                           ? std::string()
                           : program.source_paths.at(function.location.source_id),
    });
  }
  return diagnostics;
}

}  // namespace vela::compiler
