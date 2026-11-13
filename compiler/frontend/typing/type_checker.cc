#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/syntax/names.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/capture_analysis.h"
#include "compiler/frontend/flow/control_flow.h"
#include "compiler/frontend/generics/specialization.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

TypeChecker::Type TypeChecker::InvalidType() {
  return {};
}

TypeCheckResult TypeChecker::Check(Program& program) && {
  Specializer specializer(program);
  auto specialization_errors = specializer.Initialize();
  if (!specialization_errors.empty()) {
    return {.diagnostics = std::move(specialization_errors), .model = {}};
  }
  return std::move(*this).CheckPrepared(program, specializer, {});
}

TypeCheckResult TypeChecker::CheckPrepared(Program& program,
                                           Specializer& specializer,
                                           std::span<const GenericSpecialization> retained) && {
  auto specialization_errors = specializer.Retain(retained);
  if (!specialization_errors.empty()) {
    return {.diagnostics = std::move(specialization_errors), .model = {}};
  }
  specializer_ = &specializer;
  CollectCapturedMutableDeclarations(program, captured_declarations_);
  sources_ = program.sources;

  CollectTypes(program);
  RegisterTemplateParameters(program);
  ResolveInterfaces(program);
  ResolveDeclarations(program);
  ResolveGlobals(program);
  CheckImplementations(program);
  for (const auto& alias : program.aliases) {
    static_cast<void>(ResolveType(
        {
            .name = alias.name,
            .location = alias.location,
            .kind = TypeName::Kind::Named,
            .arguments = {},
            .parameter = {},
        },
        VoidTypePolicy::Allowed));
  }
  // 泛型定义先在抽象参数环境下检查，具体特化之后只验证具体布局与执行契约。
  CheckDefinitions(program);
  RestoreMethodSpecializations(retained);
  CheckReflectionSelections(program);
  // 普通声明先注册；检查函数体时按需注册特化，描述对象保持稳定地址。
  for (const Function& function : program.functions) {
    specializer.SetExpansionOwner(function.name);
    CheckFunction(function, nullptr);
  }
  for (const Class& class_declaration : program.classes) {
    specializer.SetExpansionOwner(class_declaration.name);
    CheckClassBody(class_declaration, classes_.at(class_declaration.name));
  }
  while (true) {
    CheckPendingSpecializations();
    DrainInterfaceBindings();
    if (!DrainBoundChecks() && checked_functions_ == specializer.function_count() &&
        checked_classes_ == specializer.class_count() &&
        checked_methods_ == specializer.method_count()) {
      break;
    }
  }
  specializer.Publish();
  ResolveNominalArguments(program);
  for (const auto& source : program.classes) {
    if (const auto found = classes_.find(source.name); found != classes_.end()) {
      found->second.declaration = &source;
    }
  }
  CheckAttributes(program);
  SemanticModel model;
  for (const auto& [identity, parameter] : type_parameters_) {
    if (parameter.bound) {
      model.parameter_bounds.emplace(identity, *parameter.bound);
    }
  }
  model.globals = std::move(globals_);
  model.types = std::move(types_);
  model.array_elements = std::move(array_elements_);
  model.function_types = std::move(function_types_);
  model.captured_mutables = std::move(captured_mutables_);
  model.enum_values = std::move(enum_values_);
  model.classes = std::move(classes_);
  model.functions = std::move(functions_);
  model.interfaces = std::move(interfaces_);
  model.explicit_super_constructors = std::move(explicit_super_constructors_);
  model.generic_arguments = std::move(generic_arguments_);
  static_cast<NodeFacts&>(model) = std::exchange(facts_, {});
  model.type_applications = specializer.TakeTypeApplications();
  model.definitions = std::move(definitions_);
  model.json_decode_requests = std::move(json_decode_requests_);
  model.reflection = std::move(reflection_);
  model.attributes = std::move(attributes_);
  for (auto& diagnostic : BuildJSONDecodePlans(model)) {
    AddDiagnostic(diagnostic.location, std::move(diagnostic.message));
  }
  // 模板与其特化的声明解析可能在同一源码位置给出相同诊断，只保留首次出现。
  absl::flat_hash_set<std::tuple<std::string, std::uint32_t, std::uint32_t, std::string>> reported;
  std::erase_if(diagnostics_, [&](const Diagnostic& diagnostic) {
    return !reported
                .emplace(diagnostic.source_path, diagnostic.location.line,
                         diagnostic.location.column, diagnostic.message)
                .second;
  });
  return {
      .diagnostics = std::move(diagnostics_),
      .function_count = program.functions.size(),
      .model = std::move(model),
  };
}

void TypeChecker::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path =
          location.source_id < sources_.size() ? sources_[location.source_id].path : std::string(),
  });
}

TypeChecker::FunctionInfo TypeChecker::ResolveSignature(const Function& function,
                                                        const FunctionInfo* context) {
  FunctionInfo info;
  info.intrinsic = function.intrinsic;
  info.intrinsic_operation = function.intrinsic_operation;
  info.optional_parameters.insert(function.optional_parameters.begin(),
                                  function.optional_parameters.end());
  if (function.intrinsic == Builtin::IntParse || function.intrinsic == Builtin::IntTryParse) {
    info.optional_parameters.insert("radix");
  } else if (function.intrinsic == Builtin::DateFromUTC) {
    info.optional_parameters = {"hour", "minute", "second", "millisecond"};
  } else if (function.intrinsic && IsByteBuiltin(*function.intrinsic)) {
    info.optional_parameters = BuiltinSignature(*function.intrinsic).optional_parameters;
  }
  info.visibility = function.visibility;
  info.storage = function.storage;
  info.is_getter = function.is_getter;
  info.is_setter = function.is_setter;
  if (function.application) {
    for (const auto& argument : function.application->arguments) {
      info.type_arguments.push_back(ResolveType(argument, VoidTypePolicy::Allowed));
    }
  }
  if (function.return_type.kind != TypeName::Kind::Inferred) {
    info.return_type = ResolveType(function.return_type, VoidTypePolicy::Allowed);
  } else if (context != nullptr) {
    info.return_type = context->return_type;
  }
  info.parameters.reserve(function.parameters.size());
  info.parameter_indices.reserve(function.parameters.size());
  for (std::size_t index = 0; index < function.parameters.size(); ++index) {
    const auto& parameter = function.parameters.at(index);
    Type type;
    if (parameter.type.kind != TypeName::Kind::Inferred) {
      type = ResolveType(parameter.type, VoidTypePolicy::Forbidden);
    } else if (context != nullptr && index < context->parameters.size()) {
      type = context->parameters.at(index).type;
    } else {
      AddDiagnostic(parameter.location, "cannot infer lambda parameter '" + parameter.name +
                                            "'; add a type annotation or function type context");
    }
    if (!info.parameter_indices
             .emplace(parameter.name, static_cast<std::uint32_t>(info.parameters.size()))
             .second) {
      AddDiagnostic(parameter.location, "duplicate parameter '" + parameter.name + "'");
    } else {
      info.parameters.push_back({.name = parameter.name, .type = type});
    }
  }
  return info;
}

