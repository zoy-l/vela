#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_inference.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

void TypeChecker::DeclareFunctionParameters(const Function& function,
                                            const FunctionInfo& signature,
                                            BindingKind kind) {
  for (const auto& parameter : function.parameters) {
    const auto& type = signature.parameters.at(signature.parameter_indices.at(parameter.name)).type;
    if (parameter.initializer) {
      const auto& initializer = *parameter.initializer;
      const auto actual = CheckExpression(initializer, &type);
      RecordConversion(initializer, type, actual);
      if (IsValid(actual) && !CanAssign(type, actual)) {
        AddDiagnostic(initializer.location, "parameter default does not match its declared type");
      }
    }
    if (DeclareLocal(parameter.name, type, parameter.location, kind, &parameter) ==
            CheckStatus::Succeeded &&
        function.body) {
      parameter_usage_.Declare(parameter, LookupLocal(parameter.name)->id);
    }
  }
}

void TypeChecker::CheckGeneratedFunction(std::size_t index, const ClassInfo* access_owner) {
  const auto* function = facts_.generated_functions.at(index).declaration.get();
  // 检查函数体可能生成其它函数，不能跨 vector 扩容借用签名。
  const auto signature = facts_.generated_functions.at(index).signature;
  auto scopes = std::move(scopes_);
  auto refinements = std::move(refinements_);
  auto lambdas = std::move(lambda_scopes_);
  const auto* owner = std::exchange(owner_, access_owner);
  const auto constructor = std::exchange(in_constructor_, false);
  const auto asynchronous = std::exchange(in_async_function_, function->is_async);
  const auto loop = std::exchange(loop_depth_, 0);
  const auto switching = std::exchange(switch_depth_, 0);
  BeginScope();
  DeclareFunctionParameters(*function, signature, BindingKind::Constant);
  const auto flow = CheckBlock(*function->body, signature.return_type);
  facts_.control_flow.insert_or_assign(function->body.get(), flow);
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

TypeChecker::Type TypeChecker::CheckConstructorValue(const Expression& expression,
                                                     const ClassInfo& constructor,
                                                     const Type* expected) {
  const auto& declaration = *constructor.declaration;
  if (declaration.constructor_kind == ConstructorKind::Unavailable) {
    AddDiagnostic(expression.location, "class has no public constructor");
    return InvalidType();
  }
  CheckAccess(constructor.constructor.visibility, constructor.type.name, expression.location);
  auto signature = expected != nullptr && expected->kind == Type::Kind::Function
                       ? function_types_.at(NonNullable(*expected).name)
                       : constructor.constructor;
  if (!IsValid(signature.return_type)) {
    signature.return_type = constructor.type;
  }
  signature.optional_parameters.clear();
  const auto type = FunctionType(signature);
  const auto key =
      "$constructor.value." + std::string(constructor.type.name) + "." + std::string(type.name);
  if (const auto existing = function_value_ids_.find(key); existing != function_value_ids_.end()) {
    facts_.constructor_values.insert_or_assign(&expression, existing->second);
    return type;
  }
  const auto location = expression.location;
  auto function = std::make_unique<Function>();
  function->name = key;
  function->location = location;
  function->return_type = DescribeType(signature.return_type);
  function->body = std::make_unique<Statement>();
  function->body->kind = Statement::Kind::Block;
  function->body->location = location;
  auto returned = std::make_unique<Statement>();
  returned->kind = Statement::Kind::Return;
  returned->location = location;
  auto call = std::make_unique<Expression>();
  call->kind = Expression::Kind::Call;
  call->location = location;
  call->callee = std::make_unique<Expression>();
  call->callee->kind = Expression::Kind::Name;
  call->callee->text = constructor.type.name;
  call->callee->location = location;
  FunctionInfo implementation;
  implementation.return_type = signature.return_type;
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto name = "$argument" + std::to_string(index);
    const auto& parameter_type = signature.parameters[index].type;
    function->parameters.push_back({
        .type = DescribeType(parameter_type),
        .name = name,
        .location = location,
        .initializer = {},
    });
    implementation.AddParameter(name, parameter_type);
    auto argument = std::make_unique<Expression>();
    argument->kind = Expression::Kind::Name;
    argument->text = name;
    argument->location = location;
    call->arguments.push_back({
        .name = index < constructor.constructor.parameters.size()
                    ? constructor.constructor.parameters[index].name
                    : name,
        .location = location,
        .value = std::move(argument),
    });
  }
  returned->expression = std::move(call);
  function->body->statements.push_back(std::move(returned));
  const auto index = facts_.generated_functions.size();
  function_value_ids_.emplace(key, index);
  facts_.constructor_values.insert_or_assign(&expression, index);
  facts_.generated_functions.push_back(
      {.declaration = std::move(function), .signature = std::move(implementation)});
  CheckGeneratedFunction(index, owner_);
  return type;
}

