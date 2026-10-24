#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <charconv>
#include <ranges>
#include <string_view>

namespace vela::frontend {

auto TypeChecker::InvalidType() -> TypeChecker::Type {
  return {};
}

auto TypeChecker::Check(const Program& program) -> TypeCheckResult {
  diagnostics_.clear();
  function_count_ = program.functions.size();
  types_.clear();
  enum_members_.clear();
  classes_.clear();
  functions_.clear();
  scopes_.clear();
  loop_depth_ = 0;

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
  model.enum_members = std::move(enum_members_);
  model.classes = std::move(classes_);
  model.functions = std::move(functions_);
  return {
      .diagnostics = std::move(diagnostics_),
      .function_count = function_count_,
      .model = std::move(model),
  };
}

void TypeChecker::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message)});
}

void TypeChecker::CollectTypes(const Program& program) {
  types_.emplace("void", Type{.kind = Type::Kind::Void, .name = "void"});
  types_.emplace("bool", Type{.kind = Type::Kind::Bool, .name = "bool"});
  types_.emplace("int", Type{.kind = Type::Kind::Int, .name = "int"});
  types_.emplace("float", Type{.kind = Type::Kind::Float, .name = "float"});
  types_.emplace("string", Type{.kind = Type::Kind::String, .name = "string"});
  types_.emplace("array", Type{.kind = Type::Kind::Array, .name = "array"});
  types_.reserve(types_.size() + program.enums.size() + program.classes.size());
  enum_members_.reserve(program.enums.size());
  classes_.reserve(program.classes.size());

  for (const Enum& enumeration : program.enums) {
    const Type type{.kind = Type::Kind::Enum, .name = enumeration.name};
    if (!types_.emplace(enumeration.name, type).second) {
      AddDiagnostic(enumeration.location, "duplicate type '" + enumeration.name + "'");
      continue;
    }
    auto& stored_members = enum_members_.try_emplace(enumeration.name).first->second;
    stored_members.reserve(enumeration.members.size());
    for (const std::string& member : enumeration.members) {
      if (!stored_members.insert(member).second) {
        AddDiagnostic(enumeration.location, "duplicate enum member '" + member + "'");
      }
    }
  }

  for (const Class& class_declaration : program.classes) {
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
      if (!info.methods.emplace(method.name, std::move(method_info)).second) {
        AddDiagnostic(method.location, "duplicate method '" + method.name + "'");
      }
    }
  }

  for (const Function& function : program.functions) {
    FunctionInfo function_info = ResolveSignature(function);
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
    DeclareLocal("this", owner->type, function.location);
  }
  for (const Parameter& parameter : function.parameters) {
    const auto parameter_type = function_info->parameters.find(parameter.name);
    const Type type =
        parameter_type == function_info->parameters.end() ? InvalidType() : parameter_type->second;
    DeclareLocal(parameter.name, type, parameter.location);
  }
  if (function.body) {
    CheckStatement(*function.body, return_type);
  }
  EndScope();
}

void TypeChecker::CheckStatement(const Statement& statement, const Type& return_type) {
  switch (statement.kind) {
    case Statement::Kind::VariableDeclaration:
      CheckVariableDeclaration(statement);
      break;
    case Statement::Kind::Return:
      CheckReturn(statement, return_type);
      break;
    case Statement::Kind::Expression:
      if (statement.expression) {
        CheckExpression(*statement.expression, nullptr);
      }
      break;
    case Statement::Kind::Block:
      BeginScope();
      for (const std::unique_ptr<Statement>& child : statement.statements) {
        CheckStatement(*child, return_type);
      }
      EndScope();
      break;
    case Statement::Kind::If:
      CheckIf(statement, return_type);
      break;
    case Statement::Kind::While:
      CheckWhile(statement, return_type);
      break;
    case Statement::Kind::For:
      CheckFor(statement, return_type);
      break;
    case Statement::Kind::Break:
    case Statement::Kind::Continue:
      if (loop_depth_ == 0) {
        AddDiagnostic(statement.location, "loop control statement must be inside a loop");
      }
      break;
  }
}