void TypeChecker::CheckFunction(const Function& function, const ClassInfo* owner) {
  if (owner == nullptr) {
    const auto found = functions_.find(function.name);
    if (found != functions_.end()) {
      CheckFunctionBody(function, found->second, owner);
    }
    return;
  }
  const auto found = owner->methods.find(function.name);
  if (found == owner->methods.end()) {
    return;
  }
  if (function.is_getter || function.is_setter) {
    const auto signature = ResolveSignature(function);
    CheckFunctionBody(function, signature, owner);
  } else {
    CheckFunctionBody(function, found->second, owner);
  }
}

void TypeChecker::CheckFunctionBody(const Function& function,
                                    const FunctionInfo& function_info,
                                    const ClassInfo* owner) {
  owner_ = owner;
  in_async_function_ = function.is_async;
  refinements_.Clear();
  constant_bindings_.clear();
  const auto& return_type = function_info.return_type;
  in_constructor_ = owner != nullptr && function.name == kConstructorName;
  if (function.is_async && return_type.kind != Type::Kind::Future) {
    AddDiagnostic(function.return_type.location, "async function return type must be Future<T>");
  }

  BeginScope();
  if (owner != nullptr && function.storage == MemberStorage::Instance) {
    scopes_.back().try_emplace(kReceiverName, LocalBinding{
                                                  .type = owner->type,
                                                  .kind = BindingKind::Constant,
                                                  .id = next_binding_id_++,
                                                  .scope = scopes_.size() - 1,
                                              });
    // 方法节点在发布泛型方法应用时可能移动；接收者绑定以函数体节点为键。
    facts_.bindings.emplace(function.body.get(), LookupLocal(kReceiverName)->id);
  }
  DeclareFunctionParameters(function, function_info, BindingKind::Mutable);
  if (function.body) {
    const auto flow = CheckSequence(*function.body, return_type);
    facts_.control_flow.insert_or_assign(function.body.get(), flow);
    if (!CanFallThrough(function, return_type) && flow.can_fall_through) {
      AddDiagnostic(function.location, "non-void function may fall through without returning");
    }
  }
  EndScope();
  owner_ = nullptr;
  in_constructor_ = false;
  in_async_function_ = false;
}

bool TypeChecker::CanFallThrough(const Function& function, const Type& return_type) const {
  if (return_type.kind == Type::Kind::Void || return_type.kind == Type::Kind::Invalid) {
    return true;
  }
  if (!function.is_async || return_type.kind != Type::Kind::Future) {
    return false;
  }
  return IsVoidCallbackResult(return_type);
}

TypeChecker::Type TypeChecker::CheckExpression(const Expression& expression, const Type* expected) {
  if (const auto prepared = prepared_expressions_.find(&expression);
      prepared != prepared_expressions_.end()) {
    auto result = std::move(prepared->second);
    prepared_expressions_.erase(prepared);
    refinements_.Apply(result.refinements);
    return result.type;
  }
  auto type = CheckExpressionType(expression, expected);
  if (expression.kind == Expression::Kind::Call) {
    InvalidateCapturedRefinements();
  }
  facts_.expression_types.insert_or_assign(&expression, type);
  return type;
}

