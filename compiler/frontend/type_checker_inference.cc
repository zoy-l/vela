#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/capture_analysis.h"
#include "compiler/frontend/type_inference.h"

namespace vela::frontend {

void TypeChecker::CheckPendingSpecializations() {
  // 新特化只排入工作队列，函数体按声明作用域检查，不递归借用调用方局部变量。
  std::size_t checked_functions = 0;
  std::size_t checked_classes = 0;
  while (checked_functions < specializer_->function_count() ||
         checked_classes < specializer_->class_count()) {
    while (checked_functions < specializer_->function_count()) {
      const auto& function = specializer_->function(checked_functions++);
      specializer_->SetExpansionOwner(function.name);
      CheckFunction(function, nullptr);
    }
    while (checked_classes < specializer_->class_count()) {
      const auto& source = specializer_->class_declaration(checked_classes++);
      specializer_->SetExpansionOwner(source.name);
      CheckClassBody(source);
    }
  }
}

TypeChecker::Type TypeChecker::ResolveClassInstantiation(const TypeName& type_name) {
  std::vector<TypeName> arguments;
  for (const auto& argument : type_name.arguments) {
    const auto type = ResolveType(argument, true);
    if (!IsValid(type)) {
      return InvalidType();
    }
    arguments.push_back(DescribeType(type));
  }
  const auto name = specializer_->Specialize(type_name.name, arguments, type_name.location);
  RegisterSpecializations();
  const auto found = types_.find(name);
  return found == types_.end() ? InvalidType() : found->second;
}

void TypeChecker::SetConstructorParameters(const Class& owner, InferenceSignature& source) {
  if (owner.constructor_kind == ConstructorKind::Explicit) {
    source.parameters = owner.constructor->parameters;
  } else {
    for (const auto& field : owner.fields) {
      if (owner.IsConstructorField(field)) {
        source.parameters.push_back(
            {.type = field.type, .name = field.name, .location = field.location});
        if (field.initializer) {
          source.optional_parameters.insert(field.name);
        }
      }
    }
  }
}

void TypeChecker::RegisterSpecializations() {
  for (auto& diagnostic : specializer_->TakeDiagnostics()) {
    diagnostics_.push_back(std::move(diagnostic));
  }
  const auto class_begin = registered_classes_;
  const auto class_end = specializer_->class_count();
  registered_classes_ = class_end;
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    const Type type{.kind = Type::Kind::Class, .name = source.name};
    types_.try_emplace(source.name, type);
    ClassInfo info;
    info.type = type;
    classes_.try_emplace(source.name, std::move(info));
  }
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    ResolveClass(source, classes_.at(source.name));
    const auto captured = FindCapturedMutableDeclarations(source);
    captured_declarations_.insert(captured.begin(), captured.end());
  }
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    if (source.base_class) {
      MergeBaseClass(source.name, merged_classes_, visiting_classes_);
    }
    CheckClassImplementation(source);
  }
  while (registered_functions_ < specializer_->function_count()) {
    const auto& function = specializer_->function(registered_functions_++);
    functions_.try_emplace(function.name, ResolveSignature(function));
    const auto captured = FindCapturedMutableDeclarations(function);
    captured_declarations_.insert(captured.begin(), captured.end());
  }
}

void TypeChecker::CheckClassBody(const Class& source) {
  const auto found = classes_.find(source.name);
  if (found == classes_.end()) {
    return;
  }
  const auto& info = found->second;
  CheckFieldInitializers(source, info);
  for (const auto& method : source.methods) {
    CheckFunction(method, &info);
  }
  if (source.constructor) {
    CheckFunction(*source.constructor, &info);
  }
}

