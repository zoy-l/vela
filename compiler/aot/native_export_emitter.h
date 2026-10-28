#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "compiler/aot/native_compiler.h"
#include "compiler/ir/ir.h"

namespace llvm {
class Function;
class Module;
}

namespace vela::compiler::aot {

struct NativeExportInterface final {
  std::string header;
  std::string instantiate_symbol;
};

[[nodiscard]] std::expected<NativeExportInterface, std::string> EmitNativeExports(
    llvm::Module& module,
    const ir::Program& program,
    std::span<const NativeExport> exports,
    llvm::Function& image_entry,
    std::string_view module_name);

}  // namespace vela::compiler::aot
