#pragma once

#include "compiler/frontend/symbol_ids.h"
#include "compiler/ir/persistent_ids.h"
#include "compiler/runtime_layouts.h"

namespace vela::compiler {

struct CompilationState final {
  frontend::SymbolIds interfaces;
  ir::PersistentIds ids;
  LayoutCache layouts;
};

}  // namespace vela::compiler