TypeName TypeChecker::DescribeType(const Type& type) const {
  if (type.nullable) {
    return {
        .name = {},
        .location = {},
        .kind = TypeName::Kind::Nullable,
        .arguments = {DescribeType(NonNullable(type))},
    };
  }
  if (type.kind == Type::Kind::Array) {
    return {
        .name = {},
        .location = {},
        .kind = TypeName::Kind::Array,
        .arguments = {DescribeType(array_elements_.at(type.name))},
    };
  }
  if (type.kind == Type::Kind::Function) {
    TypeName result{.name = {}, .location = {}, .kind = TypeName::Kind::Function, .arguments = {}};
    const auto& signature = function_types_.at(type.name);
    for (const auto& parameter : signature.parameters) {
      result.arguments.push_back(DescribeType(parameter.type));
    }
    result.arguments.push_back(DescribeType(signature.return_type));
    return result;
  }
  if (const auto found = generic_arguments_.find(type.name); found != generic_arguments_.end()) {
    TypeName result{.name = {}, .location = {}, .kind = TypeName::Kind::Generic, .arguments = {}};
    switch (type.kind) {
      case Type::Kind::Map:
        result.name = "Map";
        break;
      case Type::Kind::Set:
        result.name = "Set";
        break;
      case Type::Kind::Future:
        result.name = "Future";
        break;
      case Type::Kind::Completer:
        result.name = "Completer";
        break;
      default:
        break;
    }
    for (const auto& argument : found->second) {
      result.arguments.push_back(DescribeType(argument));
    }
    return result;
  }
  return specializer_->DescribeType({.name = type.name, .location = {}, .arguments = {}});
}

TypeChecker::Type TypeChecker::ResolveInferenceContext(const TypeName& pattern,
                                                       const TypeInference& inference) {
  if (const auto resolved = inference.Substitute(pattern)) {
    return ResolveType(*resolved, true);
  }
  if (pattern.kind != TypeName::Kind::Function) {
    return InvalidType();
  }
  FunctionInfo context;
  for (std::size_t index = 0; index + 1 < pattern.arguments.size(); ++index) {
    const auto parameter = ResolveInferenceContext(pattern.arguments.at(index), inference);
    if (!IsValid(parameter)) {
      return InvalidType();
    }
    const auto name = std::to_string(index);
    context.parameter_indices.emplace(name, static_cast<std::uint32_t>(index));
    context.parameters.push_back({.name = name, .type = parameter});
  }
  context.return_type = ResolveInferenceContext(pattern.arguments.back(), inference);
  return FunctionType(context);
}

bool TypeChecker::CanInferInvocation(const Expression& expression, const Type* expected) {
  const auto& callee = *expression.callee;
  if (callee.kind != Expression::Kind::Name || !callee.type_arguments.empty() ||
      LookupLocal(callee.text) != nullptr) {
    return true;
  }
  const auto& name = callee.text;
  if (name == "Map" || name == "Set" || name == "Completer") {
    return expected != nullptr;
  }
  if (specializer_->FunctionTemplate(name) == nullptr &&
      specializer_->ClassTemplate(name) == nullptr &&
      specializer_->AliasTemplate(name) == nullptr) {
    return true;
  }
  const auto source = GenericSignature(callee);
  if (!source) {
    return true;
  }
  TypeInference inference(source->type_parameters);
  if (expected != nullptr) {
    inference.Constrain(source->return_type, DescribeType(*expected),
                        TypeInference::Evidence::Context);
  }
  std::vector<TypeName> inputs;
  std::size_t positional = 0;
  for (const auto& argument : expression.arguments) {
    auto parameter = source->parameters.end();
    if (argument.name.empty()) {
      if (positional < source->parameters.size()) {
        parameter = source->parameters.begin() + static_cast<std::ptrdiff_t>(positional++);
      }
    } else {
      parameter = std::ranges::find(source->parameters, argument.name, &Parameter::name);
    }
    if (parameter != source->parameters.end() && CanCheckExpression(*argument.value, nullptr)) {
      inputs.push_back(parameter->type);
    }
  }
  return inference.CanResolveFrom(inputs);
}

