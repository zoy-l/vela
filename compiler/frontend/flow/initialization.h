#pragma once

#include <absl/container/flat_hash_map.h>
#include <cstddef>
#include <span>
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_checker.h"

namespace vela::frontend {

struct ConstructorInitializationFacts final {
  const ClassDescriptor* owner{};
  const ClassDescriptor* base = nullptr;
  std::span<const LambdaDescriptor> lambdas;
  const absl::flat_hash_map<const Expression*, std::size_t>* lambda_indices{};
  const absl::flat_hash_map<const Statement*, StatementFlow>* control_flow{};
  bool has_explicit_super_call = false;
};

[[nodiscard]] Diagnostics CheckConstructorInitialization(
    const Class& owner,
    const ConstructorInitializationFacts& facts);

}  // namespace vela::frontend
