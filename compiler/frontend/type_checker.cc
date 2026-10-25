#include "compiler/frontend/type_checker.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/capture_analysis.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/initialization.h"
#include "compiler/frontend/standard_environment.h"

namespace vela::frontend {

auto TypeChecker::InvalidType() -> TypeChecker::Type {
  return {};
}

auto TypeChecker::Check(const Program& program) -> TypeCheckResult {
  diagnostics_.clear();
  function_count_ = program.functions.size();
  types_.clear();
  array_elements_.clear();
  function_types_.clear();
  aliases_.clear();
  resolving_aliases_.clear();
  lambdas_.clear();
  lambda_indices_.clear();
  lambda_scopes_.clear();
  bindings_.clear();
  captured_mutables_.clear();
  captured_declarations_ = FindCapturedMutableDeclarations(program);
  next_binding_id_ = 0;
  expression_types_.clear();
  case_values_.clear();
  enum_values_.clear();
  builtin_calls_.clear();
  array_algorithms_.clear();
  local_types_.clear();
  control_flow_.clear();
  classes_.clear();
  functions_.clear();
  static_members_.clear();
  globals_.clear();
  checking_globals_.clear();
  checked_globals_.clear();
  interfaces_.clear();
  interface_members_.clear();
  type_operands_.clear();
  generic_arguments_.clear();
  expected_calls_.clear();
  match_bindings_.clear();
  catch_types_.clear();
  enum_sizes_.clear();
  argument_order_.clear();
  collection_constructions_.clear();
  scopes_.clear();
  loop_depth_ = 0;
  switch_depth_ = 0;
  sources_ = program.sources;

  CollectTypes(program);
  ResolveInterfaces(program);
  ResolveDeclarations(program);
  ResolveGlobals(program);
  CheckImplementations(program);
  for (const auto& alias : program.aliases) {
    static_cast<void>(
        ResolveType({.name = alias.name, .location = alias.location, .arguments = {}}, true));
  }
  // 描述表在检查函数体前完成构建；检查期间仅修改作用域表，借用的描述信息保持有效。
  for (const Function& function : program.functions) {
    CheckFunction(function, nullptr);
  }
  for (const Class& class_declaration : program.classes) {
    const auto class_info = classes_.find(class_declaration.name);
    if (class_info == classes_.end()) {
      continue;
    }
    CheckFieldInitializers(class_declaration, class_info->second);
    for (const Function& method : class_declaration.methods) {
      CheckFunction(method, &class_info->second);
    }
    if (class_declaration.constructor) {
      CheckFunction(*class_declaration.constructor, &class_info->second);
    }
  }

  SemanticModel model;
  model.globals = std::move(globals_);
  model.types = std::move(types_);
  model.array_elements = std::move(array_elements_);
  model.function_types = std::move(function_types_);
  model.lambdas = std::move(lambdas_);
  model.lambda_indices = std::move(lambda_indices_);
  model.bindings = std::move(bindings_);
  model.captured_mutables = std::move(captured_mutables_);
  model.expression_types = std::move(expression_types_);
  model.case_values = std::move(case_values_);
  model.enum_values = std::move(enum_values_);
  model.builtin_calls = std::move(builtin_calls_);
  model.array_algorithms = std::move(array_algorithms_);
  model.local_types = std::move(local_types_);
  model.control_flow = std::move(control_flow_);
  model.classes = std::move(classes_);
  model.functions = std::move(functions_);
  model.static_members = std::move(static_members_);
  model.interfaces = std::move(interfaces_);
  model.interface_members = std::move(interface_members_);
  model.type_operands = std::move(type_operands_);
  model.generic_arguments = std::move(generic_arguments_);
  model.expected_calls = std::move(expected_calls_);
  model.match_bindings = std::move(match_bindings_);
  model.catch_types = std::move(catch_types_);
  model.argument_order = std::move(argument_order_);
  model.collection_constructions = std::move(collection_constructions_);
  if (diagnostics_.empty()) {
    for (const auto& owner : program.classes) {
      const auto initialization = CheckConstructorInitialization(owner, model);
      for (const auto& diagnostic : initialization) {
        AddDiagnostic(diagnostic.location, diagnostic.message);
      }
    }
  }
  return {
      .diagnostics = std::move(diagnostics_),
      .function_count = function_count_,
      .model = std::move(model),
  };
}

void TypeChecker::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = location.source_id < sources_.size()
                         ? sources_.subspan(location.source_id).front().path
                         : std::string(),
  });
}