TypeChecker::Type TypeChecker::FunctionType(const FunctionInfo& signature) {
  auto result = MakeFunctionType(signature);
  const auto [found, inserted] = function_types_.try_emplace(result.name);
  if (!inserted) {
    return result;
  }
  FunctionInfo positional;
  positional.return_type = signature.return_type;
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters.at(index).type;
    const auto key = std::to_string(index);
    positional.AddParameter(key, parameter);
  }
  found->second = std::move(positional);
  types_.try_emplace(result.name, result);
  return result;
}

TypeChecker::Type TypeChecker::FunctionValueType(const FunctionInfo& signature,
                                                 const Type* expected) {
  if (expected == nullptr || expected->kind != Type::Kind::Function) {
    return FunctionType(signature);
  }
  const auto& context = function_types_.at(NonNullable(*expected).name);
  return FunctionValueType(signature, context);
}

TypeChecker::Type TypeChecker::FunctionValueType(const FunctionInfo& signature,
                                                 const FunctionInfo& context) {
  const auto count = context.parameters.size();
  if (count > signature.parameters.size() ||
      (IsValid(context.return_type) && !IsSameType(context.return_type, signature.return_type))) {
    return FunctionType(signature);
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters[index];
    if (index < count ? !IsSameType(parameter.type, context.parameters[index].type)
                      : !parameter.has_default) {
      return FunctionType(signature);
    }
  }
  auto selected = context;
  selected.return_type = signature.return_type;
  return FunctionType(selected);
}

void TypeChecker::CaptureLocal(const Expression& expression, const LocalBinding& binding) {
  facts_.bindings.insert_or_assign(&expression, binding.id);
  parameter_usage_.Read(expression, binding.id);
  for (auto& context : lambda_scopes_) {
    if (binding.scope >= context.scope) {
      continue;
    }
    auto& captures = facts_.lambdas.at(context.index).captures;
    if (context.captures.insert(binding.id).second) {
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
  if (const auto found = facts_.lambda_indices.find(&expression);
      found != facts_.lambda_indices.end()) {
    return FunctionValueType(facts_.lambdas.at(found->second).signature, expected);
  }
  const auto& function = *expression.function;
  const bool previous_async = in_async_function_;
  in_async_function_ = function.is_async;
  std::optional<FunctionInfo> context;
  if (expected != nullptr && expected->kind == Type::Kind::Function) {
    context = function_types_.at(NonNullable(*expected).name);
    const auto count = context->parameters.size();
    const auto can_omit =
        count <= function.parameters.size() &&
        std::ranges::all_of(std::span(function.parameters).subspan(count),
                            [](const auto& parameter) { return parameter.initializer != nullptr; });
    if (count != function.parameters.size() && !can_omit) {
      AddDiagnostic(function.location,
                    "lambda parameter count does not match its function type context");
    }
  }
  auto signature = ResolveSignature(function, context ? &*context : nullptr);
  if (function.is_async && signature.return_type.kind != Type::Kind::Future) {
    AddDiagnostic(function.location, "async lambda return type must be Future<T>");
  }
  const auto index = facts_.lambdas.size();
  facts_.lambda_indices.emplace(&expression, index);
  facts_.lambdas.push_back({.expression = &expression, .signature = signature, .captures = {}});
  lambda_scopes_.push_back({.index = index, .scope = scopes_.size(), .captures = {}});
  auto previous_facts = std::move(refinements_);
  refinements_.Clear();
  const auto previous_loop = std::exchange(loop_depth_, 0);
  const auto previous_switch = std::exchange(switch_depth_, 0);
  BeginScope();
  DeclareFunctionParameters(function, signature, BindingKind::Mutable);
  const auto flow = CheckSequence(*function.body, signature.return_type);
  facts_.control_flow.insert_or_assign(function.body.get(), flow);
  if (!IsValid(signature.return_type)) {
    signature.return_type = lambda_scopes_.back().inferred_return.value_or(
        Type{.kind = Type::Kind::Void, .name = kVoidTypeName});
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
  facts_.lambdas.at(index).signature = signature;
  in_async_function_ = previous_async;
  return context ? FunctionValueType(signature, *context) : FunctionType(signature);
}

void TypeChecker::InferLambdaReturn(const Statement& statement) {
  const auto value = statement.expression ? CheckExpression(*statement.expression, nullptr)
                                          : Type{.kind = Type::Kind::Void, .name = kVoidTypeName};
  if (!IsValid(value)) {
    return;
  }
  auto& inferred = lambda_scopes_.back().inferred_return;
  if (!inferred) {
    inferred = value;
  } else if (const auto common =
                 TypeInference::CommonType(DescribeType(*inferred), DescribeType(value))) {
    inferred = ResolveType(*common, VoidTypePolicy::Allowed);
  } else {
    AddDiagnostic(statement.location,
                  "lambda return types do not match; add an explicit return type");
  }
}

}  // namespace vela::frontend