TypeChecker::Type TypeChecker::CheckExpressionType(const Expression& expression,
                                                   const Type* expected) {
  switch (expression.kind) {
    case Expression::Kind::Match:
      return CheckMatch(expression, expected);
    case Expression::Kind::Await:
      return CheckAwait(expression, expected);
    case Expression::Kind::Conditional:
      return CheckConditional(expression, expected);
    case Expression::Kind::TypeTest:
    case Expression::Kind::TypeCast:
      return CheckTypeOperation(expression);
    case Expression::Kind::Lambda:
      return CheckLambda(expression, expected);
    case Expression::Kind::NullLiteral:
      return {.kind = Type::Kind::Null, .name = kNullTypeName};
    case Expression::Kind::IntegerLiteral:
      return CheckIntegerLiteral(expression);
    case Expression::Kind::DoubleLiteral:
      return {.kind = Type::Kind::Double, .name = kDoubleTypeName};
    case Expression::Kind::StringLiteral:
      return {.kind = Type::Kind::String, .name = kStringTypeName};
    case Expression::Kind::StringInterpolation:
      for (const auto& part : expression.elements) {
        auto type = CheckExpression(*part, nullptr);
        type = ParameterCapability(type, part->location);
        if (!CanFormatString(type) && IsValid(type)) {
          AddDiagnostic(part->location,
                        "interpolation requires string, int, double, bool, or null");
        }
      }
      return {.kind = Type::Kind::String, .name = kStringTypeName};
    case Expression::Kind::ArrayLiteral:
      return CheckArrayLiteral(expression, expected);
    case Expression::Kind::MapLiteral:
      return CheckMapLiteral(expression, expected);
    case Expression::Kind::Index:
      return CheckIndex(expression);
    case Expression::Kind::BooleanLiteral:
      return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
    case Expression::Kind::Name:
      return LookupName(expression, expected);
    case Expression::Kind::Unary:
      return CheckUnary(expression);
    case Expression::Kind::Binary:
      return CheckBinary(expression, expected);
    case Expression::Kind::CompoundAssignment:
      return CheckCompoundAssignment(expression);
    case Expression::Kind::Call:
      return CheckCall(expression, expected);
    case Expression::Kind::Member:
      return CheckMemberAccess(expression, expected);
  }
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckAwait(const Expression& expression, const Type* expected) {
  if (!in_async_function_) {
    AddDiagnostic(expression.location, "await is only valid inside an async function");
    return InvalidType();
  }
  const auto context = expected != nullptr ? MakeFutureType(expression, *expected) : InvalidType();
  auto operand = CheckExpression(*expression.operand, IsValid(context) ? &context : nullptr);
  operand = ParameterCapability(operand, expression.operand->location);
  if (operand.kind != Type::Kind::Future) {
    return operand;
  }
  const auto& element = generic_arguments_.at(NonNullableTypeName(operand)).front();
  return operand.nullable && element.kind != Type::Kind::Void ? NullableType(element) : element;
}

TypeChecker::Type TypeChecker::CheckCall(const Expression& expression, const Type* expected) {
  const auto& callee = *expression.callee;
  const auto* local = callee.kind == Expression::Kind::Name ? LookupLocal(callee.text) : nullptr;
  if (callee.kind == Expression::Kind::Name && local == nullptr &&
      (callee.text == kCompleterTypeName || callee.text == kCancellationTokenTypeName)) {
    return CheckAsyncConstruction(expression, expected);
  }
  if (callee.kind == Expression::Kind::Member && callee.object->kind == Expression::Kind::Name &&
      callee.object->text == kTimerTypeName && LookupLocal(callee.object->text) == nullptr) {
    return CheckTimerFactory(expression);
  }
  if (callee.kind == Expression::Kind::Name && callee.text == kSuperName) {
    if (owner_ == nullptr || !owner_->base_class || !in_constructor_ || !lambda_scopes_.empty()) {
      AddDiagnostic(expression.location, "super() requires a derived constructor context");
      return InvalidType();
    }
    const auto base = classes_.find(*owner_->base_class);
    if (base == classes_.end()) {
      return InvalidType();
    }
    explicit_super_constructors_.insert(std::string(owner_->type.name));
    return CheckClassConstruction(base->second, expression);
  }
  if (callee.kind == Expression::Kind::Member && callee.object->kind == Expression::Kind::Name &&
      callee.object->text == kFutureTypeName && LookupLocal(callee.object->text) == nullptr) {
    return CheckFutureCall(expression, expected);
  }
  if (callee.kind == Expression::Kind::Name && local == nullptr &&
      (callee.text == kSetTypeName || callee.text == kMapTypeName)) {
    return CheckCollectionConstruction(expression, expected);
  }
  if (callee.kind == Expression::Kind::Member) {
    return CheckMethodCall(expression, expected);
  }
  if (callee.kind != Expression::Kind::Name || local != nullptr) {
    return CheckIndirectCall(expression, CheckExpression(callee, nullptr));
  }
  if (globals_.contains(callee.text)) {
    return CheckIndirectCall(expression, CheckExpression(callee, nullptr));
  }
  if (callee.text == kSleepFunctionName) {
    return CheckSleepCall(expression);
  }
  return CheckNamedCall(expression, expected);
}

TypeChecker::Type TypeChecker::CheckIndirectCall(const Expression& expression, const Type& type) {
  if (type.kind == Type::Kind::Parameter) {
    return CheckIndirectCall(expression, ParameterCapability(type, expression.callee->location));
  }
  if (type.kind == Type::Kind::Dynamic) {
    return CheckDynamicCall(expression);
  }
  const auto& callee = *expression.callee;
  if (type.kind != Type::Kind::Function || type.nullable) {
    AddDiagnostic(callee.location,
                  "expression is not callable; nullable functions require a non-null check");
    return InvalidType();
  }
  for (const auto& argument : expression.arguments) {
    if (!argument.name.empty()) {
      AddDiagnostic(argument.location, "function values accept positional arguments only");
    }
  }
  const auto& signature = function_types_.at(type.name);
  return CheckFunctionCall(signature, expression, {}, CallArgumentNames::Positional);
}

TypeChecker::Type TypeChecker::CheckDynamicCall(const Expression& expression) {
  const Type dynamic{.kind = Type::Kind::Dynamic, .name = kDynamicTypeName};
  if (!expression.callee->type_arguments.empty()) {
    AddDiagnostic(expression.callee->location,
                  "dynamic calls require generic callables to be instantiated before erasure");
  }
  for (const auto& argument : expression.arguments) {
    const auto actual = CheckExpression(*argument.value, &dynamic);
    RecordConversion(*argument.value, dynamic, actual);
    if (actual.kind == Type::Kind::Void) {
      AddDiagnostic(argument.location, "call argument requires a value");
    }
  }
  return dynamic;
}

TypeChecker::Type TypeChecker::CheckNamedCall(const Expression& expression, const Type* expected) {
  const auto& callee = *expression.callee;
  if (specializer_->FunctionTemplate(callee.text) != nullptr ||
      specializer_->ClassTemplate(callee.text) != nullptr ||
      specializer_->AliasTemplate(callee.text) != nullptr) {
    return InstantiateGeneric(callee, &expression, expected);
  }
  if (const auto builtin = FindBuiltin(Type::Kind::Invalid, callee.text)) {
    return CheckBuiltinCall(expression, *builtin);
  }
  if (callee.text == "print") {
    if (expression.arguments.size() != 1) {
      AddDiagnostic(expression.location, "print expects one argument");
    } else {
      const auto& argument = *expression.arguments.front().value;
      if (CheckExpression(argument, nullptr).kind == Type::Kind::Void) {
        AddDiagnostic(argument.location, "print requires a value");
      }
    }
    return {.kind = Type::Kind::Void, .name = kVoidTypeName};
  }
  std::string constructor_name = callee.text;
  if (aliases_.contains(callee.text)) {
    const auto alias_type = ResolveType(
        {
            .name = callee.text,
            .location = callee.location,
            .kind = TypeName::Kind::Named,
            .arguments = {},
            .parameter = {},
        },
        VoidTypePolicy::Forbidden);
    if (alias_type.nullable) {
      AddDiagnostic(callee.location, "nullable type aliases are not constructible");
      return InvalidType();
    }
    if (alias_type.kind == Type::Kind::Set || alias_type.kind == Type::Kind::Map) {
      return CheckCollectionConstruction(expression, &alias_type);
    }
    constructor_name = alias_type.name;
  }
  if (constructor_name != callee.text) {
    if (const auto builtin = FindBuiltin(Type::Kind::Invalid, constructor_name)) {
      return CheckBuiltinCall(expression, *builtin);
    }
  }
  const auto class_info = classes_.find(constructor_name);
  if (class_info != classes_.end()) {
    return CheckClassConstruction(class_info->second, expression);
  }
  const auto function = functions_.find(callee.text);
  if (function != functions_.end()) {
    return CheckFunctionCall(function->second, expression);
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckMethodCall(const Expression& expression, const Type* expected) {
  if (const auto* owner = StaticOwner(*expression.callee)) {
    return CheckStaticCall(expression, *owner, expected);
  }
  const auto& receiver = *expression.callee->object;
  if (receiver.kind == Expression::Kind::Name && receiver.text == kSuperName) {
    return CheckSuperMethodCall(expression, expected);
  }
  Type context;
  if (receiver.kind == Expression::Kind::ArrayLiteral && receiver.elements.empty() &&
      expression.callee->text == "join") {
    context = ArrayType({.kind = Type::Kind::String, .name = kStringTypeName});
  }
  auto object = CheckExpression(receiver, IsValid(context) ? &context : nullptr);
  object = ParameterCapability(object, receiver.location);
  if (object.kind == Type::Kind::Dynamic) {
    if (!expression.callee->type_arguments.empty()) {
      AddDiagnostic(expression.callee->location,
                    "generic methods require a statically known class or interface receiver");
      return InvalidType();
    }
    facts_.expression_types.insert_or_assign(expression.callee.get(), object);
    InvalidateCapturedRefinements();
    return CheckDynamicCall(expression);
  }
  if (CheckReceiverNullability(*expression.callee, object) == CheckStatus::Failed) {
    return InvalidType();
  }
  Type result = CheckMethodReceiver(expression, object, expected);
  if (expression.callee->null_safe && IsValid(result) && result.kind != Type::Kind::Void) {
    result = NullableType(result);
  }
  return result;
}

TypeChecker::Type TypeChecker::CheckSuperMethodCall(const Expression& expression,
                                                    const Type* expected) {
  const auto* base = CheckSuperClass(expression.location);
  if (base == nullptr) {
    return InvalidType();
  }
  if (const auto applied = CheckMethodApplication(base->method_templates, *expression.callee,
                                                  &expression, expected, MethodAccess::Super)) {
    return *applied;
  }
  const auto method = base->methods.find(expression.callee->text);
  if (method == base->methods.end() || method->second.is_getter) {
    AddDiagnostic(expression.location, "unknown super method '" + expression.callee->text + "'");
    return InvalidType();
  }
  facts_.super_members.insert_or_assign(expression.callee.get(),
                                        MemberReference{
                                            .owner = method->second.member_owner,
                                            .member = expression.callee->text,
                                        });
  return CheckFunctionCall(method->second, expression);
}

TypeChecker::Type TypeChecker::CheckMethodReceiver(const Expression& expression,
                                                   const Type& receiver,
                                                   const Type* expected) {
  if (receiver.kind == Type::Kind::Future || receiver.kind == Type::Kind::Completer ||
      receiver.kind == Type::Kind::Timer || receiver.kind == Type::Kind::CancellationToken) {
    return CheckAsyncMethod(expression, receiver);
  }
  if (receiver.kind == Type::Kind::Array) {
    return CheckArrayMethod(expression, receiver, expected);
  }
  if (receiver.kind == Type::Kind::Set || receiver.kind == Type::Kind::Map) {
    return CheckCollectionMethod(expression, receiver);
  }
  if (const auto builtin = FindBuiltin(receiver.kind, expression.callee->text)) {
    return CheckBuiltinCall(expression, *builtin);
  }
  if (receiver.kind == Type::Kind::Interface) {
    const auto& owner = interfaces_.at(receiver.name);
    if (const auto applied =
            CheckMethodApplication(owner.method_templates, *expression.callee, &expression,
                                   expected, MethodAccess::Instance)) {
      return *applied;
    }
    const auto method = owner.methods.find(expression.callee->text);
    if (method == owner.methods.end()) {
      AddDiagnostic(expression.location, "expression is not callable");
      return InvalidType();
    }
    facts_.interface_members.insert_or_assign(
        &expression,
        MemberReference{.owner = std::string(receiver.name), .member = expression.callee->text});
    return CheckFunctionCall(method->second, expression);
  }
  if (receiver.kind == Type::Kind::Class) {
    return CheckInstanceCall(expression, receiver, expected);
  }
  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckStaticCall(const Expression& expression,
                                               const ClassInfo& owner,
                                               const Type* expected) {
  if (const auto applied = CheckMethodApplication(owner.method_templates, *expression.callee,
                                                  &expression, expected, MethodAccess::Static)) {
    return *applied;
  }
  const auto method = owner.methods.find(expression.callee->text);
  if (method != owner.methods.end() && method->second.storage == MemberStorage::Static) {
    CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
    facts_.static_members.insert_or_assign(
        &expression,
        MemberReference{.owner = method->second.member_owner, .member = expression.callee->text});
    return CheckFunctionCall(method->second, expression);
  }
  return CheckIndirectCall(expression, CheckExpression(*expression.callee, nullptr));
}

TypeChecker::Type TypeChecker::CheckInstanceCall(const Expression& expression,
                                                 const Type& object,
                                                 const Type* expected) {
  const auto class_info = classes_.find(object.name);
  if (class_info != classes_.end()) {
    const auto& owner = class_info->second;
    if (const auto applied =
            CheckMethodApplication(owner.method_templates, *expression.callee, &expression,
                                   expected, MethodAccess::Instance)) {
      return *applied;
    }
    const auto method = owner.methods.find(expression.callee->text);
    if (method != owner.methods.end()) {
      CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
      if (method->second.storage == MemberStorage::Static) {
        AddDiagnostic(expression.location, "static method must be accessed through its class");
      }
      if (method->second.is_getter) {
        const auto type = method->second.return_type;
        facts_.expression_types.insert_or_assign(expression.callee.get(), type);
        InvalidateCapturedRefinements();
        return CheckIndirectCall(expression, type);
      }
      facts_.instance_members.insert_or_assign(
          &expression,
          MemberReference{.owner = method->second.member_owner, .member = expression.callee->text});
      return CheckFunctionCall(method->second, expression);
    }
    const auto field = owner.fields.find(expression.callee->text);
    if (field != owner.fields.end() &&
        (field->second.type.kind == Type::Kind::Dynamic ||
         (field->second.type.kind == Type::Kind::Function && !field->second.type.nullable))) {
      CheckAccess(field->second.visibility, object.name, expression.location);
      facts_.expression_types.insert_or_assign(expression.callee.get(), field->second.type);
      return CheckIndirectCall(expression, field->second.type);
    }
  }
  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckBuiltinCall(const Expression& expression, Builtin builtin) {
  facts_.builtin_calls.insert_or_assign(&expression, builtin);
  return CheckFunctionCall(BuiltinSignature(builtin), expression);
}

std::optional<std::size_t> TypeChecker::CallParameterIndex(const FunctionInfo& function,
                                                           const CallArgument& argument,
                                                           std::size_t positional) {
  const auto& parameters = function.parameters;
  if (!argument.name.empty()) {
    const auto found = function.parameter_indices.find(argument.name);
    if (found == function.parameter_indices.end()) {
      AddDiagnostic(argument.location, "unknown named argument '" + argument.name + "'");
      return std::nullopt;
    }
    const auto index = found->second;
    if (parameters.at(index).rest) {
      AddDiagnostic(argument.location, "rest arguments must be positional");
      return std::nullopt;
    }
    return index;
  }
  if (positional < parameters.size()) {
    return positional;
  }
  if (!parameters.empty() && parameters.back().rest) {
    return parameters.size() - 1;
  }
  AddDiagnostic(argument.location, "too many call arguments");
  return std::nullopt;
}

TypeChecker::Type TypeChecker::CheckFunctionCall(
    const FunctionInfo& function,
    const Expression& expression,
    std::span<const std::optional<Type>> checked_arguments,
    CallArgumentNames argument_names) {
  facts_.call_targets.insert_or_assign(
      &expression, CallDescriptor{.signature = function, .argument_names = argument_names});
  const auto& arguments = expression.arguments;
  const auto location = expression.location;
  std::vector<bool> assigned(function.parameters.size(), false);
  std::vector<std::uint32_t> order;
  order.reserve(arguments.size());
  std::size_t positional = 0;
  bool named = false;
  for (std::size_t source_index = 0; source_index < arguments.size(); ++source_index) {
    const auto& argument = arguments.at(source_index);
    const auto parameter_index = CallParameterIndex(function, argument, positional);
    if (!argument.name.empty()) {
      named = true;
    } else {
      if (named) {
        AddDiagnostic(argument.location, "positional argument cannot follow a named argument");
      }
      ++positional;
    }
    if (!parameter_index) {
      continue;
    }
    const auto index = *parameter_index;
    const auto& parameter = function.parameters.at(index);
    if (assigned.at(index) && !parameter.rest) {
      AddDiagnostic(argument.location, "call argument is specified more than once");
      continue;
    }
    assigned.at(index) = true;
    order.push_back(static_cast<std::uint32_t>(index));
    const auto expected = parameter.type;
    const auto checked = checked_arguments.empty() ? std::nullopt : checked_arguments[source_index];
    const Type actual = checked ? *checked : CheckExpression(*argument.value, &expected);
    RecordConversion(*argument.value, expected, actual);
    if (IsValid(actual) && !CanAssign(expected, actual)) {
      AddDiagnostic(argument.location, "call argument type does not match parameter type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    const auto& parameter = function.parameters.at(index);
    if (!assigned.at(index) && !parameter.rest &&
        !function.optional_parameters.contains(parameter.name)) {
      AddDiagnostic(location, "missing argument '" + parameter.name + "'");
    }
  }
  facts_.argument_order.insert_or_assign(&expression, std::move(order));
  return function.return_type;
}

TypeChecker::Type TypeChecker::LookupName(const Expression& expression, const Type* expected) {
  const auto* local = LookupLocal(expression.text);
  if (local != nullptr) {
    CaptureLocal(expression, *local);
    const auto* refined = refinements_.Find(local->id);
    return refined == nullptr ? local->type : *refined;
  }
  if (globals_.contains(expression.text)) {
    return ResolveGlobal(expression.text);
  }
  if (specializer_->FunctionTemplate(expression.text) != nullptr) {
    return InstantiateGeneric(expression, nullptr, expected);
  }
  if (const auto function = functions_.find(expression.text); function != functions_.end()) {
    return FunctionType(function->second);
  }
  AddDiagnostic(expression.location, "unknown name '" + expression.text + "'");
  return InvalidType();
}

const TypeChecker::LocalBinding* TypeChecker::LookupLocal(std::string_view name) const {
  for (const auto& scope : std::views::reverse(scopes_)) {
    const auto value = scope.find(name);
    if (value != scope.end()) {
      return &value->second;
    }
  }
  return nullptr;
}

void TypeChecker::BeginScope() {
  scopes_.emplace_back();
}

void TypeChecker::EndScope() {
  for (const auto& [name, binding] : scopes_.back()) {
    refinements_.Erase(binding.id);
  }
  scopes_.pop_back();
}

TypeChecker::CheckStatus TypeChecker::DeclareLocal(std::string_view name,
                                                   const Type& type,
                                                   SourceLocation location,
                                                   BindingKind kind,
                                                   const void* declaration) {
  auto& scope = scopes_.back();
  if (name == kReceiverName) {
    AddDiagnostic(location, "'this' is reserved for the method receiver");
    return CheckStatus::Failed;
  }
  const auto binding_id = next_binding_id_++;
  if (!scope
           .try_emplace(name,
                        LocalBinding{
                            .type = type,
                            .kind = kind,
                            .id = binding_id,
                            .scope = scopes_.size() - 1,
                        })
           .second) {
    AddDiagnostic(location, "duplicate local name '" + std::string(name) + "'");
    return CheckStatus::Failed;
  }
  if (captured_declarations_.contains(declaration)) {
    captured_mutables_.insert(binding_id);
  }
  if (declaration != nullptr) {
    facts_.bindings.emplace(declaration, binding_id);
  }
  return CheckStatus::Succeeded;
}

bool TypeChecker::IsAssignable(const Expression& expression) {
  return expression.kind == Expression::Kind::Name || expression.kind == Expression::Kind::Member ||
         expression.kind == Expression::Kind::Index;
}

void TypeChecker::CheckVariableDeclaration(const Statement& statement) {
  const bool inferred = statement.type.kind == TypeName::Kind::Inferred;
  Type type = inferred ? InvalidType() : ResolveType(statement.type, VoidTypePolicy::Forbidden);
  const Type initializer = CheckExpression(*statement.expression, inferred ? nullptr : &type);
  if (!inferred) {
    RecordConversion(*statement.expression, type, initializer);
  }
  if (inferred) {
    type = initializer;
    if (type.kind == Type::Kind::Null) {
      AddDiagnostic(statement.location, "null initializer requires an explicit nullable type");
      type = InvalidType();
    }
  }
  if (type.kind == Type::Kind::Void) {
    AddDiagnostic(statement.location, "void is not a value type");
  }
  const bool valid = IsValid(type) && IsValid(initializer);
  const bool assignable = valid && CanAssign(type, initializer);
  if (valid && !assignable) {
    AddDiagnostic(statement.location, "cannot initialize '" + TypeNameFor(type) + "' with '" +
                                          TypeNameFor(initializer) + "'");
  }
  const auto constant = statement.binding == BindingKind::Constant
                            ? EvaluateConstant(*statement.expression)
                            : std::nullopt;
  facts_.local_types.insert_or_assign(&statement, type);
  const auto declared =
      DeclareLocal(statement.name, type, statement.location, statement.binding, &statement);
  if (declared == CheckStatus::Failed) {
    return;
  }
  const auto& local = *LookupLocal(statement.name);
  if (assignable) {
    RefineAssignedLocal(local, initializer);
  }
  if (constant) {
    constant_bindings_.insert_or_assign(local.id, *constant);
  }
}

void TypeChecker::CheckReturn(const Statement& statement, const Type& return_type) {
  if (!IsValid(return_type) && !lambda_scopes_.empty()) {
    InferLambdaReturn(statement);
    return;
  }
  Type expected_type = return_type;
  if (in_async_function_ && return_type.kind == Type::Kind::Future) {
    const auto arguments = generic_arguments_.find(return_type.name);
    if (arguments != generic_arguments_.end() && arguments->second.size() == 1) {
      expected_type = arguments->second.front();
    }
  }
  if (expected_type.kind == Type::Kind::Void) {
    CheckVoidReturn(statement, return_type);
    return;
  }
  if (!statement.expression) {
    AddDiagnostic(statement.location, "non-void function must return a value");
  } else {
    const Type value = CheckExpression(*statement.expression, &expected_type);
    if (!in_async_function_ || value.kind != Type::Kind::Dynamic) {
      RecordConversion(*statement.expression, expected_type, value);
    } else {
      DiagnoseDynamicConversion(*statement.expression, expected_type, value);
    }
    const bool adopts_future = in_async_function_ && return_type.kind == Type::Kind::Future &&
                               CanAssign(return_type, value);
    if (IsValid(value) && !CanAssign(expected_type, value) && !adopts_future) {
      AddDiagnostic(statement.location, "cannot return '" + TypeNameFor(value) +
                                            "' from function returning '" +
                                            TypeNameFor(expected_type) + "'");
    }
  }
}

void TypeChecker::CheckVoidReturn(const Statement& statement, const Type& return_type) {
  if (!statement.expression) {
    return;
  }
  const Type expected{.kind = Type::Kind::Void, .name = kVoidTypeName};
  const auto value = CheckExpression(*statement.expression, &expected);
  const bool async_payload =
      in_async_function_ && (value.kind == Type::Kind::Dynamic || CanAssign(return_type, value));
  if (IsValid(value) && value.kind != Type::Kind::Void && !async_payload) {
    AddDiagnostic(statement.location, "void function cannot return a value");
  }
}

TypeChecker::Type TypeChecker::CheckIntegerLiteral(const Expression& expression) {
  if (!expression.IntegerValue()) {
    AddDiagnostic(expression.location, "integer literal is out of range");
    return InvalidType();
  }
  return {.kind = Type::Kind::Int, .name = kIntTypeName};
}

TypeChecker::Type TypeChecker::CheckIndex(const Expression& expression) {
  Type object = CheckExpression(*expression.object, nullptr);
  object = ParameterCapability(object, expression.object->location);
  const Type index = CheckExpression(*expression.index, nullptr);
  const Type integer{.kind = Type::Kind::Int, .name = kIntTypeName};
  RecordConversion(*expression.index, integer, index);
  if (!CanAssign(integer, index)) {
    AddDiagnostic(expression.index->location, "index must be int");
  }
  if (object.kind == Type::Kind::Dynamic) {
    return object;
  }
  if (CheckReceiverNullability(expression, object) == CheckStatus::Failed) {
    return InvalidType();
  }
  if (object.kind != Type::Kind::Array && object.kind != Type::Kind::String &&
      object.kind != Type::Kind::ByteArray && object.kind != Type::Kind::RegExpMatch) {
    AddDiagnostic(expression.location,
                  "indexing requires an array, ByteArray, string, or RegExpMatch");
  }
  if (object.kind == Type::Kind::String) {
    facts_.builtin_calls.insert_or_assign(&expression, Builtin::StringIndex);
    return expression.null_safe ? NullableType(object) : object;
  }
  if (object.kind == Type::Kind::RegExpMatch) {
    facts_.builtin_calls.insert_or_assign(&expression, Builtin::RegExpMatchGroup);
    return NullableType(Type{.kind = Type::Kind::String, .name = kStringTypeName});
  }
  if (object.kind == Type::Kind::ByteArray) {
    Type result{.kind = Type::Kind::Int, .name = kIntTypeName};
    return expression.null_safe ? NullableType(result) : result;
  }
  if (object.kind != Type::Kind::Array) {
    return InvalidType();
  }
  auto result = array_elements_.at(object.name);
  if (expression.null_safe) {
    result = NullableType(result);
  }
  return result;
}

TypeChecker::Type TypeChecker::CheckArrayLiteral(const Expression& expression,
                                                 const Type* expected) {
  Type element_type;
  if (expected != nullptr && expected->kind == Type::Kind::Dynamic) {
    element_type = *expected;
  }
  if (expected != nullptr && expected->kind == Type::Kind::Array) {
    const auto array = NonNullable(*expected);
    element_type = array_elements_.at(array.name);
  }
  for (const auto& element : expression.elements) {
    const Type actual = CheckExpression(*element, IsValid(element_type) ? &element_type : nullptr);
    if (IsValid(element_type)) {
      RecordConversion(*element, element_type, actual);
    }
    if (!IsValid(actual)) {
      continue;
    }
    if (actual.kind == Type::Kind::Void) {
      AddDiagnostic(element->location, "array elements must be values");
    } else if (actual.kind == Type::Kind::Null && !IsValid(element_type)) {
      AddDiagnostic(element->location, "null array element requires an expected nullable type");
    } else if (!IsValid(element_type)) {
      element_type = actual;
    } else if (!CanAssign(element_type, actual)) {
      AddDiagnostic(element->location,
                    "array element type does not match '" + TypeNameFor(element_type) + "'");
    }
  }
  if (!IsValid(element_type)) {
    if (expression.elements.empty()) {
      AddDiagnostic(expression.location, "empty array requires an expected array type");
    }
    return InvalidType();
  }
  return ArrayType(element_type);
}

TypeChecker::Type TypeChecker::CheckUnary(const Expression& expression) {
  Type operand = CheckExpression(*expression.operand, nullptr);
  operand = ParameterCapability(operand, expression.operand->location);
  if (expression.operation == TokenKind::PlusPlus ||
      expression.operation == TokenKind::MinusMinus) {
    CheckBuiltinAssignment(*expression.operand);
    if (!IsAssignable(*expression.operand)) {
      AddDiagnostic(expression.operand->location, "increment operand is not assignable");
    }
  }
  if (expression.operation == TokenKind::Bang) {
    CheckBooleanOperand(*expression.operand, operand, "'!' requires a bool operand");
    return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
  }
  if (operand.kind == Type::Kind::Dynamic) {
    if (expression.operation == TokenKind::Minus) {
      return operand;
    }
    const Type integer{.kind = Type::Kind::Int, .name = kIntTypeName};
    RecordConversion(*expression.operand, integer, operand);
    return integer;
  }
  if ((expression.operation == TokenKind::Tilde || expression.operation == TokenKind::PlusPlus ||
       expression.operation == TokenKind::MinusMinus) &&
      (operand.kind != Type::Kind::Int || operand.nullable)) {
    DiagnoseOperandMismatch(*expression.operand, operand, "integer unary operator requires int");
    return InvalidType();
  }
  if (expression.operation == TokenKind::Minus && !IsNumeric(operand)) {
    DiagnoseOperandMismatch(*expression.operand, operand, "unary '-' requires a numeric operand");
    return InvalidType();
  }
  return operand;
}

TypeChecker::Type TypeChecker::RecordDynamicOperands(const Expression& expression,
                                                     const Type& left,
                                                     const Type& right) {
  const Type dynamic{.kind = Type::Kind::Dynamic, .name = kDynamicTypeName};
  RecordConversion(*expression.left, dynamic, left);
  RecordConversion(*expression.right, dynamic, right);
  return dynamic;
}

TypeChecker::Type TypeChecker::CheckConditional(const Expression& expression,
                                                const Type* expected) {
  const auto condition = CheckExpression(*expression.operand, nullptr);
  CheckBooleanOperand(*expression.operand, condition, "conditional condition must be bool");
  const auto true_branch = refinements_.BeginBranch();
  RefineCondition(*expression.operand, ConditionTruth::AlwaysTrue);
  auto when_true = CheckExpression(*expression.left, expected);
  auto true_facts = refinements_.Capture(true_branch);
  const auto false_branch = refinements_.BeginBranch();
  RefineCondition(*expression.operand, ConditionTruth::AlwaysFalse);
  auto when_false = CheckExpression(*expression.right, expected);
  const auto false_facts = refinements_.Capture(false_branch);
  refinements_.Intersect(true_facts, false_facts);
  refinements_.Apply(true_facts);
  if ((when_true.kind == Type::Kind::Dynamic || when_false.kind == Type::Kind::Dynamic) &&
      when_true.kind != Type::Kind::Void && when_false.kind != Type::Kind::Void) {
    return RecordDynamicOperands(expression, when_true, when_false);
  }
  if (CanAssign(when_true, when_false)) {
    return when_true;
  }
  if (CanAssign(when_false, when_true)) {
    return when_false;
  }
  AddDiagnostic(expression.location, "conditional branches must have a common type");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckCompoundAssignment(const Expression& expression) {
  if (CheckAssignmentTarget(*expression.left) == CheckStatus::Failed) {
    return InvalidType();
  }
  const auto binding = CheckAssignedBinding(*expression.left);
  const auto left = CheckExpression(*expression.left, nullptr);
  const auto right = CheckExpression(*expression.right, &left);
  if (expression.left->kind == Expression::Kind::Member) {
    CheckMemberAssignment(*expression.left);
  }
  if (left.kind == Type::Kind::Dynamic || right.kind == Type::Kind::Dynamic) {
    RecordConversion(*expression.right, left, right);
    if (binding) {
      refinements_.Erase(binding->id);
    }
    return left.kind == Type::Kind::Dynamic ? left
                                            : CheckCompoundAssignmentTypes(expression, left, left);
  }
  CheckBuiltinAssignment(*expression.left);
  if (!IsValid(left) || !IsValid(right)) {
    return InvalidType();
  }
  return CheckCompoundAssignmentTypes(expression, left, right);
}

TypeChecker::Type TypeChecker::CheckCompoundAssignmentTypes(const Expression& expression,
                                                            const Type& left,
                                                            const Type& right) {
  const auto operation = expression.operation;
  if (!IsSameType(left, right)) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "compound assignment operands must have the same type");
    return InvalidType();
  }
  const bool bitwise = operation == TokenKind::Ampersand || operation == TokenKind::Pipe ||
                       operation == TokenKind::Caret || operation == TokenKind::ShiftLeft ||
                       operation == TokenKind::ShiftRight ||
                       operation == TokenKind::ShiftUnsignedRight;
  if (bitwise && (left.kind != Type::Kind::Int || left.nullable)) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "bitwise assignment requires int operands");
    return InvalidType();
  }
  if (operation == TokenKind::Percent && (left.kind != Type::Kind::Int || left.nullable)) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "remainder assignment requires int operands");
    return InvalidType();
  }
  if (left.kind == Type::Kind::String && operation == TokenKind::Plus) {
    return left;
  }
  if (!IsNumeric(left) && !bitwise) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "compound assignment requires numeric operands");
    return InvalidType();
  }
  return left;
}

