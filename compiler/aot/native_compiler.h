#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "compiler/aot/native_roots.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

enum class NativeOptimization : std::uint8_t { None, Release };
enum class NativeEntryMode : std::uint8_t { Library, Executable };

struct NativeOptions final {
  std::string module_name = "vela_aot";
  NativeOptimization optimization = NativeOptimization::Release;
  std::vector<std::string> exports;
  NativeEntryMode entry_mode = NativeEntryMode::Library;
};

struct NativeExport final {
  std::string name;
  std::string symbol;
  std::uint32_t function = 0;
};

struct NativeArtifact final {
  std::vector<std::byte> object;
  std::string llvm_ir;
  std::string target;
  std::vector<NativeExport> exports;
  std::string image_symbol;
  std::string header;
  std::string instantiate_symbol;
  NativeOptimization optimization = NativeOptimization::Release;
  ir::Program program;
  std::vector<NativeFunctionMetrics> function_metrics;
};

[[nodiscard]] std::string NativeToolchainBuildIdentity(NativeOptimization optimization);

[[nodiscard]] std::expected<NativeArtifact, frontend::Diagnostics> Compile(
    ir::Program program,
    const NativeOptions& options = {});

}  // namespace vela::compiler::aot