bool TypeChecker::CanCheckExpression(const Expression& expression, const Type* expected) {
  switch (expression.kind) {
    case Expression::Kind::Conditional:
      return CanCheckExpression(*expression.left, expected) &&
             CanCheckExpression(*expression.right, expected);
    case Expression::Kind::Binary:
      return expression.text != "??" || (CanCheckExpression(*expression.left, expected) &&
                                         CanCheckExpression(*expression.right, expected));
    case Expression::Kind::Match:
      return std::ranges::all_of(expression.arms, [&](const auto& arm) {
        return CanCheckExpression(*arm.value, expected);
      });
    case Expression::Kind::Await: {
      const auto context =
          expected != nullptr ? MakeFutureType(expression, *expected) : InvalidType();
      return CanCheckExpression(*expression.operand, IsValid(context) ? &context : nullptr);
    }
    case Expression::Kind::Lambda:
      return (expected != nullptr && expected->kind == Type::Kind::Function) ||
             std::ranges::all_of(expression.function->parameters, [](const Parameter& parameter) {
               return !parameter.type.name.empty();
             });
    case Expression::Kind::ArrayLiteral:
      return expected != nullptr ||
             (!expression.elements.empty() &&
              std::ranges::all_of(expression.elements, [&](const auto& element) {
                return CanCheckExpression(*element, nullptr);
              }));
    case Expression::Kind::Name:
      return LookupLocal(expression.text) != nullptr ||
             specializer_->FunctionTemplate(expression.text) == nullptr ||
             (expected != nullptr && expected->kind == Type::Kind::Function);
    case Expression::Kind::Call:
      return CanInferInvocation(expression, expected);
    default:
      return true;
  }
}

TypeChecker::Type TypeChecker::CheckCallback(const Expression& expression,
                                             FunctionInfo context,
                                             CallbackArity arity,
                                             CallbackPresence presence,
                                             CallbackParameters parameters) {
  // 函数体检查可能重排或移除临时签名；接管上下文，跨检查持有独立契约。
  const auto contract = std::move(context);
  auto signature = contract;
  const auto* generic =
      expression.kind == Expression::Kind::Name && LookupLocal(expression.text) == nullptr
          ? specializer_->FunctionTemplate(expression.text)
          : nullptr;
  if (arity == CallbackArity::Prefix && expression.kind == Expression::Kind::Lambda &&
      expression.function->parameters.size() <= signature.parameters.size()) {
    signature.parameters.resize(expression.function->parameters.size());
  } else if (arity == CallbackArity::Prefix && generic != nullptr &&
             generic->parameters.size() <= signature.parameters.size()) {
    signature.parameters.resize(generic->parameters.size());
  }
  auto expected = FunctionType(signature);
  if (presence == CallbackPresence::Optional) {
    expected = NullableType(expected);
  }
  auto actual = CheckExpression(expression, arity == CallbackArity::Prefix &&
                                                    expression.kind != Expression::Kind::Lambda &&
                                                    generic == nullptr
                                                ? nullptr
                                                : &expected);
  if (!IsValid(signature.return_type) && actual.name != expected.name) {
    function_types_.erase(expected.name);
    types_.erase(expected.name);
  }
  if (actual.kind != Type::Kind::Function ||
      (actual.nullable && presence == CallbackPresence::Required)) {
    AddDiagnostic(expression.location, "callback must be a non-null function");
    return InvalidType();
  }
  CheckCallbackContract(expression, actual, contract, arity, parameters);
  return actual;
}

void TypeChecker::CheckCallbackContract(const Expression& expression,
                                        const Type& actual,
                                        const FunctionInfo& context,
                                        CallbackArity arity,
                                        CallbackParameters parameters) {
  const auto& checked = function_types_.at(NonNullable(actual).name);
  if ((arity == CallbackArity::Exact && checked.parameters.size() != context.parameters.size()) ||
      checked.parameters.size() > context.parameters.size()) {
    AddDiagnostic(expression.location, "callback parameter count does not match");
    return;
  }
  for (std::size_t index = 0;
       parameters == CallbackParameters::Assignable && index < checked.parameters.size(); ++index) {
    const auto& parameter = checked.parameters.at(index).type;
    const auto& argument = context.parameters.at(index).type;
    if (!CanAssign(parameter, argument)) {
      AddDiagnostic(expression.location, "callback parameter type does not match");
    }
    DiagnoseDynamicConversion(expression, parameter, argument);
  }
}