TypeChecker::Type TypeChecker::CheckBinary(const Expression& expression, const Type* expected) {
  if (expression.operation == TokenKind::Equal) {
    return CheckAssignment(expression);
  }
  const bool coalesce = expression.operation == TokenKind::QuestionQuestion && expected != nullptr;
  const Type left_context = coalesce ? NullableType(*expected) : InvalidType();
  Type left = CheckExpression(*expression.left, coalesce ? &left_context : nullptr);
  const bool logical =
      expression.operation == TokenKind::AndAnd || expression.operation == TokenKind::OrOr;
  std::optional<Refinements::Checkpoint> after_left;
  if (logical) {
    after_left = refinements_.BeginBranch();
    RefineCondition(*expression.left, expression.operation == TokenKind::AndAnd
                                          ? ConditionTruth::AlwaysTrue
                                          : ConditionTruth::AlwaysFalse);
  }
  const Type right = CheckExpression(*expression.right, coalesce ? expected : nullptr);
  if (logical) {
    auto changes = refinements_.Capture(*after_left);
    refinements_.Intersect(changes, {});
    refinements_.Apply(changes);
  }
  if (!IsValid(left) || !IsValid(right)) {
    return InvalidType();
  }
  return CheckBinaryTypes(expression, left, right);
}

TypeChecker::Type TypeChecker::CheckEquality(const Expression& expression,
                                             const Type& left,
                                             const Type& right) {
  if (left.kind == Type::Kind::Void || right.kind == Type::Kind::Void) {
    AddDiagnostic(expression.location, "equality requires value operands");
    return InvalidType();
  }
  const bool null_comparison = left.kind == Type::Kind::Null || right.kind == Type::Kind::Null;
  if (null_comparison) {
    return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
  }
  if (left.kind == Type::Kind::Parameter || right.kind == Type::Kind::Parameter) {
    return CheckBoundBinaryTypes(expression, left, right);
  }
  if (left.kind == Type::Kind::Dynamic || right.kind == Type::Kind::Dynamic) {
    static_cast<void>(RecordDynamicOperands(expression, left, right));
    return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
  }
  if (left.kind == Type::Kind::Array || left.kind == Type::Kind::MutableString ||
      left.kind == Type::Kind::Function || left.kind == Type::Kind::Set ||
      left.kind == Type::Kind::Map) {
    AddDiagnostic(expression.location, "equality is not defined for this type");
    return InvalidType();
  }
  if (!IsSameType(NonNullable(left), NonNullable(right))) {
    AddDiagnostic(expression.location, "comparison operands must have the same type");
    return InvalidType();
  }
  return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
}

