#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_inference.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

TypeChecker::FunctionContext::FunctionContext(TypeChecker& checker)
    : checker_(&checker),
      scopes_(std::exchange(checker.scopes_, {})),
      refinements_(std::exchange(checker.refinements_, {})),
      lambdas_(std::exchange(checker.lambda_scopes_, {})),
      constants_(std::exchange(checker.constant_bindings_, {})),
      predicate_(std::exchange(checker.predicate_scope_, std::nullopt)),
      owner_(std::exchange(checker.owner_, nullptr)),
      constructor_(std::exchange(checker.in_constructor_, false)),
      asynchronous_(std::exchange(checker.in_async_function_, false)),
      loop_(std::exchange(checker.loop_depth_, 0)),
      switching_(std::exchange(checker.switch_depth_, 0)) {}

TypeChecker::FunctionContext::~FunctionContext() {
  auto& checker = *checker_;
  checker.scopes_ = std::move(scopes_);
  checker.refinements_ = std::move(refinements_);
  checker.lambda_scopes_ = std::move(lambdas_);
  checker.constant_bindings_ = std::move(constants_);
  checker.predicate_scope_ = predicate_;
  checker.owner_ = owner_;
  checker.in_constructor_ = constructor_;
  checker.in_async_function_ = asynchronous_;
  checker.loop_depth_ = loop_;
  checker.switch_depth_ = switching_;
}

void TypeChecker::DeclareFunctionParameters(const Function& function,
                                            const FunctionInfo& signature,
                                            BindingKind kind) {
  for (const auto& parameter : function.parameters) {
    if (parameter.binding != Parameter::Binding::Local) {
      continue;
    }
    const auto& type = signature.parameters.at(signature.parameter_indices.at(parameter.name)).type;
    if (parameter.initializer) {
      const auto& initializer = *parameter.initializer;
      const auto actual = CheckExpression(initializer, &type);
      RecordConversion(initializer, type, actual);
      if (IsValid(actual) && !CanAssign(type, actual)) {
        AddDiagnostic(initializer.location, "parameter default does not match its declared type");
      }
    }
    if (const auto* binding =
            DeclareLocal(parameter.name, type, parameter.location, kind, &parameter);
        binding != nullptr && function.body) {
      parameter_usage_.Declare(parameter, binding->id);
    }
  }
}

void TypeChecker::CheckGeneratedFunction(std::size_t index, const ClassInfo* access_owner) {
  const auto* function = facts_.generated_functions.at(index).declaration.get();
  // 检查函数体可能生成其它函数，不能跨 vector 扩容借用签名。
  const auto signature = facts_.generated_functions.at(index).signature;
  const FunctionContext context(*this);
  owner_ = access_owner;
  in_async_function_ = function->is_async;
  BeginScope();
  DeclareFunctionParameters(*function, signature, BindingKind::Constant);
  const auto flow = CheckBlock(*function->body, signature.return_type);
  facts_.control_flow.insert_or_assign(function->body.get(), flow);
  EndScope();
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
        .member_name = {},
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
  return FunctionType(signature.parameters, signature.return_type, signature.narrowed_parameter,
                      signature.false_narrowed_parameter);
}

TypeChecker::Type TypeChecker::FunctionType(std::span<const FunctionInfo::Parameter> parameters,
                                            const Type& return_type,
                                            const std::optional<Type>& narrowed_parameter,
                                            const std::optional<Type>& false_narrowed_parameter) {
  auto result =
      MakeFunctionType(parameters, return_type, narrowed_parameter, false_narrowed_parameter);
  const auto [found, inserted] = function_types_.try_emplace(result.name);
  if (!inserted) {
    return result;
  }
  FunctionInfo positional;
  positional.return_type = return_type;
  positional.narrowed_parameter = narrowed_parameter;
  positional.false_narrowed_parameter = false_narrowed_parameter;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& parameter = parameters[index].type;
    const auto key = std::to_string(index);
    positional.AddParameter(key, parameter);
  }
  found->second = std::move(positional);
  types_.try_emplace(result.name, result);
  return result;
}

