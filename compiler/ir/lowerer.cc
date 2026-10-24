#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <utility>

namespace vela::ir {
namespace {

frontend::TypeDescriptor InvalidType() {
  return {};
}

}  // namespace

LoweringResult Lowerer::Lower() {
  program_ = {};
  diagnostics_.clear();
  class_ids_.clear();
  function_ids_.clear();
  BuildClassTable();
  BuildFunctionTable();
  for (const frontend::Function& function : source_.functions) {
    LowerFunction(function);
  }
  return {std::move(program_), std::move(diagnostics_)};
}

void Lowerer::AddDiagnostic(frontend::SourceLocation location, std::string message) {
  diagnostics_.push_back({location, std::move(message)});
}

void Lowerer::BuildClassTable() {
  for (const frontend::Class& source_class : source_.classes) {
    const auto class_info = model_.classes.find(source_class.name);
    if (class_info == model_.classes.end()) {
      continue;
    }
    const ClassId class_id = static_cast<ClassId>(program_.classes.size());
    class_ids_.emplace(source_class.name, class_id);

    Class output_class;
    output_class.name = source_class.name;
    output_class.field_order = class_info->second.field_order;
    output_class.field_types.reserve(output_class.field_order.size());
    for (const std::string& field_name : output_class.field_order) {
      const auto field = class_info->second.fields.find(field_name);
      output_class.field_types.push_back(field == class_info->second.fields.end() ? InvalidType()
                                                                                  : field->second);
    }
    program_.classes.push_back(std::move(output_class));
  }
}

void Lowerer::BuildFunctionTable() {
  for (std::size_t index = 0; index < source_.functions.size(); ++index) {
    function_ids_.emplace(source_.functions[index].name, static_cast<std::uint32_t>(index));
  }
}

void Lowerer::LowerFunction(const frontend::Function& source) {
  Function function;
  function.name = source.name;
  const auto function_info = model_.functions.find(source.name);
  if (function_info == model_.functions.end()) {
    return;
  }
  function.return_type = function_info->second.return_type;
  function.parameter_count = static_cast<std::uint16_t>(source.parameters.size());
  current_function_ = &function;
  next_register_ = 0;
  scopes_.clear();
  PushScope();

  for (const frontend::Parameter& parameter : source.parameters) {
    const auto parameter_info = function_info->second.parameters.find(parameter.name);
    const frontend::TypeDescriptor type = parameter_info == function_info->second.parameters.end()
                                              ? InvalidType()
                                              : parameter_info->second;
    const RegisterId reg = AllocateRegister(parameter.location);
    DeclareLocal(parameter.name, reg, type, parameter.location);
  }

  if (source.body != nullptr) {
    LowerBlock(*source.body);
  }
  PopScope();

  if (function.return_type.kind == frontend::TypeDescriptor::Kind::Void &&
      (function.instructions.empty() || function.instructions.back().opcode != Opcode::Return)) {
    const RegisterId result = AllocateTemporary(source.location);
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = source.location});
    Emit({.opcode = Opcode::Return, .dst = result, .location = source.location});
  }
  if (function.return_type.kind != frontend::TypeDescriptor::Kind::Void &&
      (function.instructions.empty() || function.instructions.back().opcode != Opcode::Return)) {
    AddDiagnostic(source.location, "non-void function may fall through without returning");
  }

  function.register_count = next_register_;
  if (function.register_count == 0) {
    AddDiagnostic(source.location, "function requires at least one register");
  }

  program_.functions.push_back(std::move(function));
  current_function_ = nullptr;
}