TypeChecker::Type TypeChecker::CheckBinaryTypes(const Expression& expression,
                                                const Type& left,
                                                const Type& right) {
  if (expression.operation == TokenKind::EqualEqual ||
      expression.operation == TokenKind::BangEqual) {
    return CheckEquality(expression, left, right);
  }
  if (expression.operation == TokenKind::AndAnd || expression.operation == TokenKind::OrOr) {
    return CheckLogicalTypes(expression, left, right);
  }
  if (expression.operation == TokenKind::QuestionQuestion) {
    return CheckCoalesceTypes(expression, left, right);
  }
  if (left.kind == Type::Kind::Parameter || right.kind == Type::Kind::Parameter) {
    return CheckBoundBinaryTypes(expression, left, right);
  }
  if ((left.kind == Type::Kind::Dynamic || right.kind == Type::Kind::Dynamic) &&
      left.kind != Type::Kind::Void && right.kind != Type::Kind::Void) {
    const auto dynamic = RecordDynamicOperands(expression, left, right);
    const bool comparison = expression.operation == TokenKind::Less ||
                            expression.operation == TokenKind::LessEqual ||
                            expression.operation == TokenKind::Greater ||
                            expression.operation == TokenKind::GreaterEqual;
    return comparison ? Type{.kind = Type::Kind::Bool, .name = kBoolTypeName} : dynamic;
  }
  if (expression.operation == TokenKind::Ampersand || expression.operation == TokenKind::Pipe ||
      expression.operation == TokenKind::Caret || expression.operation == TokenKind::ShiftLeft ||
      expression.operation == TokenKind::ShiftRight ||
      expression.operation == TokenKind::ShiftUnsignedRight ||
      expression.operation == TokenKind::Percent) {
    return CheckIntegerTypes(expression, left, right);
  }
  if (expression.operation == TokenKind::Less || expression.operation == TokenKind::LessEqual ||
      expression.operation == TokenKind::Greater ||
      expression.operation == TokenKind::GreaterEqual) {
    return CheckRelationalTypes(expression, left, right);
  }
  if (expression.operation == TokenKind::Plus &&
      (left.kind == Type::Kind::String || right.kind == Type::Kind::String) &&
      CanFormatString(left) && CanFormatString(right)) {
    return {.kind = Type::Kind::String, .name = kStringTypeName};
  }
  if (!IsNumeric(left) || !IsSameType(left, right)) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "arithmetic operands must have the same numeric type");
    return InvalidType();
  }
  return left;
}