auto TypeChecker::CheckExpression(const Expression& expression, const Type* expected)
    -> TypeChecker::Type {
  switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
      return CheckIntegerLiteral(expression);
    case Expression::Kind::FloatLiteral:
      return {.kind = Type::Kind::Float, .name = "float"};
    case Expression::Kind::StringLiteral:
      return {.kind = Type::Kind::String, .name = "string"};
    case Expression::Kind::ArrayLiteral: {
      for (const auto& element : expression.elements) {
        CheckExpression(*element, nullptr);
      }
      return {.kind = Type::Kind::Array, .name = "array"};
    }
    case Expression::Kind::Index:
      return CheckIndex(expression);
    case Expression::Kind::BooleanLiteral:
      return {.kind = Type::Kind::Bool, .name = "bool"};
    case Expression::Kind::Name:
      return LookupName(expression);
    case Expression::Kind::ShorthandMember:
      return CheckShorthandMember(expression, expected);
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
  if (expression.callee->kind == Expression::Kind::Name) {
    if (expression.callee->text == "print") {
      if (expression.arguments.size() != 1) {
        AddDiagnostic(expression.location, "print expects one argument");
      } else {
        CheckExpression(*expression.arguments.front().value, nullptr);
      }
      return {.kind = Type::Kind::Void, .name = "void"};
    }
    const auto type = types_.find(expression.callee->text);
    if (type != types_.end() && type->second.kind == Type::Kind::Class) {
      const auto class_info = classes_.find(expression.callee->text);
      if (class_info != classes_.end()) {
        const Type result =
            CheckClassConstruction(class_info->second, expression.arguments, expression.location);
        if (expected != nullptr && IsValid(result) && !IsSameType(result, *expected)) {
          AddDiagnostic(expression.location, "constructed type does not match expected type");
        }
        return result;
      }
    }
    const auto function = functions_.find(expression.callee->text);
    if (function != functions_.end()) {
      const Type result =
          CheckFunctionCall(function->second, expression.arguments, expression.location);
      if (expected != nullptr && IsValid(result) && !IsSameType(result, *expected)) {
        AddDiagnostic(expression.location, "function result does not match expected type");
      }
      return result;
    }
  }

  if (expression.callee->kind == Expression::Kind::Member) {
    return CheckMethodCall(expression, expected);
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

auto TypeChecker::CheckMethodCall(const Expression& expression, const Type* expected) -> Type {
  const Type object = CheckExpression(*expression.callee->object, nullptr);
  if (object.kind == Type::Kind::Class) {
    const auto class_info = classes_.find(object.name);
    if (class_info != classes_.end()) {
      const auto method = class_info->second.methods.find(expression.callee->text);
      if (method != class_info->second.methods.end()) {
        const Type result =
            CheckFunctionCall(method->second, expression.arguments, expression.location);
        if (expected != nullptr && IsValid(result) && !IsSameType(result, *expected)) {
          AddDiagnostic(expression.location, "method result does not match expected type");
        }
        return result;
      }
    }
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
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
    if (IsValid(actual) && !IsSameType(expected, actual)) {
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
    if (IsValid(actual) && !IsSameType(expected, actual)) {
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
  if (expression.object->kind == Expression::Kind::Name) {
    const auto enumeration = types_.find(expression.object->text);
    if (enumeration != types_.end() && enumeration->second.kind == Type::Kind::Enum) {
      const auto members = enum_members_.find(expression.object->text);
      if (members != enum_members_.end() && members->second.contains(expression.text)) {
        return enumeration->second;
      }
      AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
      return InvalidType();
    }
  }

  const Type object = CheckExpression(*expression.object, nullptr);
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
  const auto type = types_.find(type_name.name);
  if (type == types_.end()) {
    AddDiagnostic(type_name.location, "unknown type '" + type_name.name + "'");
    return InvalidType();
  }
  if (!allow_void && type->second.kind == Type::Kind::Void) {
    AddDiagnostic(type_name.location, "void is not a value type");
    return InvalidType();
  }
  return type->second;
}

auto TypeChecker::LookupName(const Expression& expression) -> TypeChecker::Type {
  Type local = LookupLocal(expression.text);
  if (IsValid(local)) {
    return local;
  }
  AddDiagnostic(expression.location, "unknown name '" + expression.text + "'");
  return InvalidType();
}

auto TypeChecker::LookupLocal(std::string_view name) const -> TypeChecker::Type {
  for (const auto& scope : std::views::reverse(scopes_)) {
    const auto value = scope.find(name);
    if (value != scope.end()) {
      return value->second;
    }
  }
  return InvalidType();
}

void TypeChecker::BeginScope() {
  scopes_.emplace_back();
}

void TypeChecker::EndScope() {
  scopes_.pop_back();
}

auto TypeChecker::DeclareLocal(std::string_view name, const Type& type, SourceLocation location)
    -> bool {
  auto& scope = scopes_.back();
  if (!scope.try_emplace(name, type).second) {
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
  return left.kind == right.kind && left.name == right.name;
}

auto TypeChecker::IsNumeric(const Type& type) -> bool {
  return type.kind == Type::Kind::Int || type.kind == Type::Kind::Float;
}

auto TypeChecker::IsValid(const Type& type) -> bool {
  return type.kind != Type::Kind::Invalid;
}

auto TypeChecker::TypeNameFor(const Type& type) -> std::string {
  return type.name.empty() ? "<invalid>" : type.name;
}

void TypeChecker::CheckVariableDeclaration(const Statement& statement) {
  const Type type = ResolveType(statement.type, false);
  if (!statement.expression) {
    AddDiagnostic(statement.location, "variable declarations require an initializer");
  } else {
    const Type initializer = CheckExpression(*statement.expression, &type);
    if (IsValid(initializer) && !IsSameType(type, initializer)) {
      AddDiagnostic(statement.location, "cannot initialize '" + TypeNameFor(type) + "' with '" +
                                            TypeNameFor(initializer) + "'");
    }
  }
  DeclareLocal(statement.name, type, statement.location);
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
    if (IsValid(value) && !IsSameType(return_type, value)) {
      AddDiagnostic(statement.location, "cannot return '" + TypeNameFor(value) +
                                            "' from function returning '" +
                                            TypeNameFor(return_type) + "'");
    }
  }
}

void TypeChecker::CheckIf(const Statement& statement, const Type& return_type) {
  const Type condition = CheckExpression(*statement.condition, nullptr);
  if (condition.kind != Type::Kind::Bool) {
    AddDiagnostic(statement.condition->location, "if condition must be bool");
  }
  CheckStatement(*statement.then_branch, return_type);
  if (statement.else_branch != nullptr) {
    CheckStatement(*statement.else_branch, return_type);
  }
}

void TypeChecker::CheckWhile(const Statement& statement, const Type& return_type) {
  const Type condition = CheckExpression(*statement.condition, nullptr);
  if (condition.kind != Type::Kind::Bool) {
    AddDiagnostic(statement.condition->location, "while condition must be bool");
  }
  ++loop_depth_;
  CheckStatement(*statement.then_branch, return_type);
  --loop_depth_;
}

void TypeChecker::CheckFor(const Statement& statement, const Type& return_type) {
  BeginScope();
  if (statement.initializer) {
    CheckStatement(*statement.initializer, return_type);
  }
  if (statement.condition) {
    const Type condition = CheckExpression(*statement.condition, nullptr);
    if (condition.kind != Type::Kind::Bool) {
      AddDiagnostic(statement.condition->location, "for condition must be bool");
    }
  }
  if (statement.increment) {
    CheckExpression(*statement.increment, nullptr);
  }
  ++loop_depth_;
  CheckStatement(*statement.then_branch, return_type);
  --loop_depth_;
  EndScope();
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
  const Type object = CheckExpression(*expression.object, nullptr);
  const Type index = CheckExpression(*expression.index, nullptr);
  if (object.kind != Type::Kind::Array) {
    AddDiagnostic(expression.location, "indexing requires an array");
  }
  if (index.kind != Type::Kind::Int) {
    AddDiagnostic(expression.index->location, "array index must be int");
  }
  return {.kind = Type::Kind::Int, .name = "int"};
}

auto TypeChecker::CheckShorthandMember(const Expression& expression, const Type* expected)
    -> TypeChecker::Type {
  if (expected == nullptr || expected->kind != Type::Kind::Enum) {
    AddDiagnostic(expression.location, "enum shorthand requires an expected enum type");
    return InvalidType();
  }
  const auto members = enum_members_.find(expected->name);
  if (members == enum_members_.end() || !members->second.contains(expression.text)) {
    AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
    return InvalidType();
  }
  return *expected;
}

auto TypeChecker::CheckUnary(const Expression& expression) -> TypeChecker::Type {
  Type operand = CheckExpression(*expression.operand, nullptr);
  if (expression.text == "!" && operand.kind != Type::Kind::Bool) {
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
  const Type right = CheckExpression(*expression.right, nullptr);
  if (!IsValid(left) || !IsValid(right)) {
    return InvalidType();
  }
  if (expression.text == "==" || expression.text == "!=") {
    if (!IsSameType(left, right)) {
      AddDiagnostic(expression.location, "comparison operands must have the same type");
      return InvalidType();
    }
    return {.kind = Type::Kind::Bool, .name = "bool"};
  }
  if (expression.text == "&&" || expression.text == "||") {
    if (left.kind != Type::Kind::Bool || right.kind != Type::Kind::Bool) {
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
      right.kind == Type::Kind::String) {
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
  Type left = CheckExpression(*expression.left, nullptr);
  const Type right = CheckExpression(*expression.right, &left);
  if (IsValid(left) && IsValid(right) && !IsSameType(left, right)) {
    AddDiagnostic(expression.location, "assignment types do not match");
    return InvalidType();
  }
  return left;
}

}  // namespace vela::frontend