std::optional<TypeInference> TypeChecker::CreateInference(
    const InferenceSignature& source,
    SourceLocation location,
    const Type* expected,
    std::span<const TypeName> explicit_arguments) {
  TypeInference inference(source.type_parameters);
  if (!explicit_arguments.empty() && explicit_arguments.size() != source.type_parameters.size()) {
    AddDiagnostic(location, "generic argument count does not match type parameter count");
    return std::nullopt;
  }
  for (std::size_t index = 0; index < explicit_arguments.size(); ++index) {
    const auto type = ResolveType(explicit_arguments.subspan(index).front(), true);
    if (!IsValid(type)) {
      return std::nullopt;
    }
    inference.Constrain(
        {.name = source.type_parameters.at(index), .location = location, .arguments = {}},
        DescribeType(type), TypeInference::Evidence::Explicit);
  }
  if (expected != nullptr && IsValid(*expected) && expected->kind != Type::Kind::Dynamic) {
    inference.Constrain(source.return_type, DescribeType(*expected),
                        TypeInference::Evidence::Context);
  }
  return inference;
}

std::optional<TypeInference> TypeChecker::CreateFutureInference(const Expression& expression,
                                                                const Type* expected) {
  InferenceSignature source;
  source.type_parameters = {"$Value"};
  source.return_type = {
      .name = "Future",
      .location = expression.location,
      .kind = TypeName::Kind::Generic,
      .arguments = {{.name = "$Value", .location = expression.location, .arguments = {}}},
  };
  const auto projected = expected != nullptr && expected->kind != Type::Kind::Dynamic &&
                                 expected->kind != Type::Kind::Future
                             ? MakeFutureType(expression, *expected)
                             : InvalidType();
  auto inference =
      CreateInference(source, expression.location, IsValid(projected) ? &projected : expected,
                      expression.callee->type_arguments);
  if (inference) {
    const auto payload = ResolveInferenceContext(source.return_type.arguments.front(), *inference);
    if (payload.kind == Type::Kind::Future) {
      AddDiagnostic(expression.location, "Future payload types must be flattened");
      return std::nullopt;
    }
  }
  return inference;
}

TypeChecker::Type TypeChecker::InferFutureResult(const Expression& expression,
                                                 TypeInference& inference,
                                                 const Type* actual) {
  const TypeName variable{.name = "$Value", .location = expression.location, .arguments = {}};
  const auto context = ResolveInferenceContext(variable, inference);
  Type produced;
  const bool dynamic_value = actual != nullptr && actual->kind == Type::Kind::Dynamic &&
                             expression.callee->text == "value";
  if (actual != nullptr && actual->kind == Type::Kind::Null) {
    if (!IsValid(context) || !CanAssign(context, *actual)) {
      AddDiagnostic(expression.location, "null Future value requires a nullable payload type");
      return InvalidType();
    }
    return MakeFutureType(expression, context);
  }
  if (actual != nullptr) {
    produced = MakeFutureType(expression, *actual);
    if (!IsValid(produced)) {
      return InvalidType();
    }
    if (dynamic_value && IsValid(context)) {
      DiagnoseDynamicConversion(*expression.arguments.front().value, context, *actual);
    } else {
      inference.Constrain(variable, DescribeType(generic_arguments_.at(produced.name).front()),
                          TypeInference::Evidence::Argument);
    }
  } else if (!IsValid(context)) {
    // Future.error 没有成功值，缺少目标类型时按公开契约产生 Future<void>。
    inference.Constrain(variable, DescribeType(types_.at("void")),
                        TypeInference::Evidence::Context);
  }
  const auto payload = ResolveInferenceContext(variable, inference);
  const auto result = MakeFutureType(expression, payload);
  if (IsValid(produced) && IsValid(result) && !dynamic_value && !CanAssign(result, produced)) {
    AddDiagnostic(expression.location, "Future result does not match its type argument");
  }
  return result;
}

