#pragma once

#include "compiler/frontend/modules/symbol_ids.h"

namespace vela::ir {

struct PersistentIds final {
  frontend::SymbolIds functions;
  frontend::SymbolIds globals;
  frontend::SymbolIds selectors;
};

}  // namespace vela::ir