TypeChecker::Type TypeChecker::FunctionValueType(const FunctionInfo& signature,
                                                 const Type* expected) {
  const auto narrowed = ResolvePredicate(signature, ConditionTruth::AlwaysTrue);
  const auto false_narrowed = ResolvePredicate(signature, ConditionTruth::AlwaysFalse);
  if (expected == nullptr || expected->kind != Type::Kind::Function) {
    return FunctionType(signature.parameters, signature.return_type, narrowed, false_narrowed);
  }
  return FunctionValueType(signature, function_types_.at(NonNullable(*expected).name), narrowed,
                           false_narrowed);
}

TypeChecker::Type TypeChecker::FunctionValueType(
    const FunctionInfo& signature,
    const FunctionInfo& context,
    const std::optional<Type>& narrowed_parameter,
    const std::optional<Type>& false_narrowed_parameter) {
  const auto count = context.parameters.size();
  if (count > signature.parameters.size() ||
      (IsValid(context.return_type) && signature.return_type.kind != Type::Kind::Never &&
       !IsSameType(context.return_type, signature.return_type))) {
    return FunctionType(signature.parameters, signature.return_type, narrowed_parameter,
                        false_narrowed_parameter);
  }
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters[index];
    if (index < count ? !IsSameType(parameter.type, context.parameters[index].type)
                      : !signature.optional_parameters.contains(parameter.name)) {
      return FunctionType(signature.parameters, signature.return_type, narrowed_parameter,
                          false_narrowed_parameter);
    }
  }
  return FunctionType(context.parameters, signature.return_type, narrowed_parameter,
                      false_narrowed_parameter);
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
  const auto previous_predicate = std::exchange(predicate_scope_, std::nullopt);
  refinements_.Clear();
  const auto previous_loop = std::exchange(loop_depth_, 0);
  const auto previous_switch = std::exchange(switch_depth_, 0);
  BeginScope();
  DeclareFunctionParameters(function, signature, BindingKind::Mutable);
  checking_function_bodies_.insert(function.body.get());
  BeginPredicate(function, signature);
  const auto flow = CheckSequence(*function.body, signature.return_type);
  facts_.control_flow.insert_or_assign(function.body.get(), flow);
  if (!IsValid(signature.return_type)) {
    signature.return_type = lambda_scopes_.back().inferred_return.value_or(
        flow.can_fall_through ? Type{.kind = Type::Kind::Void, .name = kVoidTypeName}
                              : Type{.kind = Type::Kind::Never, .name = kNeverTypeName});
  }
  if (!CanFallThrough(function, signature.return_type) && flow.can_fall_through) {
    AddDiagnostic(function.location, "non-void lambda may fall through without returning");
  }
  auto predicate = FinishPredicate();
  signature.narrowed_parameter = predicate.matched;
  signature.false_narrowed_parameter = predicate.unmatched;
  if (signature.return_type.kind != Type::Kind::Bool || signature.return_type.nullable) {
    signature.narrowed_parameter.reset();
    signature.false_narrowed_parameter.reset();
  }
  function_predicates_.insert_or_assign(function.body.get(), std::move(predicate));
  checking_function_bodies_.erase(function.body.get());
  EndScope();
  loop_depth_ = previous_loop;
  switch_depth_ = previous_switch;
  refinements_ = std::move(previous_facts);
  predicate_scope_ = previous_predicate;
  lambda_scopes_.pop_back();
  facts_.lambdas.at(index).signature = signature;
  in_async_function_ = previous_async;
  return context ? FunctionValueType(signature, *context, signature.narrowed_parameter,
                                     signature.false_narrowed_parameter)
                 : FunctionType(signature);
}

void TypeChecker::InferLambdaReturn(const Statement& statement) {
  const auto value = statement.expression ? CheckExpression(*statement.expression, nullptr)
                                          : Type{.kind = Type::Kind::Void, .name = kVoidTypeName};
  if (!IsValid(value)) {
    return;
  }
  if (statement.expression && value.kind == Type::Kind::Bool && !value.nullable) {
    RecordPredicateReturn(*statement.expression);
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
