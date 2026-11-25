#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Support/MemoryBuffer.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_inputs.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"
#include "compiler/native/function_symbols.h"
#include "compiler/native/native_image_data.h"

namespace vela::compiler::jit {

inline constexpr std::string_view kFunctionEntrySuffix = "_lazy";

// 编译服务只产生可重定位的代码；加载端独占地址、代码退休及运行状态。
class FunctionCompiler {
 public:
  FunctionCompiler() = default;
  FunctionCompiler(const FunctionCompiler&) = delete;
  FunctionCompiler& operator=(const FunctionCompiler&) = delete;
  FunctionCompiler(FunctionCompiler&&) = delete;
  FunctionCompiler& operator=(FunctionCompiler&&) = delete;
  virtual ~FunctionCompiler() = default;
  virtual std::expected<std::unique_ptr<llvm::MemoryBuffer>, std::string> Compile(
      std::size_t function) = 0;
  std::vector<native::FunctionEntrySymbols> symbols;
  std::vector<FunctionCodeInputs> inputs;
};

std::expected<std::shared_ptr<FunctionCompiler>, frontend::Diagnostics> PrepareFunctions(
    std::shared_ptr<const ir::Program> program,
    const CompileOptions& options);

std::expected<CompiledImage, frontend::Diagnostics> LinkFunctions(
    const std::shared_ptr<CodeLinker>& jit,
    const std::shared_ptr<CodeResourceCounters>& counters,
    std::uint64_t batch,
    const std::shared_ptr<native::NativeImageData>& data,
    const CompileOptions& options,
    CodeReuseCache* reuse,
    std::shared_ptr<FunctionCompiler> functions);

}  // namespace vela::compiler::jit
