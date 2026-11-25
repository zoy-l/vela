#include "compiler/development/development_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "runtime/isolate/isolate.h"

#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace vela::compiler {

std::expected<std::unique_ptr<DevelopmentSession>, frontend::Diagnostics>
DevelopmentSession::Create(std::string entry,
                           runtime::NativeBindings bindings,
                           DevelopmentOptions options) {
  auto compiler = jit::Compiler::Create();
  if (!compiler) {
    return std::unexpected(std::move(compiler.error()));
  }
  return Create(std::move(entry), std::move(bindings), std::move(options), std::move(*compiler));
}

}  // namespace vela::compiler