TypeChecker::Type TypeChecker::CheckInferenceArgument(const Expression& expression,
                                                      const TypeName& pattern,
                                                      const Type& context,
                                                      TypeInference& inference,
                                                      CallbackArity arity) {
  auto actual = context.kind == Type::Kind::Function
                    ? CheckCallback(expression, function_types_.at(context.name), arity)
                    : CheckExpression(expression, IsValid(context) ? &context : nullptr);
  if (!IsValid(actual)) {
    return actual;
  }
  auto matched = pattern;
  const auto described = DescribeType(actual);
  if (arity == CallbackArity::Prefix && matched.kind == TypeName::Kind::Function &&
      described.kind == TypeName::Kind::Function &&
      described.arguments.size() < matched.arguments.size()) {
    auto returned = matched.arguments.back();
    matched.arguments.resize(described.arguments.size());
    matched.arguments.back() = std::move(returned);
  }
  inference.Constrain(matched, described,
                      pattern.kind == TypeName::Kind::Function ? TypeInference::Evidence::Callback
                                                               : TypeInference::Evidence::Argument);
  return actual;
}

std::vector<std::optional<TypeChecker::Type>> TypeChecker::CheckInferenceArguments(
    std::span<const Expression* const> expressions,
    std::span<const TypeName> patterns,
    TypeInference& inference,
    CallbackArity arity) {
  std::vector<std::optional<Type>> result(expressions.size());
  std::vector<std::optional<Refinements>> snapshots(expressions.size());
  for (std::size_t index = 0; index < expressions.size(); ++index) {
    const auto& expression = *expressions.subspan(index).front();
    const auto& pattern = patterns.subspan(index).front();
    if (CanCheckExpression(expression, nullptr)) {
      const auto context = ResolveInferenceContext(pattern, inference);
      result.at(index) = CheckInferenceArgument(expression, pattern, context, inference, arity);
    } else {
      snapshots.at(index) = refinements_.Snapshot();
      // 延迟的是静态类型求解，不是源码中的效果；后续实参不能沿用已失效的收窄。
      InvalidateAssignments(expression);
    }
  }
  bool progress = true;
  while (progress) {
    progress = false;
    for (std::size_t index = 0; index < expressions.size(); ++index) {
      if (result.at(index)) {
        continue;
      }
      const auto& expression = *expressions.subspan(index).front();
      const auto& pattern = patterns.subspan(index).front();
      const auto context = ResolveInferenceContext(pattern, inference);
      if (!CanCheckExpression(expression, IsValid(context) ? &context : nullptr)) {
        continue;
      }
      auto current = std::move(refinements_);
      refinements_ = std::move(*snapshots.at(index));
      result.at(index) = CheckInferenceArgument(expression, pattern, context, inference, arity);
      refinements_ = std::move(current);
      progress = true;
    }
  }
  return result;
}

std::optional<TypeChecker::InferredCall> TypeChecker::InferInvocation(
    const InferenceSignature& source,
    const Expression& expression,
    const Type* expected,
    std::span<const TypeName> explicit_arguments,
    CallbackArity arity) {
  auto created = CreateInference(source, expression.location, expected, explicit_arguments);
  if (!created) {
    return std::nullopt;
  }
  auto& inference = *created;
  InferredCall result;
  result.arguments.resize(expression.arguments.size());
  result.signature.optional_parameters = source.optional_parameters;
  for (const auto& parameter : source.parameters) {
    result.signature.parameter_indices.emplace(
        parameter.name, static_cast<std::uint32_t>(result.signature.parameters.size()));
    result.signature.parameters.push_back({.name = parameter.name, .type = {}});
  }
  std::vector<const Expression*> arguments;
  std::vector<TypeName> patterns;
  std::size_t positional = 0;
  for (const auto& argument : expression.arguments) {
    const auto parameter = CallParameterIndex(result.signature, argument, positional);
    if (!parameter) {
      return std::nullopt;
    }
    arguments.push_back(argument.value.get());
    patterns.push_back(source.parameters.at(*parameter).type);
    if (argument.name.empty()) {
      ++positional;
    }
  }
  result.arguments = CheckInferenceArguments(arguments, patterns, inference, arity);
  const auto remaining =
      std::ranges::count_if(result.arguments, [](const auto& type) { return !type; });
  for (const auto& conflict : inference.conflicts()) {
    AddDiagnostic(expression.location, "conflicting inferred types for '" + conflict + "'");
  }
  const auto unresolved = inference.Unresolved();
  for (const auto& parameter : unresolved) {
    AddDiagnostic(expression.location, "cannot infer type argument '" + parameter +
                                           "'; provide type arguments or a type context");
  }
  if (!unresolved.empty() || !inference.conflicts().empty() || remaining != 0) {
    if (remaining != 0 && unresolved.empty()) {
      AddDiagnostic(expression.location, "cannot infer argument types from this call context");
    }
    return std::nullopt;
  }
  const auto solution = inference.Solve();
  if (!solution) {
    return std::nullopt;
  }
  for (const auto& parameter : source.type_parameters) {
    result.type_arguments.push_back(solution->Substitute(
        {.name = parameter, .location = expression.location, .arguments = {}}));
  }
  result.signature.return_type = ResolveType(solution->Substitute(source.return_type), true);
  for (std::size_t index = 0; index < source.parameters.size(); ++index) {
    result.signature.parameters.at(index).type =
        ResolveType(solution->Substitute(source.parameters.at(index).type), false);
  }
  return result;
}