void Lowerer::LowerStatement(const frontend::Statement& statement) {
  switch (statement.kind) {
    case frontend::Statement::Kind::VariableDeclaration: {
      const RegisterId local = AllocateRegister(statement.location);
      const frontend::TypeDescriptor type = model_.types.contains(statement.type.name)
                                                ? model_.types.at(statement.type.name)
                                                : InvalidType();
      if (statement.expression != nullptr) {
        const LoweredValue initializer = LowerExpression(*statement.expression, &type);
        if (initializer.type.kind != frontend::TypeDescriptor::Kind::Invalid &&
            !IsSameType(type, initializer.type)) {
          AddDiagnostic(statement.location, "initializer type does not match local type");
        }
        Emit({.opcode = Opcode::Move,
              .dst = local,
              .lhs = initializer.reg,
              .location = statement.location});
      }
      DeclareLocal(statement.name, local, type, statement.location);
      break;
    }
    case frontend::Statement::Kind::Return: {
      if (statement.expression == nullptr) {
        const RegisterId result = AllocateTemporary(statement.location);
        Emit({.opcode = Opcode::LoadNull, .dst = result, .location = statement.location});
        Emit({.opcode = Opcode::Return, .dst = result, .location = statement.location});
        break;
      }
      const LoweredValue value =
          LowerExpression(*statement.expression, &current_function_->return_type);
      Emit({.opcode = Opcode::Return, .dst = value.reg, .location = statement.location});
      break;
    }
    case frontend::Statement::Kind::Expression:
      if (statement.expression != nullptr) {
        static_cast<void>(LowerExpression(*statement.expression));
      }
      break;
    case frontend::Statement::Kind::Block:
      LowerBlock(statement);
      break;
    case frontend::Statement::Kind::If: {
      const LoweredValue condition = LowerExpression(*statement.condition);
      if (!IsBoolean(condition.type)) {
        AddDiagnostic(statement.condition->location, "if condition must be bool");
      }
      const std::size_t false_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
      LowerStatement(*statement.then_branch);
      if (statement.else_branch != nullptr) {
        const std::size_t end_jump = current_function_->instructions.size();
        Emit({.opcode = Opcode::Jump, .location = statement.location});
        current_function_->instructions[false_jump].index =
            static_cast<std::uint32_t>(current_function_->instructions.size());
        LowerStatement(*statement.else_branch);
        current_function_->instructions[end_jump].index =
            static_cast<std::uint32_t>(current_function_->instructions.size());
      } else {
        current_function_->instructions[false_jump].index =
            static_cast<std::uint32_t>(current_function_->instructions.size());
      }
      break;
    }
    case frontend::Statement::Kind::While: {
      const std::size_t loop_start = current_function_->instructions.size();
      const LoweredValue condition = LowerExpression(*statement.condition);
      if (!IsBoolean(condition.type)) {
        AddDiagnostic(statement.condition->location, "while condition must be bool");
      }
      const std::size_t exit_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
      LowerStatement(*statement.then_branch);
      Emit({.opcode = Opcode::Jump,
            .index = static_cast<std::uint32_t>(loop_start),
            .location = statement.location});
      current_function_->instructions[exit_jump].index =
          static_cast<std::uint32_t>(current_function_->instructions.size());
      break;
    }
  }
}

void Lowerer::LowerBlock(const frontend::Statement& statement) {
  PushScope();
  for (const std::unique_ptr<frontend::Statement>& child : statement.statements) {
    LowerStatement(*child);
  }
  PopScope();
}