auto TypeChecker::ResolveSignature(const Function& function) -> FunctionInfo {
  FunctionInfo info;
  info.visibility = function.visibility;
  info.storage = function.storage;
  info.return_type = ResolveType(function.return_type, true);
  info.parameters.reserve(function.parameters.size());
  info.parameter_indices.reserve(function.parameters.size());
  for (const auto& parameter : function.parameters) {
    const Type type = ResolveType(parameter.type, false);
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
  owner_ = owner;
  refinements_.clear();
  constant_bindings_.clear();
  Type return_type;
  const FunctionInfo* function_info = nullptr;
  if (owner == nullptr) {
    const auto found = functions_.find(function.name);
    if (found == functions_.end()) {
      return;
    }
    function_info = &found->second;
  } else {
    const auto found = owner->methods.find(function.name);
    if (found == owner->methods.end()) {
      return;
    }
    function_info = &found->second;
  }
  return_type = function_info->return_type;

  BeginScope();
  if (owner != nullptr && function.storage == MemberStorage::Instance) {
    scopes_.back().try_emplace("this", LocalBinding{
                                           .type = owner->type,
                                           .kind = BindingKind::Constant,
                                           .id = next_binding_id_++,
                                           .scope = scopes_.size() - 1,
                                       });
    bindings_.emplace(&function, LookupLocal("this")->id);
  }
  for (const Parameter& parameter : function.parameters) {
    const auto type =
        function_info->parameters.at(function_info->parameter_indices.at(parameter.name)).type;
    DeclareLocal(parameter.name, type, parameter.location, BindingKind::Mutable, &parameter);
    bindings_.emplace(&parameter, LookupLocal(parameter.name)->id);
  }
  if (function.body) {
    const auto flow = CheckSequence(*function.body, return_type);
    control_flow_.insert_or_assign(function.body.get(), flow);
    if (return_type.kind != Type::Kind::Void && return_type.kind != Type::Kind::Invalid &&
        flow.can_fall_through) {
      AddDiagnostic(function.location, "non-void function may fall through without returning");
    }
  }
  EndScope();
  owner_ = nullptr;
}

auto TypeChecker::CheckExpression(const Expression& expression, const Type* expected)
    -> TypeChecker::Type {
  auto type = CheckExpressionType(expression, expected);
  if (expression.kind == Expression::Kind::Call) {
    InvalidateCapturedRefinements();
  }
  expression_types_.insert_or_assign(&expression, type);
  return type;
}

auto TypeChecker::CheckExpressionType(const Expression& expression, const Type* expected) -> Type {
  switch (expression.kind) {
    case Expression::Kind::Match:
      return CheckMatch(expression, expected);
    case Expression::Kind::TypeTest:
    case Expression::Kind::TypeCast:
      return CheckTypeOperation(expression);
    case Expression::Kind::Lambda:
      return CheckLambda(expression);
    case Expression::Kind::NullLiteral:
      return {.kind = Type::Kind::Null, .name = "null"};
    case Expression::Kind::IntegerLiteral:
      return CheckIntegerLiteral(expression);
    case Expression::Kind::DoubleLiteral:
      return {.kind = Type::Kind::Double, .name = "double"};
    case Expression::Kind::StringLiteral:
      return {.kind = Type::Kind::String, .name = "string"};
    case Expression::Kind::StringInterpolation:
      for (const auto& part : expression.elements) {
        const auto type = CheckExpression(*part, nullptr);
        if (!CanFormatString(type) && IsValid(type)) {
          AddDiagnostic(part->location,
                        "interpolation requires string, int, double, bool, or null");
        }
      }
      return {.kind = Type::Kind::String, .name = "string"};
    case Expression::Kind::ArrayLiteral:
      return CheckArrayLiteral(expression, expected);
    case Expression::Kind::Index:
      return CheckIndex(expression);
    case Expression::Kind::BooleanLiteral:
      return {.kind = Type::Kind::Bool, .name = "bool"};
    case Expression::Kind::Name:
      return LookupName(expression);
    case Expression::Kind::Unary:
      return CheckUnary(expression);
    case Expression::Kind::Binary:
      return CheckBinary(expression);
    case Expression::Kind::Call:
      return CheckCall(expression, expected);
    case Expression::Kind::Member:
      return CheckMemberAccess(expression);
  }
  return InvalidType();
}

auto TypeChecker::CheckCall(const Expression& expression, const Type* expected)
    -> TypeChecker::Type {
  const auto& callee = *expression.callee;
  if (callee.kind == Expression::Kind::Name && LookupLocal(callee.text) == nullptr &&
      (callee.text == "Set" || callee.text == "Map")) {
    return CheckCollectionConstruction(expression, expected);
  }
  if (callee.kind == Expression::Kind::Name && LookupLocal(callee.text) == nullptr &&
      (callee.text == "ok" || callee.text == "err")) {
    return CheckExpectedFactory(expression, expected);
  }
  if (callee.kind == Expression::Kind::Member) {
    return CheckMethodCall(expression, expected);
  }
  if (callee.kind != Expression::Kind::Name || LookupLocal(callee.text) != nullptr) {
    return CheckIndirectCall(expression, CheckExpression(callee, nullptr));
  }
  if (globals_.contains(callee.text)) {
    return CheckIndirectCall(expression, CheckExpression(callee, nullptr));
  }
  return CheckNamedCall(expression, expected);
}

auto TypeChecker::CheckIndirectCall(const Expression& expression, const Type& type) -> Type {
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
  // 检查参数时可能解析新的函数类型，不能跨 rehash 借用描述表内的元素。
  const auto signature = function_types_.at(type.name);
  return CheckFunctionCall(signature, expression);
}

auto TypeChecker::CheckNamedCall(const Expression& expression, const Type* expected) -> Type {
  const auto& callee = *expression.callee;
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
    return {.kind = Type::Kind::Void, .name = "void"};
  }
  std::string constructor_name = callee.text;
  if (aliases_.contains(callee.text)) {
    const auto alias_type =
        ResolveType({.name = callee.text, .location = callee.location, .arguments = {}}, false);
    if (alias_type.nullable) {
      AddDiagnostic(callee.location, "nullable type aliases are not constructible");
      return InvalidType();
    }
    if (alias_type.kind == Type::Kind::Set || alias_type.kind == Type::Kind::Map) {
      return CheckCollectionConstruction(expression, &alias_type);
    }
    constructor_name = alias_type.name;
  }
  const auto class_info = classes_.find(constructor_name);
  if (class_info != classes_.end()) {
    const Type result = CheckClassConstruction(class_info->second, expression);
    if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
      AddDiagnostic(expression.location, "constructed type does not match expected type");
    }
    return result;
  }
  const auto function = functions_.find(callee.text);
  if (function != functions_.end()) {
    const Type result = CheckFunctionCall(function->second, expression);
    if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
      AddDiagnostic(expression.location, "function result does not match expected type");
    }
    return result;
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckMethodCall(const Expression& expression, const Type* expected) -> Type {
  if (const auto* owner = StaticOwner(*expression.callee)) {
    return CheckStaticCall(expression, *owner);
  }
  const auto& receiver = *expression.callee->object;
  Type context;
  if (receiver.kind == Expression::Kind::ArrayLiteral && receiver.elements.empty() &&
      expression.callee->text == "join") {
    context = ArrayType({.kind = Type::Kind::String, .name = "string"});
  }
  const Type object = CheckExpression(receiver, IsValid(context) ? &context : nullptr);
  if (object.nullable) {
    AddDiagnostic(expression.callee->object->location,
                  "nullable receiver requires a non-null check");
    return InvalidType();
  }
  if (object.kind == Type::Kind::Expected) {
    return CheckExpectedMethod(expression, object);
  }
  if (object.kind == Type::Kind::Array) {
    return CheckArrayMethod(expression, object);
  }
  if (object.kind == Type::Kind::Set || object.kind == Type::Kind::Map) {
    return CheckCollectionMethod(expression, object);
  }
  if (const auto builtin = FindBuiltin(object.kind, expression.callee->text)) {
    return CheckBuiltinCall(expression, *builtin);
  }
  if (object.kind == Type::Kind::Interface) {
    const auto& owner = interfaces_.at(object.name);
    const auto method = owner.methods.find(expression.callee->text);
    if (method != owner.methods.end()) {
      interface_members_.insert_or_assign(
          &expression, MemberReference{.owner = object.name, .member = expression.callee->text});
      return CheckFunctionCall(method->second, expression);
    }
  }
  if (object.kind == Type::Kind::Class) {
    return CheckInstanceCall(expression, object, expected);
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckStaticCall(const Expression& expression, const ClassInfo& owner) -> Type {
  const auto method = owner.methods.find(expression.callee->text);
  if (method != owner.methods.end() && method->second.storage == MemberStorage::Static) {
    CheckAccess(method->second.visibility, owner.type.name, expression.location);
    static_members_.insert_or_assign(
        &expression, MemberReference{.owner = owner.type.name, .member = expression.callee->text});
    return CheckFunctionCall(method->second, expression);
  }
  return CheckIndirectCall(expression, CheckExpression(*expression.callee, nullptr));
}

auto TypeChecker::CheckInstanceCall(const Expression& expression,
                                    const Type& object,
                                    const Type* expected) -> Type {
  const auto class_info = classes_.find(object.name);
  if (class_info != classes_.end()) {
    const auto method = class_info->second.methods.find(expression.callee->text);
    if (method != class_info->second.methods.end()) {
      CheckAccess(method->second.visibility, object.name, expression.location);
      if (method->second.storage == MemberStorage::Static) {
        AddDiagnostic(expression.location, "static method must be accessed through its class");
      }
      const Type result = CheckFunctionCall(method->second, expression);
      if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
        AddDiagnostic(expression.location, "method result does not match expected type");
      }
      return result;
    }
    const auto field = class_info->second.fields.find(expression.callee->text);
    if (field != class_info->second.fields.end() &&
        field->second.type.kind == Type::Kind::Function && !field->second.type.nullable) {
      CheckAccess(field->second.visibility, object.name, expression.location);
      expression_types_.insert_or_assign(expression.callee.get(), field->second.type);
      return CheckIndirectCall(expression, field->second.type);
    }
  }
  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckBuiltinCall(const Expression& expression, Builtin builtin) -> Type {
  builtin_calls_.insert_or_assign(&expression, builtin);
  return CheckFunctionCall(BuiltinSignature(builtin), expression);
}

auto TypeChecker::CheckFunctionCall(const FunctionInfo& function,
                                    const Expression& expression,
                                    std::span<const Type> checked_arguments) -> TypeChecker::Type {
  const auto& arguments = expression.arguments;
  const auto location = expression.location;
  std::vector<bool> assigned(function.parameters.size(), false);
  std::vector<std::uint32_t> order;
  std::size_t positional = 0;
  bool named = false;
  for (std::size_t source_index = 0; source_index < arguments.size(); ++source_index) {
    const auto& argument = arguments.at(source_index);
    std::size_t index = positional;
    if (!argument.name.empty()) {
      named = true;
      const auto parameter = function.parameter_indices.find(argument.name);
      if (parameter == function.parameter_indices.end()) {
        AddDiagnostic(argument.location, "unknown named argument '" + argument.name + "'");
        continue;
      }
      index = parameter->second;
    } else {
      if (named) {
        AddDiagnostic(argument.location, "positional argument cannot follow a named argument");
      }
      ++positional;
    }
    if (index >= function.parameters.size()) {
      AddDiagnostic(argument.location, "too many call arguments");
      continue;
    }
    if (assigned.at(index)) {
      AddDiagnostic(argument.location, "call argument is specified more than once");
      continue;
    }
    assigned.at(index) = true;
    order.push_back(static_cast<std::uint32_t>(index));
    const auto expected = function.parameters.at(index).type;
    const Type actual = checked_arguments.empty() ? CheckExpression(*argument.value, &expected)
                                                  : checked_arguments.subspan(source_index).front();
    if (IsValid(actual) && !CanAssign(expected, actual)) {
      AddDiagnostic(argument.location, "call argument type does not match parameter type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    if (!assigned.at(index) &&
        !function.optional_parameters.contains(function.parameters.at(index).name)) {
      AddDiagnostic(location, "missing argument '" + function.parameters.at(index).name + "'");
    }
  }
  argument_order_.insert_or_assign(&expression, std::move(order));
  return function.return_type;
}

auto TypeChecker::LookupName(const Expression& expression) -> TypeChecker::Type {
  const auto* local = LookupLocal(expression.text);
  if (local != nullptr) {
    CaptureLocal(expression, *local);
    const auto refined = refinements_.find(local->id);
    return refined != refinements_.end() ? refined->second : local->type;
  }
  if (globals_.contains(expression.text)) {
    return ResolveGlobal(expression.text);
  }
  if (const auto function = functions_.find(expression.text); function != functions_.end()) {
    return FunctionType(function->second);
  }
  AddDiagnostic(expression.location, "unknown name '" + expression.text + "'");
  return InvalidType();
}

auto TypeChecker::LookupLocal(std::string_view name) const -> const LocalBinding* {
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
    refinements_.erase(binding.id);
  }
  scopes_.pop_back();
}

auto TypeChecker::DeclareLocal(std::string_view name,
                               const Type& type,
                               SourceLocation location,
                               BindingKind kind,
                               const void* declaration) -> bool {
  auto& scope = scopes_.back();
  if (name == "this") {
    AddDiagnostic(location, "'this' is reserved for the method receiver");
    return false;
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
    return false;
  }
  if (captured_declarations_.contains(declaration)) {
    captured_mutables_.insert(binding_id);
  }
  return true;
}

auto TypeChecker::IsAssignable(const Expression& expression) -> bool {
  return expression.kind == Expression::Kind::Name || expression.kind == Expression::Kind::Member ||
         expression.kind == Expression::Kind::Index;
}

void TypeChecker::CheckVariableDeclaration(const Statement& statement) {
  const bool inferred = statement.type.name.empty();
  Type type = inferred ? InvalidType() : ResolveType(statement.type, false);
  const Type initializer = CheckExpression(*statement.expression, inferred ? nullptr : &type);
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
  if (IsValid(type) && IsValid(initializer) && !CanAssign(type, initializer)) {
    AddDiagnostic(statement.location, "cannot initialize '" + TypeNameFor(type) + "' with '" +
                                          TypeNameFor(initializer) + "'");
  }
  const auto constant = statement.binding == BindingKind::Constant
                            ? EvaluateConstant(*statement.expression)
                            : std::nullopt;
  local_types_.insert_or_assign(&statement, type);
  const auto declared =
      DeclareLocal(statement.name, type, statement.location, statement.binding, &statement);
  if (declared) {
    bindings_.emplace(&statement, LookupLocal(statement.name)->id);
  }
  if (statement.binding == BindingKind::Constant) {
    const auto* local = LookupLocal(statement.name);
    if (declared && constant && local != nullptr) {
      constant_bindings_.insert_or_assign(local->id, *constant);
    }
  }
}

void TypeChecker::CheckReturn(const Statement& statement, const Type& return_type) {
  if (return_type.kind == Type::Kind::Void) {
    if (statement.expression) {
      const auto value = CheckExpression(*statement.expression, &return_type);
      if (IsValid(value) && value.kind != Type::Kind::Void) {
        AddDiagnostic(statement.location, "void function cannot return a value");
      }
    }
  } else if (!statement.expression) {
    AddDiagnostic(statement.location, "non-void function must return a value");
  } else {
    const Type value = CheckExpression(*statement.expression, &return_type);
    if (IsValid(value) && !CanAssign(return_type, value)) {
      AddDiagnostic(statement.location, "cannot return '" + TypeNameFor(value) +
                                            "' from function returning '" +
                                            TypeNameFor(return_type) + "'");
    }
  }
}

auto TypeChecker::CheckIntegerLiteral(const Expression& expression) -> TypeChecker::Type {
  std::int64_t value = 0;
  const auto result =
      std::from_chars(expression.text.data(), std::to_address(expression.text.end()), value);
  if (result.ec != std::errc()) {
    AddDiagnostic(expression.location, "integer literal is out of range");
    return InvalidType();
  }
  return {.kind = Type::Kind::Int, .name = "int"};
}

auto TypeChecker::CheckIndex(const Expression& expression) -> TypeChecker::Type {
  Type object = CheckExpression(*expression.object, nullptr);
  const Type index = CheckExpression(*expression.index, nullptr);
  if (object.nullable) {
    AddDiagnostic(expression.object->location, "nullable receiver requires a non-null check");
    return InvalidType();
  }
  if (object.kind != Type::Kind::Array && object.kind != Type::Kind::String) {
    AddDiagnostic(expression.location, "indexing requires an array or string");
  }
  if (index.kind != Type::Kind::Int || index.nullable) {
    AddDiagnostic(expression.index->location, "index must be int");
  }
  if (object.kind == Type::Kind::String) {
    builtin_calls_.insert_or_assign(&expression, Builtin::StringIndex);
    return object;
  }
  if (object.kind != Type::Kind::Array) {
    return InvalidType();
  }
  return array_elements_.at(object.name);
}

auto TypeChecker::CheckArrayLiteral(const Expression& expression, const Type* expected) -> Type {
  Type element_type;
  if (expected != nullptr && expected->kind == Type::Kind::Array) {
    const auto array = NonNullable(*expected);
    element_type = array_elements_.at(array.name);
  }
  for (const auto& element : expression.elements) {
    const Type actual = CheckExpression(*element, IsValid(element_type) ? &element_type : nullptr);
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

auto TypeChecker::CheckUnary(const Expression& expression) -> TypeChecker::Type {
  Type operand = CheckExpression(*expression.operand, nullptr);
  if (expression.text == "!" && (operand.kind != Type::Kind::Bool || operand.nullable)) {
    AddDiagnostic(expression.location, "'!' requires a bool operand");
    return InvalidType();
  }
  if (expression.text == "-" && !IsNumeric(operand)) {
    AddDiagnostic(expression.location, "unary '-' requires a numeric operand");
    return InvalidType();
  }
  return operand;
}

auto TypeChecker::CheckBinary(const Expression& expression) -> TypeChecker::Type {
  if (expression.text == "=") {
    return CheckAssignment(expression);
  }
  Type left = CheckExpression(*expression.left, nullptr);
  const auto after_left = refinements_;
  const bool logical = expression.text == "&&" || expression.text == "||";
  if (logical) {
    RefineCondition(*expression.left, expression.text == "&&" ? ConditionTruth::AlwaysTrue
                                                              : ConditionTruth::AlwaysFalse);
  }
  const Type right = CheckExpression(*expression.right, nullptr);
  if (logical) {
    refinements_ = IntersectFacts(after_left, refinements_);
  }
  if (!IsValid(left) || !IsValid(right)) {
    return InvalidType();
  }
  return CheckBinaryTypes(expression, left, right);
}

auto TypeChecker::CheckEquality(const Expression& expression, const Type& left, const Type& right)
    -> Type {
  if (left.kind == Type::Kind::Void || right.kind == Type::Kind::Void) {
    AddDiagnostic(expression.location, "equality requires value operands");
    return InvalidType();
  }
  const bool null_comparison = left.kind == Type::Kind::Null || right.kind == Type::Kind::Null;
  if (null_comparison) {
    return {.kind = Type::Kind::Bool, .name = "bool"};
  }
  if (left.kind == Type::Kind::Array || left.kind == Type::Kind::StringBuilder ||
      left.kind == Type::Kind::Function || left.kind == Type::Kind::Expected ||
      left.kind == Type::Kind::Set || left.kind == Type::Kind::Map) {
    AddDiagnostic(expression.location, "equality is not defined for this type");
    return InvalidType();
  }
  if (!IsSameType(NonNullable(left), NonNullable(right))) {
    AddDiagnostic(expression.location, "comparison operands must have the same type");
    return InvalidType();
  }
  return {.kind = Type::Kind::Bool, .name = "bool"};
}

auto TypeChecker::CheckBinaryTypes(const Expression& expression,
                                   const Type& left,
                                   const Type& right) -> Type {
  if (expression.text == "==" || expression.text == "!=") {
    return CheckEquality(expression, left, right);
  }
  if (expression.text == "&&" || expression.text == "||") {
    if (left.kind != Type::Kind::Bool || right.kind != Type::Kind::Bool || left.nullable ||
        right.nullable) {
      AddDiagnostic(expression.location, "logical operands must be bool");
      return InvalidType();
    }
    return {.kind = Type::Kind::Bool, .name = "bool"};
  }
  if (expression.text == "<" || expression.text == "<=" || expression.text == ">" ||
      expression.text == ">=") {
    if (!IsNumeric(left) || !IsSameType(left, right)) {
      AddDiagnostic(expression.location, "relational operands must have the same numeric type");
      return InvalidType();
    }
    return {.kind = Type::Kind::Bool, .name = "bool"};
  }
  if (expression.text == "+" &&
      (left.kind == Type::Kind::String || right.kind == Type::Kind::String) &&
      CanFormatString(left) && CanFormatString(right)) {
    return {.kind = Type::Kind::String, .name = "string"};
  }
  if (!IsNumeric(left) || !IsSameType(left, right)) {
    AddDiagnostic(expression.location, "arithmetic operands must have the same numeric type");
    return InvalidType();
  }
  return left;
}

auto TypeChecker::CheckAssignment(const Expression& expression) -> Type {
  if (!IsAssignable(*expression.left)) {
    AddDiagnostic(expression.left->location, "left side of assignment is not assignable");
    return InvalidType();
  }
  if (expression.left->kind == Expression::Kind::Name) {
    const auto* local = LookupLocal(expression.left->text);
    if (local != nullptr && local->kind == BindingKind::Constant) {
      AddDiagnostic(expression.left->location,
                    "cannot assign to const binding '" + expression.left->text + "'");
    }
    if (local == nullptr) {
      const auto global = globals_.find(expression.left->text);
      if (global == globals_.end() ||
          global->second.declaration->binding == BindingKind::Constant) {
        AddDiagnostic(expression.left->location, "cannot assign to an immutable global binding");
      }
    }
  }
  Type left = CheckExpression(*expression.left, nullptr);
  if (expression.left->kind == Expression::Kind::Member) {
    CheckMemberAssignment(*expression.left);
  }
  const LocalBinding* binding = expression.left->kind == Expression::Kind::Name
                                    ? LookupLocal(expression.left->text)
                                    : nullptr;
  if (binding != nullptr) {
    left = binding->type;
  }
  if (builtin_calls_.contains(expression.left.get())) {
    AddDiagnostic(expression.left->location,
                  "builtin properties and string elements are immutable");
  }
  const Type right = CheckExpression(*expression.right, &left);
  if (binding != nullptr) {
    refinements_.erase(binding->id);
    if (IsValid(right) && right.kind != Type::Kind::Null && !right.nullable) {
      refinements_.insert_or_assign(binding->id, NonNullable(left));
    }
  }
  if (IsValid(left) && IsValid(right) && !CanAssign(left, right)) {
    AddDiagnostic(expression.location, "assignment types do not match");
    return InvalidType();
  }
  return left;
}

void TypeChecker::CheckMemberAssignment(const Expression& member) {
  bool read_only = interface_members_.contains(&member);
  if (const auto found = static_members_.find(&member); found != static_members_.end()) {
    const auto& owner = classes_.at(found->second.owner);
    read_only = read_only || owner.methods.contains(found->second.member) ||
                (!sources_.empty() && sources_.subspan(owner.declaration->location.source_id)
                                          .front()
                                          .path.starts_with(kStandardSourcePrefix));
  } else if (const auto object = expression_types_.find(member.object.get());
             object != expression_types_.end() && object->second.kind == Type::Kind::Class) {
    read_only =
        read_only || classes_.at(NonNullable(object->second).name).methods.contains(member.text);
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
