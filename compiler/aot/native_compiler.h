#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::aot {

struct NativeOptions final {
  std::string module_name = "vela_aot";
};

struct NativeArtifact final {
  std::vector<std::byte> object;
  std::string llvm_ir;
  std::string target;
};

[[nodiscard]] std::expected<NativeArtifact, frontend::Diagnostics> Compile(
    const ir::Program& program,
    NativeOptions options = {});

}  // namespace vela::compiler::aot
