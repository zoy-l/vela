#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

void TypeChecker::CheckGeneratedFunction(std::size_t index) {
  const auto* function = generated_functions_.at(index).declaration.get();
  // 检查函数体可能生成其它函数，不能跨 vector 扩容借用签名。
  const auto signature = generated_functions_.at(index).signature;
  auto scopes = std::move(scopes_);
  auto refinements = std::move(refinements_);
  auto lambdas = std::move(lambda_scopes_);
  const auto* owner = std::exchange(owner_, nullptr);
  const auto constructor = std::exchange(in_constructor_, false);
  const auto asynchronous = std::exchange(in_async_function_, function->is_async);
  const auto loop = std::exchange(loop_depth_, 0);
  const auto switching = std::exchange(switch_depth_, 0);
  BeginScope();
  for (std::size_t parameter = 0; parameter < function->parameters.size(); ++parameter) {
    const auto& declaration = function->parameters.at(parameter);
    DeclareLocal(declaration.name, signature.parameters.at(parameter).type, declaration.location,
                 BindingKind::Constant, &declaration);
  }
  const auto flow = CheckBlock(*function->body, signature.return_type);
  control_flow_.insert_or_assign(function->body.get(), flow);
  EndScope();
  scopes_ = std::move(scopes);
  refinements_ = std::move(refinements);
  lambda_scopes_ = std::move(lambdas);
  owner_ = owner;
  in_constructor_ = constructor;
  in_async_function_ = asynchronous;
  loop_depth_ = loop;
  switch_depth_ = switching;
}

TypeDescriptor MakeFunctionType(const FunctionDescriptor& signature) {
  std::string name = "(";
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    name += (index == 0 ? "" : ", ") + signature.parameters.at(index).type.name;
  }
  name += ") => " + signature.return_type.name;
  return {.kind = TypeDescriptor::Kind::Function, .name = std::move(name)};
}

TypeChecker::Type TypeChecker::FunctionType(const FunctionInfo& signature) {
  auto result = MakeFunctionType(signature);
  FunctionInfo positional;
  positional.return_type = signature.return_type;
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters.at(index).type;
    const auto key = std::to_string(index);
    positional.parameter_indices.emplace(key, static_cast<std::uint32_t>(index));
    positional.parameters.push_back({.name = key, .type = parameter});
  }
  function_types_.try_emplace(result.name, std::move(positional));
  types_.try_emplace(result.name, result);
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
  const bool previous_async = in_async_function_;
  in_async_function_ = function.is_async;
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
  if (function.is_async && signature.return_type.kind != Type::Kind::Future) {
    AddDiagnostic(function.location, "async lambda return type must be Future<T>");
  }
  const auto index = lambdas_.size();
  lambda_indices_.emplace(&expression, index);
  lambdas_.push_back({.expression = &expression, .signature = signature, .captures = {}});
  lambda_scopes_.push_back({.index = index, .scope = scopes_.size()});
  auto previous_facts = std::move(refinements_);
  refinements_.Clear();
  const auto previous_loop = std::exchange(loop_depth_, 0);
  const auto previous_switch = std::exchange(switch_depth_, 0);
  BeginScope();
  for (const auto& parameter : function.parameters) {
    DeclareLocal(parameter.name,
                 signature.parameters.at(signature.parameter_indices.at(parameter.name)).type,
                 parameter.location, BindingKind::Mutable, &parameter);
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
  if (!CanFallThrough(function, signature.return_type) && flow.can_fall_through) {
    AddDiagnostic(function.location, "non-void lambda may fall through without returning");
  }
  EndScope();
  loop_depth_ = previous_loop;
  switch_depth_ = previous_switch;
  refinements_ = std::move(previous_facts);
  lambda_scopes_.pop_back();
  lambdas_.at(index).signature = signature;
  in_async_function_ = previous_async;
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