Lowerer::LoweredValue Lowerer::LowerExpression(const frontend::Expression& expression,
                                               const frontend::TypeDescriptor* expected) {
  switch (expression.kind) {
    case frontend::Expression::Kind::IntegerLiteral: {
      std::int64_t value = 0;
      const auto result = std::from_chars(expression.text.data(),
                                          expression.text.data() + expression.text.size(), value);
      if (result.ec != std::errc()) {
        AddDiagnostic(expression.location, "integer literal is out of range");
        return {0, InvalidType()};
      }
      const RegisterId reg = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadInt,
            .dst = reg,
            .immediate = value,
            .location = expression.location});
      return {reg, {frontend::TypeDescriptor::Kind::Int, "int"}};
    }
    case frontend::Expression::Kind::BooleanLiteral: {
      const RegisterId reg = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadBool,
            .dst = reg,
            .immediate = expression.boolean_value ? 1 : 0,
            .location = expression.location});
      return {reg, {frontend::TypeDescriptor::Kind::Bool, "bool"}};
    }
    case frontend::Expression::Kind::FloatLiteral:
    case frontend::Expression::Kind::StringLiteral:
      AddDiagnostic(expression.location, "this value type is not supported by the bootstrap VM");
      return {0, InvalidType()};
    case frontend::Expression::Kind::Name: {
      const Local* local = FindLocal(expression.text);
      if (local == nullptr) {
        AddDiagnostic(expression.location, "unknown local during lowering");
        return {0, InvalidType()};
      }
      return {local->reg, local->type};
    }
    case frontend::Expression::Kind::ShorthandMember: {
      if (expected == nullptr || expected->kind != frontend::TypeDescriptor::Kind::Enum) {
        AddDiagnostic(expression.location, "enum shorthand has no expected enum type");
        return {0, InvalidType()};
      }
      const auto members = model_.enum_members.find(expected->name);
      if (members == model_.enum_members.end()) {
        return {0, InvalidType()};
      }
      std::uint32_t ordinal = 0;
      bool found = false;
      for (const frontend::Enum& enumeration : source_.enums) {
        if (enumeration.name != expected->name) {
          continue;
        }
        for (std::size_t index = 0; index < enumeration.members.size(); ++index) {
          if (enumeration.members[index] == expression.text) {
            ordinal = static_cast<std::uint32_t>(index);
            found = true;
            break;
          }
        }
      }
      if (!found) {
        AddDiagnostic(expression.location, "unknown enum member during lowering");
        return {0, InvalidType()};
      }
      const RegisterId reg = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadInt,
            .dst = reg,
            .immediate = ordinal,
            .location = expression.location});
      return {reg, *expected};
    }
    case frontend::Expression::Kind::Unary: {
      const LoweredValue operand = LowerExpression(*expression.operand);
      const RegisterId result = AllocateTemporary(expression.location);
      if (expression.text == "-" && IsInteger(operand.type)) {
        Emit({.opcode = Opcode::NegInt,
              .dst = result,
              .lhs = operand.reg,
              .location = expression.location});
        return {result, operand.type};
      }
      if (expression.text == "!" && IsBoolean(operand.type)) {
        Emit({.opcode = Opcode::NotBool,
              .dst = result,
              .lhs = operand.reg,
              .location = expression.location});
        return {result, operand.type};
      }
      AddDiagnostic(expression.location, "unsupported unary operation in bootstrap VM");
      return {result, InvalidType()};
    }
    case frontend::Expression::Kind::Binary: {
      if (expression.text == "=") {
        return LowerAssignment(*expression.left, *expression.right);
      }
      const LoweredValue left = LowerExpression(*expression.left);
      const LoweredValue right = LowerExpression(*expression.right, &left.type);
      const RegisterId result = AllocateTemporary(expression.location);
      Opcode opcode = Opcode::AddInt;
      frontend::TypeDescriptor result_type = left.type;
      if (expression.text == "+") {
        opcode = Opcode::AddInt;
      } else if (expression.text == "-") {
        opcode = Opcode::SubInt;
      } else if (expression.text == "*") {
        opcode = Opcode::MulInt;
      } else if (expression.text == "/") {
        opcode = Opcode::DivInt;
      } else if (expression.text == "==") {
        opcode =
            IsBoolean(left.type) && IsBoolean(right.type) ? Opcode::EqualBool : Opcode::EqualInt;
        result_type = {frontend::TypeDescriptor::Kind::Bool, "bool"};
      } else if (expression.text == "!=") {
        opcode = IsBoolean(left.type) && IsBoolean(right.type) ? Opcode::NotEqualBool
                                                               : Opcode::NotEqualInt;
        result_type = {frontend::TypeDescriptor::Kind::Bool, "bool"};
      } else {
        AddDiagnostic(expression.location, "unsupported binary operation in bootstrap VM");
        return {result, InvalidType()};
      }
      const bool comparable_booleans = IsBoolean(left.type) && IsBoolean(right.type) &&
                                       (expression.text == "==" || expression.text == "!=");
      if ((!IsInteger(left.type) || !IsInteger(right.type)) && !comparable_booleans) {
        AddDiagnostic(expression.location, "bootstrap VM arithmetic requires int operands");
        return {result, InvalidType()};
      }
      Emit({.opcode = opcode,
            .dst = result,
            .lhs = left.reg,
            .rhs = right.reg,
            .location = expression.location});
      return {result, result_type};
    }
    case frontend::Expression::Kind::Call:
      return LowerCall(expression, expected);
    case frontend::Expression::Kind::Member:
      return LowerMember(expression);
  }
  return {0, InvalidType()};
}

