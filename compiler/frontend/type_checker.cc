#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"

namespace vela::frontend {

auto TypeChecker::InvalidType() -> TypeChecker::Type {
  return {};
}

auto TypeChecker::Check(const Program& program) -> TypeCheckResult {
  diagnostics_.clear();
  function_count_ = program.functions.size();
  types_.clear();
  expression_types_.clear();
  case_values_.clear();
  enum_values_.clear();
  builtin_calls_.clear();
  local_types_.clear();
  control_flow_.clear();
  classes_.clear();
  functions_.clear();
  scopes_.clear();
  loop_depth_ = 0;
  switch_depth_ = 0;
  source_paths_ = program.source_paths;

  CollectTypes(program);
  ResolveDeclarations(program);
  // 描述表在检查函数体前完成构建；检查期间仅修改作用域表，借用的描述信息保持有效。
  for (const Function& function : program.functions) {
    CheckFunction(function, nullptr);
  }
  for (const Class& class_declaration : program.classes) {
    const auto class_info = classes_.find(class_declaration.name);
    if (class_info == classes_.end()) {
      continue;
    }
    for (const Function& method : class_declaration.methods) {
      CheckFunction(method, &class_info->second);
    }
  }

  SemanticModel model;
  model.types = std::move(types_);
  model.expression_types = std::move(expression_types_);
  model.case_values = std::move(case_values_);
  model.enum_values = std::move(enum_values_);
  model.builtin_calls = std::move(builtin_calls_);
  model.local_types = std::move(local_types_);
  model.control_flow = std::move(control_flow_);
  model.classes = std::move(classes_);
  model.functions = std::move(functions_);
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
      .source_path = location.source_id < source_paths_.size()
                         ? source_paths_.subspan(location.source_id).front()
                         : std::string(),
  });
}

