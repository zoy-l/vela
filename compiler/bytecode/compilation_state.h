#pragma once

#include <cstdint>
#include <memory>
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/modules/symbol_ids.h"
#include "compiler/ir/persistent_ids.h"
#include "runtime/isolate/program_layouts.h"
#include "vm/program.h"

namespace vela::compiler {

struct CompilationState final {
  frontend::SymbolIds interfaces;
  ir::PersistentIds ids;
  runtime::LayoutCache layouts;
  std::shared_ptr<vm::GlobalStorage> globals;
  frontend::DeclarationIds declarations;
  std::uint64_t revision = 0;
};

}  // namespace vela::compiler