Lowerer::LoweredValue Lowerer::LowerAssignment(const frontend::Expression& left,
                                               const frontend::Expression& right) {
  if (left.kind == frontend::Expression::Kind::Name) {
    const Local* local = FindLocal(left.text);
    if (local == nullptr) {
      AddDiagnostic(left.location, "unknown local during assignment lowering");
      return {0, InvalidType()};
    }
    const LoweredValue value = LowerExpression(right, &local->type);
    Emit({.opcode = Opcode::Move, .dst = local->reg, .lhs = value.reg, .location = left.location});
    return {local->reg, local->type};
  }
  if (left.kind == frontend::Expression::Kind::Member) {
    const LoweredValue object = LowerExpression(*left.object);
    if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
      AddDiagnostic(left.location, "field assignment requires a class value");
      return {0, InvalidType()};
    }
    const frontend::ClassDescriptor* class_info = FindClass(object.type.name);
    const std::optional<FieldId> field =
        class_info == nullptr ? std::nullopt : FindField(*class_info, left.text);
    if (!field.has_value() || class_info == nullptr) {
      AddDiagnostic(left.location, "unknown field during assignment lowering");
      return {0, InvalidType()};
    }
    const frontend::TypeDescriptor field_type = class_info->fields.at(left.text);
    const LoweredValue value = LowerExpression(right, &field_type);
    Emit({.opcode = Opcode::SetField,
          .dst = object.reg,
          .lhs = value.reg,
          .index = *field,
          .location = left.location});
    return {value.reg, field_type};
  }
  AddDiagnostic(left.location, "left side of assignment is not lowerable");
  return {0, InvalidType()};
}

Lowerer::LoweredValue Lowerer::LowerConstruction(const frontend::Expression& expression,
                                                 const frontend::TypeDescriptor& expected) {
  const auto class_id = class_ids_.find(expression.callee->text);
  const frontend::ClassDescriptor* class_info = FindClass(expression.callee->text);
  if (class_id == class_ids_.end() || class_info == nullptr) {
    AddDiagnostic(expression.location, "unknown class during lowering");
    return {0, InvalidType()};
  }

  const RegisterId object = AllocateTemporary(expression.location);
  Emit({.opcode = Opcode::NewInstance,
        .dst = object,
        .index = class_id->second,
        .location = expression.location});
  std::vector<bool> assigned(class_info->field_order.size(), false);
  std::size_t positional = 0;
  bool named = false;
  for (const frontend::CallArgument& argument : expression.arguments) {
    std::size_t field_index = positional;
    if (!argument.name.empty()) {
      named = true;
      const std::optional<FieldId> named_field = FindField(*class_info, argument.name);
      if (!named_field.has_value()) {
        continue;
      }
      field_index = *named_field;
    } else {
      if (named) {
        continue;
      }
      ++positional;
    }
    if (field_index >= class_info->field_order.size() || assigned[field_index]) {
      continue;
    }
    assigned[field_index] = true;
    const frontend::TypeDescriptor& field_type =
        class_info->fields.at(class_info->field_order[field_index]);
    const LoweredValue value = LowerExpression(*argument.value, &field_type);
    Emit({.opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = static_cast<std::uint32_t>(field_index),
          .location = argument.location});
  }
  return {object, expected};
}

Lowerer::LoweredValue Lowerer::LowerMember(const frontend::Expression& expression) {
  const LoweredValue object = LowerExpression(*expression.object);
  if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
    AddDiagnostic(expression.location, "member access requires a class value");
    return {0, InvalidType()};
  }
  const frontend::ClassDescriptor* class_info = FindClass(object.type.name);
  const std::optional<FieldId> field =
      class_info == nullptr ? std::nullopt : FindField(*class_info, expression.text);
  if (!field.has_value() || class_info == nullptr) {
    AddDiagnostic(expression.location, "unknown field during lowering");
    return {0, InvalidType()};
  }
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({.opcode = Opcode::GetField,
        .dst = result,
        .lhs = object.reg,
        .index = *field,
        .location = expression.location});
  return {result, class_info->fields.at(expression.text)};
}