TypeChecker::Type TypeChecker::CheckBoundBinaryTypes(const Expression& expression,
                                                     const Type& left,
                                                     const Type& right) {
  const auto left_capability = ParameterCapability(left, expression.left->location);
  const auto right_capability = ParameterCapability(right, expression.right->location);
  if (!IsValid(left_capability) || !IsValid(right_capability)) {
    return InvalidType();
  }
  return CheckBinaryTypes(expression, left_capability, right_capability);
}

TypeChecker::Type TypeChecker::CheckLogicalTypes(const Expression& expression,
                                                 const Type& left,
                                                 const Type& right) {
  CheckBooleanOperand(*expression.left, left, "logical operands must be bool");
  CheckBooleanOperand(*expression.right, right, "logical operands must be bool");
  return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
}

TypeChecker::Type TypeChecker::CheckCoalesceTypes(const Expression& expression,
                                                  const Type& left,
                                                  const Type& right) {
  const auto* local = expression.left->kind == Expression::Kind::Name
                          ? LookupLocal(expression.left->text)
                          : nullptr;
  // 非空事实只描述当前读取；?? 的可空契约仍由局部变量的声明类型决定。
  const auto source = local != nullptr && local->type.nullable ? local->type : left;
  if (!source.nullable && source.kind != Type::Kind::Null && source.kind != Type::Kind::Dynamic) {
    AddDiagnostic(expression.left->location, "left operand of coalesce must be nullable");
  }
  if (left.kind == Type::Kind::Dynamic || right.kind == Type::Kind::Dynamic) {
    return RecordDynamicOperands(expression, left, right);
  }
  if (CanAssign(source, right)) {
    return right;
  }
  if (CanAssign(right, source)) {
    return source;
  }
  AddDiagnostic(expression.location, "coalesce operands must have a common type");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckIntegerTypes(const Expression& expression,
                                                 const Type& left,
                                                 const Type& right) {
  if (left.kind != Type::Kind::Int || right.kind != Type::Kind::Int || left.nullable ||
      right.nullable) {
    const std::string_view message = expression.operation == TokenKind::Percent
                                         ? "remainder operands must be int"
                                         : "bitwise operands must be int";
    DiagnoseBinaryOperandMismatch(expression, left, right, message);
    return InvalidType();
  }
  return left;
}

TypeChecker::Type TypeChecker::CheckRelationalTypes(const Expression& expression,
                                                    const Type& left,
                                                    const Type& right) {
  if (!IsNumeric(left) || !IsSameType(left, right)) {
    DiagnoseBinaryOperandMismatch(expression, left, right,
                                  "relational operands must have the same numeric type");
    return InvalidType();
  }
  return {.kind = Type::Kind::Bool, .name = kBoolTypeName};
}

TypeChecker::Type TypeChecker::CheckAssignment(const Expression& expression) {
  if (CheckAssignmentTarget(*expression.left) == CheckStatus::Failed) {
    return InvalidType();
  }
  const auto binding = CheckAssignedBinding(*expression.left);
  Type left = CheckExpression(*expression.left, nullptr);
  if (expression.left->kind == Expression::Kind::Member) {
    CheckMemberAssignment(*expression.left);
  }
  if (binding) {
    left = binding->type;
  }
  CheckBuiltinAssignment(*expression.left);
  const Type right = CheckExpression(*expression.right, &left);
  RecordConversion(*expression.right, left, right);
  const bool valid = IsValid(left) && IsValid(right);
  const bool assignable = valid && CanAssign(left, right);
  if (binding) {
    RefineAssignedLocal(*binding, assignable ? right : InvalidType());
  }
  if (valid && !assignable) {
    AddDiagnostic(expression.location, "assignment types do not match");
    return InvalidType();
  }
  return left;
}

std::optional<TypeChecker::LocalBinding> TypeChecker::CheckAssignedBinding(
    const Expression& target) {
  if (target.kind != Expression::Kind::Name) {
    return std::nullopt;
  }
  const auto* local = LookupLocal(target.text);
  if (local == nullptr) {
    const auto global = globals_.find(target.text);
    if (global == globals_.end()) {
      AddDiagnostic(target.location, "cannot assign to an immutable global binding");
    } else if (global->second.declaration->binding == BindingKind::Constant) {
      AddDiagnostic(target.location, "cannot assign to const binding '" + target.text + "'");
    }
    return std::nullopt;
  }
  if (local->kind == BindingKind::Constant) {
    AddDiagnostic(target.location, "cannot assign to const binding '" + target.text + "'");
  }
  // 后续表达式检查可扩展 lambda 作用域表，调用方跨检查持有绑定值。
  return *local;
}

TypeChecker::CheckStatus TypeChecker::CheckReceiverNullability(const Expression& access,
                                                               Type& receiver) {
  if (receiver.nullable) {
    if (!access.null_safe) {
      AddDiagnostic(access.object->location, "nullable receiver requires a non-null check");
      return CheckStatus::Failed;
    }
    receiver = NonNullable(receiver);
  }
  return CheckStatus::Succeeded;
}

void TypeChecker::CheckBuiltinAssignment(const Expression& target) {
  if (facts_.builtin_calls.contains(&target)) {
    AddDiagnostic(target.location, "builtin properties and indexed values are immutable");
  }
}

TypeChecker::CheckStatus TypeChecker::CheckAssignmentTarget(const Expression& target) {
  if (!IsAssignable(target)) {
    AddDiagnostic(target.location, "left side of assignment is not assignable");
    return CheckStatus::Failed;
  }
  return CheckStatus::Succeeded;
}

void TypeChecker::CheckMemberAssignment(const Expression& member) {
  bool read_only =
      facts_.interface_members.contains(&member) || facts_.builtin_method_values.contains(&member);
  if (const auto found = facts_.static_members.find(&member);
      found != facts_.static_members.end()) {
    const auto& owner = classes_.at(found->second.owner);
    const auto source_id = owner.declaration->location.source_id;
    read_only = read_only || owner.methods.contains(found->second.member) ||
                (source_id < sources_.size() &&
                 sources_[source_id].path.starts_with(kStandardSourcePrefix));
  } else if (const auto object = facts_.expression_types.find(member.object.get());
             object != facts_.expression_types.end() && object->second.kind == Type::Kind::Class) {
    const auto& methods = classes_.at(NonNullable(object->second).name).methods;
    const auto method = methods.find(member.text);
    if (method != methods.end()) {
      if (method->second.is_setter) {
        read_only = read_only || !method->second.setter_parameter_type.has_value();
      } else {
        read_only = true;
      }
    }
  } else if (member.object->kind == Expression::Kind::Name &&
             LookupLocal(member.object->text) == nullptr) {
    const auto type = types_.find(member.object->text);
    read_only = read_only || (type != types_.end() && type->second.kind == Type::Kind::Enum);
  }
  if (read_only) {
    AddDiagnostic(member.location, "methods and enum members are immutable");
  }
}

}  // namespace vela::frontend