void TypeChecker::CollectTypes(const Program& program) {
  types_.emplace("void", Type{.kind = Type::Kind::Void, .name = "void"});
  types_.emplace("bool", Type{.kind = Type::Kind::Bool, .name = "bool"});
  types_.emplace("int", Type{.kind = Type::Kind::Int, .name = "int"});
  types_.emplace("double", Type{.kind = Type::Kind::Double, .name = "double"});
  types_.emplace("string", Type{.kind = Type::Kind::String, .name = "string"});
  types_.emplace("StringBuilder", Type{.kind = Type::Kind::StringBuilder, .name = "StringBuilder"});
  types_.emplace("string[]", Type{.kind = Type::Kind::Array, .name = "string[]"});
  types_.reserve(types_.size() + program.enums.size() + program.classes.size());
  classes_.reserve(program.classes.size());

  for (const Enum& enumeration : program.enums) {
    if (FindBuiltin(Type::Kind::Invalid, enumeration.name)) {
      AddDiagnostic(enumeration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Enum, .name = enumeration.name};
    if (!types_.emplace(enumeration.name, type).second) {
      AddDiagnostic(enumeration.location, "duplicate type '" + enumeration.name + "'");
      continue;
    }
    std::int64_t ordinal = 0;
    for (const auto& member : enumeration.members) {
      if (!enum_values_.try_emplace(enumeration.name + "." + member.name, ordinal++).second) {
        AddDiagnostic(member.location, "duplicate enum member '" + member.name + "'");
      }
    }
  }

  for (const Class& class_declaration : program.classes) {
    if (FindBuiltin(Type::Kind::Invalid, class_declaration.name)) {
      AddDiagnostic(class_declaration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Class, .name = class_declaration.name};
    if (!types_.emplace(class_declaration.name, type).second) {
      AddDiagnostic(class_declaration.location, "duplicate type '" + class_declaration.name + "'");
      continue;
    }
    ClassInfo info;
    info.type = type;
    classes_.emplace(class_declaration.name, std::move(info));
  }
}

void TypeChecker::ResolveDeclarations(const Program& program) {
  functions_.reserve(program.functions.size());
  for (const Class& class_declaration : program.classes) {
    const auto class_info = classes_.find(class_declaration.name);
    if (class_info == classes_.end()) {
      continue;
    }
    ClassInfo& info = class_info->second;
    info.fields.reserve(class_declaration.fields.size());
    info.field_order.reserve(class_declaration.fields.size());
    info.methods.reserve(class_declaration.methods.size());
    for (const Field& field : class_declaration.fields) {
      const Type type = ResolveType(field.type, false);
      if (!info.fields.emplace(field.name, type).second) {
        AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
      } else {
        info.field_order.push_back(field.name);
      }
    }
    for (const Function& method : class_declaration.methods) {
      FunctionInfo method_info = ResolveSignature(method);
      if (info.fields.contains(method.name)) {
        AddDiagnostic(method.location, "member name is already used by a field");
      }
      if (!info.methods.emplace(method.name, std::move(method_info)).second) {
        AddDiagnostic(method.location, "duplicate method '" + method.name + "'");
      }
    }
  }

  for (const Function& function : program.functions) {
    FunctionInfo function_info = ResolveSignature(function);
    if (FindBuiltin(Type::Kind::Invalid, function.name) || types_.contains(function.name)) {
      AddDiagnostic(function.location, "function name is reserved for a type or builtin");
      continue;
    }
    if (!functions_.emplace(function.name, std::move(function_info)).second) {
      AddDiagnostic(function.location, "duplicate function '" + function.name + "'");
    }
  }
}

auto TypeChecker::ResolveSignature(const Function& function) -> FunctionInfo {
  FunctionInfo info;
  info.return_type = ResolveType(function.return_type, true);
  info.parameters.reserve(function.parameters.size());
  info.parameter_order.reserve(function.parameters.size());
  for (const auto& parameter : function.parameters) {
    const Type type = ResolveType(parameter.type, false);
    if (!info.parameters.emplace(parameter.name, type).second) {
      AddDiagnostic(parameter.location, "duplicate parameter '" + parameter.name + "'");
    } else {
      info.parameter_order.push_back(parameter.name);
    }
  }
  return info;
}

void TypeChecker::CheckFunction(const Function& function, const ClassInfo* owner) {
  non_null_.clear();
  constant_bindings_.clear();
  next_binding_id_ = 0;
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
  if (owner != nullptr) {
    scopes_.back().try_emplace(
        "this",
        LocalBinding{.type = owner->type, .kind = BindingKind::Constant, .id = next_binding_id_++});
  }
  for (const Parameter& parameter : function.parameters) {
    const auto parameter_type = function_info->parameters.find(parameter.name);
    const Type type =
        parameter_type == function_info->parameters.end() ? InvalidType() : parameter_type->second;
    DeclareLocal(parameter.name, type, parameter.location);
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
}

auto TypeChecker::CheckExpression(const Expression& expression, const Type* expected)
    -> TypeChecker::Type {
  auto type = CheckExpressionType(expression, expected);
  // 名称读取可能被非空分析收窄，数组字面量依赖期望类型；固定运算无需重复记录类型。
  if (expression.kind == Expression::Kind::Name ||
      expression.kind == Expression::Kind::ArrayLiteral) {
    expression_types_.insert_or_assign(&expression, type);
  }
  return type;
}

auto TypeChecker::CheckExpressionType(const Expression& expression, const Type* expected) -> Type {
  switch (expression.kind) {
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
        if ((type.kind != Type::Kind::String || type.nullable) && IsValid(type)) {
          AddDiagnostic(part->location, "interpolation requires string; use explicit toString()");
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
  if (callee.kind == Expression::Kind::Member) {
    return CheckMethodCall(expression, expected);
  }
  if (callee.kind != Expression::Kind::Name) {
    AddDiagnostic(expression.location, "expression is not callable");
    return InvalidType();
  }
  if (LookupLocal(callee.text) != nullptr) {
    AddDiagnostic(callee.location, "local binding is not callable");
    return InvalidType();
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
    return {.kind = Type::Kind::Void, .name = "void"};
  }
  const auto class_info = classes_.find(callee.text);
  if (class_info != classes_.end()) {
    const Type result =
        CheckClassConstruction(class_info->second, expression.arguments, expression.location);
    if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
      AddDiagnostic(expression.location, "constructed type does not match expected type");
    }
    return result;
  }
  const auto function = functions_.find(callee.text);
  if (function != functions_.end()) {
    const Type result =
        CheckFunctionCall(function->second, expression.arguments, expression.location);
    if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
      AddDiagnostic(expression.location, "function result does not match expected type");
    }
    return result;
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckMethodCall(const Expression& expression, const Type* expected) -> Type {
  const Type object = CheckExpression(*expression.callee->object, nullptr);
  if (object.nullable) {
    AddDiagnostic(expression.callee->object->location,
                  "nullable receiver requires a non-null check");
    return InvalidType();
  }
  if (const auto builtin = FindBuiltin(object.kind, expression.callee->text)) {
    return CheckBuiltinCall(expression, *builtin);
  }
  if (object.kind == Type::Kind::Class) {
    const auto class_info = classes_.find(object.name);
    if (class_info != classes_.end()) {
      const auto method = class_info->second.methods.find(expression.callee->text);
      if (method != class_info->second.methods.end()) {
        const Type result =
            CheckFunctionCall(method->second, expression.arguments, expression.location);
        if (expected != nullptr && IsValid(result) && !CanAssign(*expected, result)) {
          AddDiagnostic(expression.location, "method result does not match expected type");
        }
        return result;
      }
    }
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckBuiltinCall(const Expression& expression, Builtin builtin) -> Type {
  builtin_calls_.insert_or_assign(&expression, builtin);
  return CheckFunctionCall(BuiltinSignature(builtin), expression.arguments, expression.location);
}

auto TypeChecker::CheckFunctionCall(const FunctionInfo& function,
                                    const std::vector<CallArgument>& arguments,
                                    SourceLocation location) -> TypeChecker::Type {
  std::vector<bool> assigned(function.parameter_order.size(), false);
  std::size_t positional = 0;
  bool named = false;
  for (const CallArgument& argument : arguments) {
    std::size_t index = positional;
    if (!argument.name.empty()) {
      named = true;
      const auto parameter = std::ranges::find(function.parameter_order, argument.name);
      if (parameter == function.parameter_order.end()) {
        AddDiagnostic(argument.location, "unknown named argument '" + argument.name + "'");
        continue;
      }
      index = static_cast<std::size_t>(parameter - function.parameter_order.begin());
    } else {
      if (named) {
        AddDiagnostic(argument.location, "positional argument cannot follow a named argument");
      }
      ++positional;
    }
    if (index >= function.parameter_order.size()) {
      AddDiagnostic(argument.location, "too many call arguments");
      continue;
    }
    if (assigned.at(index)) {
      AddDiagnostic(argument.location, "call argument is specified more than once");
      continue;
    }
    assigned.at(index) = true;
    const auto expected_type = function.parameters.find(function.parameter_order.at(index));
    const Type expected =
        expected_type == function.parameters.end() ? InvalidType() : expected_type->second;
    const Type actual = CheckExpression(*argument.value, &expected);
    if (IsValid(actual) && !CanAssign(expected, actual)) {
      AddDiagnostic(argument.location, "call argument type does not match parameter type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    if (!assigned.at(index)) {
      AddDiagnostic(location, "missing argument '" + function.parameter_order.at(index) + "'");
    }
  }
  return function.return_type;
}

auto TypeChecker::CheckClassConstruction(const ClassInfo& class_info,
                                         const std::vector<CallArgument>& arguments,
                                         SourceLocation location) -> TypeChecker::Type {
  std::vector<bool> assigned(class_info.field_order.size(), false);
  std::size_t positional = 0;
  bool named = false;
  for (const CallArgument& argument : arguments) {
    std::size_t index = positional;
    if (!argument.name.empty()) {
      named = true;
      const auto field = std::ranges::find(class_info.field_order, argument.name);
      if (field == class_info.field_order.end()) {
        AddDiagnostic(argument.location, "unknown constructor field '" + argument.name + "'");
        continue;
      }
      index = static_cast<std::size_t>(field - class_info.field_order.begin());
    } else {
      if (named) {
        AddDiagnostic(argument.location, "positional argument cannot follow a named argument");
      }
      ++positional;
    }
    if (index >= class_info.field_order.size()) {
      AddDiagnostic(argument.location, "too many constructor arguments");
      continue;
    }
    if (assigned.at(index)) {
      AddDiagnostic(argument.location, "constructor field is specified more than once");
      continue;
    }
    assigned.at(index) = true;
    const auto expected_field = class_info.fields.find(class_info.field_order.at(index));
    const Type expected =
        expected_field == class_info.fields.end() ? InvalidType() : expected_field->second;
    const Type actual = CheckExpression(*argument.value, &expected);
    if (IsValid(actual) && !CanAssign(expected, actual)) {
      AddDiagnostic(argument.location, "constructor argument type does not match field type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    if (!assigned.at(index)) {
      AddDiagnostic(location,
                    "missing constructor field '" + class_info.field_order.at(index) + "'");
    }
  }
  return class_info.type;
}

auto TypeChecker::CheckMemberAccess(const Expression& expression) -> TypeChecker::Type {
  if (expression.object->kind == Expression::Kind::Name &&
      LookupLocal(expression.object->text) == nullptr) {
    const auto enumeration = types_.find(expression.object->text);
    if (enumeration != types_.end() && enumeration->second.kind == Type::Kind::Enum) {
      if (enum_values_.contains(expression.object->text + "." + expression.text)) {
        return enumeration->second;
      }
      AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
      return InvalidType();
    }
  }

  const Type object = CheckExpression(*expression.object, nullptr);
  if (object.nullable) {
    AddDiagnostic(expression.object->location, "nullable receiver requires a non-null check");
    return InvalidType();
  }
  if (object.kind == Type::Kind::String &&
      (expression.text == "length" || expression.text == "byteLength")) {
    builtin_calls_.insert_or_assign(&expression, expression.text == "length"
                                                     ? Builtin::StringLength
                                                     : Builtin::StringByteLength);
    return {.kind = Type::Kind::Int, .name = "int"};
  }
  if (object.kind != Type::Kind::Class) {
    AddDiagnostic(expression.location, "member access requires a class object");
    return InvalidType();
  }
  const auto class_info = classes_.find(object.name);
  if (class_info == classes_.end()) {
    return InvalidType();
  }
  const auto field = class_info->second.fields.find(expression.text);
  if (field == class_info->second.fields.end()) {
    AddDiagnostic(expression.location, "unknown field '" + expression.text + "'");
    return InvalidType();
  }
  return field->second;
}

auto TypeChecker::ResolveType(const TypeName& type_name, bool allow_void) -> TypeChecker::Type {
  const auto suffix = type_name.name.find_first_of("[?");
  const std::string_view base(type_name.name.data(),
                              suffix == std::string::npos ? type_name.name.size() : suffix);
  const auto type = types_.find(base);
  if (type == types_.end()) {
    AddDiagnostic(type_name.location, "unknown type '" + type_name.name + "'");
    return InvalidType();
  }
  if ((!allow_void || base.size() != type_name.name.size()) &&
      type->second.kind == Type::Kind::Void) {
    AddDiagnostic(type_name.location, "void is not a value type");
    return InvalidType();
  }
  Type result = type->second;
  for (std::size_t length = base.size(); length < type_name.name.size();) {
    if (type_name.name.at(length) == '?') {
      if (result.nullable) {
        AddDiagnostic(type_name.location, "type is already nullable");
        return InvalidType();
      }
      result.nullable = true;
      result.name += '?';
      ++length;
    } else {
      result = {.kind = Type::Kind::Array, .name = result.name + "[]"};
      length += 2;
    }
    types_.try_emplace(result.name, result);
  }
  return result;
}

auto TypeChecker::LookupName(const Expression& expression) -> TypeChecker::Type {
  const auto* local = LookupLocal(expression.text);
  if (local != nullptr) {
    return non_null_.contains(local->id) ? NonNullable(local->type) : local->type;
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
    non_null_.erase(binding.id);
  }
  scopes_.pop_back();
}

auto TypeChecker::DeclareLocal(std::string_view name,
                               const Type& type,
                               SourceLocation location,
                               BindingKind kind) -> bool {
  auto& scope = scopes_.back();
  if (name == "this") {
    AddDiagnostic(location, "'this' is reserved for the method receiver");
    return false;
  }
  if (!scope.try_emplace(name, LocalBinding{.type = type, .kind = kind, .id = next_binding_id_++})
           .second) {
    AddDiagnostic(location, "duplicate local name '" + std::string(name) + "'");
    return false;
  }
  return true;
}

auto TypeChecker::IsAssignable(const Expression& expression) -> bool {
  return expression.kind == Expression::Kind::Name || expression.kind == Expression::Kind::Member ||
         expression.kind == Expression::Kind::Index;
}

auto TypeChecker::IsSameType(const Type& left, const Type& right) -> bool {
  return left.kind == right.kind && left.name == right.name && left.nullable == right.nullable;
}

auto TypeChecker::NonNullable(Type type) -> Type {
  if (type.nullable) {
    type.nullable = false;
    type.name.pop_back();
  }
  return type;
}

auto TypeChecker::CanAssign(const Type& target, const Type& value) -> bool {
  return IsSameType(target, value) || (target.nullable && (value.kind == Type::Kind::Null ||
                                                           IsSameType(NonNullable(target), value)));
}

auto TypeChecker::IsNumeric(const Type& type) -> bool {
  return !type.nullable && (type.kind == Type::Kind::Int || type.kind == Type::Kind::Double);
}

auto TypeChecker::IsValid(const Type& type) -> bool {
  return type.kind != Type::Kind::Invalid;
}

auto TypeChecker::TypeNameFor(const Type& type) -> std::string {
  return type.name.empty() ? "<invalid>" : type.name;
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
  const auto declared = DeclareLocal(statement.name, type, statement.location, statement.binding);
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
      AddDiagnostic(statement.location, "void function cannot return a value");
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
  return types_.at(object.name.substr(0, object.name.size() - 2));
}

auto TypeChecker::CheckArrayLiteral(const Expression& expression, const Type* expected) -> Type {
  Type element_type;
  if (expected != nullptr && expected->kind == Type::Kind::Array) {
    const auto array = NonNullable(*expected);
    element_type = types_.at(array.name.substr(0, array.name.size() - 2));
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
  Type result{.kind = Type::Kind::Array, .name = element_type.name + "[]"};
  types_.try_emplace(result.name, result);
  return result;
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
  const auto after_left = non_null_;
  const bool logical = expression.text == "&&" || expression.text == "||";
  if (logical) {
    RefineCondition(*expression.left, expression.text == "&&" ? ConditionTruth::AlwaysTrue
                                                              : ConditionTruth::AlwaysFalse);
  }
  const Type right = CheckExpression(*expression.right, nullptr);
  if (logical) {
    non_null_ = IntersectFacts(after_left, non_null_);
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
  if (left.kind == Type::Kind::Array || left.kind == Type::Kind::StringBuilder) {
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
  if (expression.text == "+" && left.kind == Type::Kind::String &&
      right.kind == Type::Kind::String && !left.nullable && !right.nullable) {
    return left;
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
  }
  Type left = CheckExpression(*expression.left, nullptr);
  const LocalBinding* binding = expression.left->kind == Expression::Kind::Name
                                    ? LookupLocal(expression.left->text)
                                    : nullptr;
  if (binding != nullptr) {
    left = binding->type;
  }
  if (builtin_calls_.contains(expression.left.get())) {
    AddDiagnostic(expression.left->location, "string values are immutable");
  }
  const Type right = CheckExpression(*expression.right, &left);
  if (binding != nullptr) {
    non_null_.erase(binding->id);
    if (IsValid(right) && right.kind != Type::Kind::Null && !right.nullable) {
      non_null_.insert(binding->id);
    }
  }
  if (IsValid(left) && IsValid(right) && !CanAssign(left, right)) {
    AddDiagnostic(expression.location, "assignment types do not match");
    return InvalidType();
  }
  return left;
}

}  // namespace vela::frontend
