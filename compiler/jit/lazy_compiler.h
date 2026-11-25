#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include "compiler/frontend/diagnostic.h"
#include "compiler/ir/ir.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/jit/compiler.h"

namespace vela::compiler::jit {

[[nodiscard]] std::expected<CompiledImage, frontend::Diagnostics> CompileOnDemand(
    const std::shared_ptr<CodeLinker>& jit,
    const std::shared_ptr<CodeResourceCounters>& counters,
    std::uint64_t batch,
    const std::shared_ptr<const ir::Program>& program,
    const CompileOptions& options,
    CodeReuseCache* reuse);

}  // namespace vela::compiler::jit
