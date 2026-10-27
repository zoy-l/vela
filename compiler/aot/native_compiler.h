#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

enum class NativeOptimization : std::uint8_t { None, Release };

struct NativeOptions final {
  std::string module_name = "vela_aot";
  NativeOptimization optimization = NativeOptimization::Release;
  std::vector<std::string> exports;
};

struct NativeExport final {
  std::string name;
  std::string symbol;
};

struct NativeArtifact final {
  std::vector<std::byte> object;
  std::string llvm_ir;
  std::string target;
  std::vector<NativeExport> exports;
};

[[nodiscard]] std::expected<NativeArtifact, frontend::Diagnostics> Compile(
    const ir::Program& program,
    const NativeOptions& options = {});

}  // namespace vela::compiler::aot
