#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace vela::frontend {

TypeChecker::Type TypeChecker::InvalidType() {
  return {};
}

TypeCheckResult TypeChecker::Check(const Program& program) {
  diagnostics_.clear();
  function_count_ = program.functions.size();
  types_.clear();
  enum_members_.clear();
  classes_.clear();
  functions_.clear();
  scopes_.clear();

  CollectTypes(program);
  ResolveDeclarations(program);
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

  return {std::move(diagnostics_), function_count_};
}

void TypeChecker::AddDiagnostic(SourceLocation location, std::string message) {
  diagnostics_.push_back({location, std::move(message)});
}

void TypeChecker::CollectTypes(const Program& program) {
  types_.emplace("void", Type{Type::Kind::Void, "void"});
  types_.emplace("bool", Type{Type::Kind::Bool, "bool"});
  types_.emplace("int", Type{Type::Kind::Int, "int"});
  types_.emplace("float", Type{Type::Kind::Float, "float"});
  types_.emplace("string", Type{Type::Kind::String, "string"});

  for (const Enum& enumeration : program.enums) {
    const Type type{Type::Kind::Enum, enumeration.name};
    if (!types_.emplace(enumeration.name, type).second) {
      AddDiagnostic(enumeration.location, "duplicate type '" + enumeration.name + "'");
      continue;
    }
    std::unordered_set<std::string_view> members;
    std::unordered_set<std::string>& stored_members = enum_members_[enumeration.name];
    for (const std::string& member : enumeration.members) {
      if (!members.emplace(member).second) {
        AddDiagnostic(enumeration.location, "duplicate enum member '" + member + "'");
      }
      stored_members.insert(member);
    }
  }

  for (const Class& class_declaration : program.classes) {
    const Type type{Type::Kind::Class, class_declaration.name};
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
  for (const Class& class_declaration : program.classes) {
    const auto class_info = classes_.find(class_declaration.name);
    if (class_info == classes_.end()) {
      continue;
    }
    ClassInfo& info = class_info->second;
    for (const Field& field : class_declaration.fields) {
      const Type type = ResolveType(field.type, false);
      if (!info.fields.emplace(field.name, type).second) {
        AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
      } else {
        info.field_order.push_back(field.name);
      }
    }
    for (const Function& method : class_declaration.methods) {
      FunctionInfo method_info;
      method_info.return_type = ResolveType(method.return_type, true);
      for (const Parameter& parameter : method.parameters) {
        const Type type = ResolveType(parameter.type, false);
        if (!method_info.parameters.emplace(parameter.name, type).second) {
          AddDiagnostic(parameter.location, "duplicate parameter '" + parameter.name + "'");
        } else {
          method_info.parameter_order.push_back(parameter.name);
        }
      }
      if (!info.methods.emplace(method.name, std::move(method_info)).second) {
        AddDiagnostic(method.location, "duplicate method '" + method.name + "'");
      }
    }
  }

  for (const Function& function : program.functions) {
    FunctionInfo function_info;
    function_info.return_type = ResolveType(function.return_type, true);
    for (const Parameter& parameter : function.parameters) {
      const Type type = ResolveType(parameter.type, false);
      if (!function_info.parameters.emplace(parameter.name, type).second) {
        AddDiagnostic(parameter.location, "duplicate parameter '" + parameter.name + "'");
      } else {
        function_info.parameter_order.push_back(parameter.name);
      }
    }
    if (!functions_.emplace(function.name, std::move(function_info)).second) {
      AddDiagnostic(function.location, "duplicate function '" + function.name + "'");
    }
  }
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

void TypeChecker::CheckStatement(const Statement& statement, Type return_type) {
  switch (statement.kind) {
    case Statement::Kind::VariableDeclaration: {
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
      break;
    }
    case Statement::Kind::Return: {
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
      break;
    }
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
  }
}

TypeChecker::Type TypeChecker::CheckExpression(const Expression& expression, const Type* expected) {
  switch (expression.kind) {
    case Expression::Kind::IntegerLiteral: {
      std::int64_t value = 0;
      const auto result = std::from_chars(expression.text.data(),
                                          expression.text.data() + expression.text.size(), value);
      if (result.ec != std::errc()) {
        AddDiagnostic(expression.location, "integer literal is out of range");
        return InvalidType();
      }
      return {Type::Kind::Int, "int"};
    }
    case Expression::Kind::FloatLiteral:
      return {Type::Kind::Float, "float"};
    case Expression::Kind::StringLiteral:
      return {Type::Kind::String, "string"};
    case Expression::Kind::BooleanLiteral:
      return {Type::Kind::Bool, "bool"};
    case Expression::Kind::Name:
      return LookupName(expression);
    case Expression::Kind::ShorthandMember: {
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
    case Expression::Kind::Unary: {
      const Type operand = CheckExpression(*expression.operand, nullptr);
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
    case Expression::Kind::Binary: {
      if (expression.text == "=") {
        if (!IsAssignable(*expression.left)) {
          AddDiagnostic(expression.left->location, "left side of assignment is not assignable");
          return InvalidType();
        }
        const Type left = CheckExpression(*expression.left, nullptr);
        const Type right = CheckExpression(*expression.right, &left);
        if (IsValid(left) && IsValid(right) && !IsSameType(left, right)) {
          AddDiagnostic(expression.location, "assignment types do not match");
          return InvalidType();
        }
        return left;
      }
      const Type left = CheckExpression(*expression.left, nullptr);
      const Type right = CheckExpression(*expression.right, nullptr);
      if (!IsValid(left) || !IsValid(right)) {
        return InvalidType();
      }
      if (expression.text == "==" || expression.text == "!=") {
        if (!IsSameType(left, right)) {
          AddDiagnostic(expression.location, "comparison operands must have the same type");
          return InvalidType();
        }
        return {Type::Kind::Bool, "bool"};
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
    case Expression::Kind::Call:
      return CheckCall(expression, expected);
    case Expression::Kind::Member:
      return CheckMemberAccess(expression);
  }
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckCall(const Expression& expression, const Type* expected) {
  if (expression.callee->kind == Expression::Kind::Name) {
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
  }

  AddDiagnostic(expression.location, "expression is not callable");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckFunctionCall(const FunctionInfo& function,
                                                 const std::vector<CallArgument>& arguments,
                                                 SourceLocation location) {
  std::vector<bool> assigned(function.parameter_order.size(), false);
  std::size_t positional = 0;
  bool named = false;
  for (const CallArgument& argument : arguments) {
    std::size_t index = positional;
    if (!argument.name.empty()) {
      named = true;
      const auto parameter = std::find(function.parameter_order.begin(),
                                       function.parameter_order.end(), argument.name);
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
    if (assigned[index]) {
      AddDiagnostic(argument.location, "call argument is specified more than once");
      continue;
    }
    assigned[index] = true;
    const auto expected_type = function.parameters.find(function.parameter_order[index]);
    const Type expected =
        expected_type == function.parameters.end() ? InvalidType() : expected_type->second;
    const Type actual = CheckExpression(*argument.value, &expected);
    if (IsValid(actual) && !IsSameType(expected, actual)) {
      AddDiagnostic(argument.location, "call argument type does not match parameter type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    if (!assigned[index]) {
      AddDiagnostic(location, "missing argument '" + function.parameter_order[index] + "'");
    }
  }
  return function.return_type;
}

TypeChecker::Type TypeChecker::CheckClassConstruction(const ClassInfo& class_info,
                                                      const std::vector<CallArgument>& arguments,
                                                      SourceLocation location) {
  std::vector<bool> assigned(class_info.field_order.size(), false);
  std::size_t positional = 0;
  bool named = false;
  for (const CallArgument& argument : arguments) {
    std::size_t index = positional;
    if (!argument.name.empty()) {
      named = true;
      const auto field =
          std::find(class_info.field_order.begin(), class_info.field_order.end(), argument.name);
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
    if (assigned[index]) {
      AddDiagnostic(argument.location, "constructor field is specified more than once");
      continue;
    }
    assigned[index] = true;
    const auto expected_field = class_info.fields.find(class_info.field_order[index]);
    const Type expected =
        expected_field == class_info.fields.end() ? InvalidType() : expected_field->second;
    const Type actual = CheckExpression(*argument.value, &expected);
    if (IsValid(actual) && !IsSameType(expected, actual)) {
      AddDiagnostic(argument.location, "constructor argument type does not match field type");
    }
  }
  for (std::size_t index = 0; index < assigned.size(); ++index) {
    if (!assigned[index]) {
      AddDiagnostic(location, "missing constructor field '" + class_info.field_order[index] + "'");
    }
  }
  return class_info.type;
}

TypeChecker::Type TypeChecker::CheckMemberAccess(const Expression& expression) {
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

TypeChecker::Type TypeChecker::ResolveType(const TypeName& type_name, bool allow_void) {
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

TypeChecker::Type TypeChecker::LookupName(const Expression& expression) {
  const Type local = LookupLocal(expression.text);
  if (IsValid(local)) {
    return local;
  }
  AddDiagnostic(expression.location, "unknown name '" + expression.text + "'");
  return InvalidType();
}

TypeChecker::Type TypeChecker::LookupLocal(std::string_view name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto value = scope->find(std::string(name));
    if (value != scope->end()) {
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

bool TypeChecker::DeclareLocal(std::string_view name, Type type, SourceLocation location) {
  auto& scope = scopes_.back();
  if (!scope.emplace(name, type).second) {
    AddDiagnostic(location, "duplicate local name '" + std::string(name) + "'");
    return false;
  }
  return true;
}

bool TypeChecker::IsAssignable(const Expression& expression) const {
  return expression.kind == Expression::Kind::Name || expression.kind == Expression::Kind::Member;
}

bool TypeChecker::IsSameType(Type left, Type right) const {
  return left.kind == right.kind && left.name == right.name;
}

bool TypeChecker::IsNumeric(Type type) const {
  return type.kind == Type::Kind::Int || type.kind == Type::Kind::Float;
}

bool TypeChecker::IsValid(Type type) const {
  return type.kind != Type::Kind::Invalid;
}

std::string TypeChecker::TypeNameFor(Type type) const {
  return type.name.empty() ? "<invalid>" : type.name;
}

}  // namespace vela::frontend
