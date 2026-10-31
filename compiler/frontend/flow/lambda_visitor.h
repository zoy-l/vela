#pragma once

#include <absl/functional/function_ref.h>

#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

using LambdaVisitor = absl::FunctionRef<void(Expression&, const Function*)>;
using ConstLambdaVisitor = absl::FunctionRef<void(const Expression&, const Function*)>;

// 按词法前序访问全部匿名函数；parent 只表示遍历范围内的直接外层匿名函数。
void VisitLambdas(Expression& expression, LambdaVisitor visitor);
void VisitLambdas(Statement& statement, LambdaVisitor visitor);
void VisitLambdas(const Expression& expression, ConstLambdaVisitor visitor);
void VisitLambdas(const Statement& statement, ConstLambdaVisitor visitor);

}  // namespace vela::frontend
