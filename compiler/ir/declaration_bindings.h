#pragma once

#include <cstdint>
#include <string>

#include <absl/container/flat_hash_map.h>

#include "base/declaration.h"
#include "compiler/frontend/modules/declaration_ids.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"

namespace vela::ir {

struct ClassDeclarations final {
  DeclarationId declaration;
  absl::flat_hash_map<std::string, DeclarationId> fields;
};

// 只在准备阶段借用 AST；Lowerer 将身份值移入拥有源码和布局的 IR。
struct DeclarationBindings final {
  std::uint64_t revision = 0;
  absl::flat_hash_map<const frontend::Function*, FunctionIdentity> functions;
  absl::flat_hash_map<std::string, DeclarationId> named_functions;
  absl::flat_hash_map<std::string, ClassDeclarations> classes;
  absl::flat_hash_map<std::string, DeclarationId> nominals;
  absl::flat_hash_map<std::string, DeclarationId> globals;
  absl::flat_hash_map<std::string, DeclarationId> selectors;
};

[[nodiscard]] DeclarationBindings BindDeclarations(const frontend::Program& source,
                                                   const frontend::SemanticModel& model,
                                                   frontend::DeclarationIds& identities,
                                                   std::uint64_t revision);

}  // namespace vela::ir
