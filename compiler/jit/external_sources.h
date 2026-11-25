#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include "compiler/development/development_files.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler_process.h"

namespace vela::compiler::jit {

std::expected<std::unique_ptr<DevelopmentFiles>, frontend::Diagnostics> CreateExternalFiles(
    std::shared_ptr<CompilerProcess> process,
    std::uint64_t resource_id,
    const std::string& entry,
    const frontend::ModulePaths& imports);

std::expected<std::shared_ptr<SourceSession>, frontend::Diagnostics> CreateExternalSources(
    std::shared_ptr<CompilerProcess> process,
    std::uint64_t resource_id,
    const SourceSessionConfiguration& configuration);

}  // namespace vela::compiler::jit