Lowerer::LoweredValue Lowerer::LowerCall(const frontend::Expression& expression,
                                         const frontend::TypeDescriptor* expected) {
  if (expression.callee->kind == frontend::Expression::Kind::Name) {
    const auto class_info = model_.classes.find(expression.callee->text);
    if (class_info != model_.classes.end()) {
      return LowerConstruction(expression, class_info->second.type);
    }
    const auto function_id = function_ids_.find(expression.callee->text);
    const auto function_info = model_.functions.find(expression.callee->text);
    if (function_id != function_ids_.end() && function_info != model_.functions.end()) {
      const std::size_t parameter_count = function_info->second.parameter_order.size();
      const RegisterId argument_base = next_register_;
      for (std::size_t index = 0; index < parameter_count; ++index) {
        static_cast<void>(AllocateRegister(expression.location));
      }

      std::vector<bool> assigned(parameter_count, false);
      std::size_t positional = 0;
      bool named = false;
      for (const frontend::CallArgument& argument : expression.arguments) {
        std::size_t parameter_index = positional;
        if (!argument.name.empty()) {
          named = true;
          const auto parameter =
              std::find(function_info->second.parameter_order.begin(),
                        function_info->second.parameter_order.end(), argument.name);
          if (parameter == function_info->second.parameter_order.end()) {
            continue;
          }
          parameter_index =
              static_cast<std::size_t>(parameter - function_info->second.parameter_order.begin());
        } else {
          if (named) {
            continue;
          }
          ++positional;
        }
        if (parameter_index >= parameter_count || assigned[parameter_index]) {
          continue;
        }
        assigned[parameter_index] = true;
        const auto parameter_type = function_info->second.parameters.find(
            function_info->second.parameter_order[parameter_index]);
        const frontend::TypeDescriptor expected_type =
            parameter_type == function_info->second.parameters.end() ? InvalidType()
                                                                     : parameter_type->second;
        const LoweredValue value = LowerExpression(*argument.value, &expected_type);
        Emit({.opcode = Opcode::Move,
              .dst = static_cast<RegisterId>(argument_base + parameter_index),
              .lhs = value.reg,
              .location = argument.location});
      }

      const RegisterId result = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::Call,
            .dst = result,
            .lhs = argument_base,
            .index = function_id->second,
            .location = expression.location});
      return {result, function_info->second.return_type};
    }
  }
  AddDiagnostic(expression.location, "function or method is not lowerable");
  return {0, expected == nullptr ? InvalidType() : *expected};
}

const frontend::ClassDescriptor* Lowerer::FindClass(std::string_view name) const {
  const auto found = model_.classes.find(std::string(name));
  return found == model_.classes.end() ? nullptr : &found->second;
}

const Lowerer::Local* Lowerer::FindLocal(std::string_view name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto found = scope->find(std::string(name));
    if (found != scope->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

RegisterId Lowerer::AllocateRegister(frontend::SourceLocation location) {
  if (next_register_ == std::numeric_limits<RegisterId>::max()) {
    AddDiagnostic(location, "function uses too many registers");
    return next_register_;
  }
  return next_register_++;
}

RegisterId Lowerer::AllocateTemporary(frontend::SourceLocation location) {
  return AllocateRegister(location);
}

bool Lowerer::IsSameType(const frontend::TypeDescriptor& left,
                         const frontend::TypeDescriptor& right) const {
  return left.kind == right.kind && left.name == right.name;
}

bool Lowerer::IsInteger(const frontend::TypeDescriptor& type) const {
  return type.kind == frontend::TypeDescriptor::Kind::Int ||
         type.kind == frontend::TypeDescriptor::Kind::Enum;
}

bool Lowerer::IsBoolean(const frontend::TypeDescriptor& type) const {
  return type.kind == frontend::TypeDescriptor::Kind::Bool;
}

std::optional<FieldId> Lowerer::FindField(const frontend::ClassDescriptor& class_info,
                                          std::string_view name) const {
  const auto field = std::find(class_info.field_order.begin(), class_info.field_order.end(), name);
  if (field == class_info.field_order.end()) {
    return std::nullopt;
  }
  return static_cast<FieldId>(field - class_info.field_order.begin());
}

void Lowerer::Emit(Instruction instruction) {
  if (current_function_ != nullptr) {
    current_function_->instructions.push_back(std::move(instruction));
  }
}

void Lowerer::PushScope() {
  scopes_.emplace_back();
}

void Lowerer::PopScope() {
  scopes_.pop_back();
}

bool Lowerer::DeclareLocal(std::string_view name,
                           RegisterId reg,
                           frontend::TypeDescriptor type,
                           frontend::SourceLocation location) {
  auto& scope = scopes_.back();
  if (!scope.emplace(std::string(name), Local{reg, std::move(type)}).second) {
    AddDiagnostic(location, "duplicate local during lowering");
    return false;
  }
  return true;
}

}  // namespace vela::ir
