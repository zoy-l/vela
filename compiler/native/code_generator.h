#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/native/escape_analysis.h"
#include "compiler/native/native_roots.h"

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
}  // namespace llvm

namespace vela::compiler {

enum class NativeOptimization : std::uint8_t { None, Release };
enum class NativeCodePlacement : std::uint8_t { Linked, Dynamic };

namespace native {

void InitializeTarget();

// context 最后释放；AOT 目标文件与 ORC 交付共用同一份完整 LLVM 语义 lowering。
struct NativeModule final {
  NativeModule();
  ~NativeModule();
  NativeModule(NativeModule&&) noexcept;
  NativeModule& operator=(NativeModule&&) = delete;
  NativeModule(const NativeModule&) = delete;
  NativeModule& operator=(const NativeModule&) = delete;
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::TargetMachine> machine;
  std::unique_ptr<llvm::Module> module;
  std::string image_symbol;
  std::vector<NativeFunctionMetrics> function_metrics;
};

[[nodiscard]] std::expected<NativeModule, frontend::Diagnostics> Generate(
    const ir::Program& program,
    const std::string& module_name,
    std::span<const std::uint8_t> exports,
    NativeDebugging debugging = NativeDebugging::Safepoints,
    NativeCodeTier tier = NativeCodeTier::Baseline,
    EscapeOptimization escape = EscapeOptimization::Enabled,
    NativeCodePlacement placement = NativeCodePlacement::Linked);

// 调用者可在优化前添加 AOT 的公开包装和 main；JIT 直接交付映像入口。
[[nodiscard]] std::expected<void, std::string> Finalize(NativeModule& generated,
                                                        NativeOptimization optimization);
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> EmitObject(
    NativeModule& generated);

}  // namespace native
}  // namespace vela::compiler
