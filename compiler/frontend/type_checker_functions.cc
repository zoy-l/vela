#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::FunctionType(const FunctionInfo& signature) {
  std::string name = "(";
  FunctionInfo positional;
  positional.return_type = signature.return_type;
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters.at(index).type;
    name += (index == 0 ? "" : ", ") + parameter.name;
    const auto key = std::to_string(index);
    positional.parameter_indices.emplace(key, static_cast<std::uint32_t>(index));
    positional.parameters.push_back({.name = key, .type = parameter});
  }
  name += ") => " + signature.return_type.name;
  Type result{.kind = Type::Kind::Function, .name = name};
  function_types_.try_emplace(name, std::move(positional));
  types_.try_emplace(name, result);
  return result;
}

void TypeChecker::CaptureLocal(const Expression& expression, const LocalBinding& binding) {
  bindings_.insert_or_assign(&expression, binding.id);
  for (const auto& context : lambda_scopes_) {
    if (binding.scope >= context.scope) {
      continue;
    }
    auto& captures = lambdas_.at(context.index).captures;
    if (std::ranges::none_of(captures, [&](const Capture& capture) -> bool {
          return capture.binding == binding.id;
        })) {
      captures.push_back({
          .name = expression.text,
          .type = binding.type,
          .binding = binding.id,
          .kind = binding.kind,
      });
    }
  }
}

TypeChecker::Type TypeChecker::CheckLambda(const Expression& expression, const Type* expected) {
  if (const auto found = lambda_indices_.find(&expression); found != lambda_indices_.end()) {
    return FunctionType(lambdas_.at(found->second).signature);
  }
  const auto& function = *expression.function;
  std::optional<FunctionInfo> context;
  if (expected != nullptr && expected->kind == Type::Kind::Function) {
    // 函数体检查会扩展 function_types_；持有签名副本，避免 flat_hash_map 重排使引用失效。
    context = function_types_.at(NonNullable(*expected).name);
    if (context->parameters.size() != function.parameters.size()) {
      AddDiagnostic(function.location,
                    "lambda parameter count does not match its function type context");
    }
  }
  auto signature = ResolveSignature(function, context ? &*context : nullptr);
  const auto index = lambdas_.size();
  lambda_indices_.emplace(&expression, index);
  lambdas_.push_back({.expression = &expression, .signature = signature, .captures = {}});
  lambda_scopes_.push_back({.index = index, .scope = scopes_.size()});
  auto previous_facts = std::move(refinements_);
  refinements_.clear();
  const auto previous_loop = std::exchange(loop_depth_, 0);
  const auto previous_switch = std::exchange(switch_depth_, 0);
  BeginScope();
  for (const auto& parameter : function.parameters) {
    if (DeclareLocal(parameter.name,
                     signature.parameters.at(signature.parameter_indices.at(parameter.name)).type,
                     parameter.location, BindingKind::Mutable, &parameter)) {
      bindings_.emplace(&parameter, LookupLocal(parameter.name)->id);
    }
  }
  const auto flow = CheckSequence(*function.body, signature.return_type);
  control_flow_.insert_or_assign(function.body.get(), flow);
  if (!IsValid(signature.return_type)) {
    signature.return_type = lambda_scopes_.back().inferred_return.value_or(
        Type{.kind = Type::Kind::Void, .name = "void"});
    if (signature.return_type.kind == Type::Kind::Null) {
      AddDiagnostic(function.location,
                    "null-only lambda requires an explicit nullable return type");
    }
  }
  if (signature.return_type.kind != Type::Kind::Void && flow.can_fall_through) {
    AddDiagnostic(function.location, "non-void lambda may fall through without returning");
  }
  EndScope();
  loop_depth_ = previous_loop;
  switch_depth_ = previous_switch;
  refinements_ = std::move(previous_facts);
  lambda_scopes_.pop_back();
  lambdas_.at(index).signature = signature;
  return FunctionType(signature);
}

void TypeChecker::InferLambdaReturn(const Statement& statement) {
  const auto value = statement.expression ? CheckExpression(*statement.expression, nullptr)
                                          : Type{.kind = Type::Kind::Void, .name = "void"};
  if (!IsValid(value)) {
    return;
  }
  auto& inferred = lambda_scopes_.back().inferred_return;
  if (!inferred || IsSameType(*inferred, value)) {
    inferred = value;
  } else if (value.kind == Type::Kind::Null && inferred->kind != Type::Kind::Void) {
    inferred = NullableType(*inferred);
  } else if ((inferred->kind == Type::Kind::Null && value.kind != Type::Kind::Void) ||
             IsSameType(NonNullable(*inferred), NonNullable(value))) {
    inferred = NullableType(value);
  } else {
    AddDiagnostic(statement.location,
                  "lambda return types do not match; add an explicit return type");
  }
}

}  // namespace vela::frontend