TypeChecker::Type TypeChecker::InferConstruction(const Expression& expression,
                                                 const Type* expected,
                                                 std::string_view name,
                                                 std::span<const std::string> parameters) {
  InferenceSignature source;
  source.type_parameters.assign(parameters.begin(), parameters.end());
  source.return_type = {
      .name = std::string(name),
      .location = expression.location,
      .kind = TypeName::Kind::Generic,
      .arguments = {},
  };
  for (const auto& parameter : parameters) {
    source.return_type.arguments.push_back(
        {.name = parameter, .location = expression.location, .arguments = {}});
  }
  const auto inferred =
      InferInvocation(source, expression, expected, expression.callee->type_arguments);
  return inferred ? CheckFunctionCall(inferred->signature, expression, inferred->arguments)
                  : InvalidType();
}

std::optional<TypeChecker::InferenceSignature> TypeChecker::GenericSignature(
    const Expression& callee) {
  InferenceSignature source;
  const auto* function = specializer_->FunctionTemplate(callee.text);
  const auto* owner = specializer_->ClassTemplate(callee.text);
  const auto* alias = specializer_->AliasTemplate(callee.text);
  if (function != nullptr) {
    source.type_parameters = function->type_parameters;
    source.parameters = function->parameters;
    source.return_type = function->return_type;
    source.optional_parameters.insert(function->optional_parameters.begin(),
                                      function->optional_parameters.end());
  } else {
    source.type_parameters = alias != nullptr ? alias->type_parameters : owner->type_parameters;
    source.return_type = {
        .name = callee.text,
        .location = callee.location,
        .kind = TypeName::Kind::Generic,
        .arguments = {},
    };
    for (const auto& parameter : source.type_parameters) {
      source.return_type.arguments.push_back(
          {.name = parameter, .location = callee.location, .arguments = {}});
    }
    source.return_type =
        specializer_->ExpandAlias(std::move(source.return_type), source.type_parameters);
    if (alias != nullptr) {
      owner = specializer_->ClassTemplate(source.return_type.name);
    }
    if (owner == nullptr) {
      if (source.return_type.kind == TypeName::Kind::Generic &&
          (source.return_type.name == "Map" || source.return_type.name == "Set")) {
        return source;
      }
      AddDiagnostic(callee.location, "type alias does not name a constructible type");
      return std::nullopt;
    }
    if (source.return_type.kind != TypeName::Kind::Generic ||
        source.return_type.arguments.size() != owner->type_parameters.size()) {
      AddDiagnostic(callee.location, "generic argument count does not match type parameter count");
      return std::nullopt;
    }
    SetConstructorParameters(*owner, source);
    TypeInference substitution(owner->type_parameters);
    for (std::size_t index = 0; index < owner->type_parameters.size(); ++index) {
      substitution.Constrain(
          {.name = owner->type_parameters.at(index), .location = callee.location, .arguments = {}},
          source.return_type.arguments.at(index), TypeInference::Evidence::Explicit);
    }
    const auto solution = substitution.Solve();
    if (!solution) {
      return std::nullopt;
    }
    for (auto& parameter : source.parameters) {
      parameter.type = solution->Substitute(parameter.type);
    }
  }
  source.return_type =
      specializer_->ExpandAlias(std::move(source.return_type), source.type_parameters);
  for (auto& parameter : source.parameters) {
    parameter.type = specializer_->ExpandAlias(std::move(parameter.type), source.type_parameters);
  }
  return source;
}

