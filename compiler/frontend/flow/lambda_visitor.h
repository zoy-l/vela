#pragma once

#include <absl/functional/function_ref.h>

#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

using LambdaVisitor = absl::FunctionRef<void(const Expression&)>;

// 按词法前序访问全部匿名函数。
void VisitLambdas(const Expression& expression, LambdaVisitor visitor);
void VisitLambdas(const Statement& statement, LambdaVisitor visitor);

}  // namespace vela::frontend
