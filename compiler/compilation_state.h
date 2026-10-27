#pragma once

#include <memory>
#include "compiler/frontend/symbol_ids.h"
#include "compiler/ir/persistent_ids.h"
#include "runtime/program_layouts.h"
#include "vm/program.h"

namespace vela::compiler {

struct CompilationState final {
  frontend::SymbolIds interfaces;
  ir::PersistentIds ids;
  runtime::LayoutCache layouts;
  std::shared_ptr<vm::GlobalStorage> globals;
};

}  // namespace vela::compiler