std::optional<std::vector<TypeName>> TypeChecker::InferFunctionValue(
    const InferenceSignature& source,
    const Expression& callee,
    const Type* expected) {
  if (expected == nullptr || expected->kind != Type::Kind::Function) {
    AddDiagnostic(callee.location, "generic function value requires a function type context");
    return std::nullopt;
  }
  TypeName signature{
      .name = {},
      .location = callee.location,
      .kind = TypeName::Kind::Function,
      .arguments = {},
  };
  signature.arguments.reserve(source.parameters.size() + 1);
  for (const auto& parameter : source.parameters) {
    signature.arguments.push_back(parameter.type);
  }
  signature.arguments.push_back(source.return_type);
  TypeInference inference(source.type_parameters);
  inference.Constrain(signature, DescribeType(NonNullable(*expected)),
                      TypeInference::Evidence::Context);
  const auto solution = inference.Solve();
  if (!solution) {
    AddDiagnostic(callee.location, "cannot infer generic function value from its type context");
    return std::nullopt;
  }
  std::vector<TypeName> arguments;
  arguments.reserve(source.type_parameters.size());
  for (const auto& parameter : source.type_parameters) {
    arguments.push_back(
        solution->Substitute({.name = parameter, .location = callee.location, .arguments = {}}));
  }
  return arguments;
}

TypeChecker::Type TypeChecker::InstantiateGeneric(const Expression& callee,
                                                  const Expression* call,
                                                  const Type* expected) {
  const auto source_signature = GenericSignature(callee);
  if (!source_signature) {
    return InvalidType();
  }
  const auto& source = *source_signature;
  std::vector<TypeName> arguments;
  std::vector<std::optional<Type>> checked_arguments;
  if (call != nullptr) {
    auto inferred = InferInvocation(source, *call, expected, callee.type_arguments);
    if (!inferred) {
      return InvalidType();
    }
    arguments = inferred->type_arguments;
    const auto result = inferred->signature.return_type;
    if (specializer_->FunctionTemplate(callee.text) == nullptr) {
      generic_applications_.insert_or_assign(&callee, result.name);
      if (result.kind == Type::Kind::Map || result.kind == Type::Kind::Set) {
        collection_constructions_.insert(call);
        return CheckFunctionCall(inferred->signature, *call, inferred->arguments);
      }
      return result.kind == Type::Kind::Class
                 ? CheckClassConstruction(classes_.at(result.name), *call, inferred->arguments)
                 : InvalidType();
    }
    checked_arguments = std::move(inferred->arguments);
  } else {
    auto inferred = InferFunctionValue(source, callee, expected);
    if (!inferred) {
      return InvalidType();
    }
    arguments = std::move(*inferred);
  }
  for (auto& argument : arguments) {
    const auto type = ResolveType(argument, true);
    if (!IsValid(type)) {
      return InvalidType();
    }
    argument = DescribeType(type);
    argument.location = callee.location;
  }
  const auto name = specializer_->Specialize(callee.text, arguments, callee.location);
  RegisterSpecializations();
  generic_applications_.insert_or_assign(&callee, name);
  const auto found = functions_.find(name);
  if (found == functions_.end()) {
    return InvalidType();
  }
  return call != nullptr ? CheckFunctionCall(found->second, *call, checked_arguments)
                         : FunctionType(found->second);
}

}  // namespace vela::frontend
