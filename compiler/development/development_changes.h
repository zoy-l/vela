#pragma once

#include <cstddef>
#include <vector>

#include "base/declaration.h"

namespace vela::compiler {

struct DevelopmentChanges final {
  std::size_t compiled_functions = 0;
  std::vector<DeclarationId> registered_types;
  std::vector<DeclarationId> withdrawn_types;
  std::vector<DeclarationId> registered_members;
  std::vector<DeclarationId> withdrawn_members;
  std::vector<DeclarationId> changed_members;
  std::vector<DeclarationId> deleted_types;
  std::vector<DeclarationId> deleted_members;
};

}  // namespace vela::compiler
