#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace vela::ir {
namespace {

auto InvalidType() -> frontend::TypeDescriptor {
  return {};
}

auto SelectBinaryOpcode(std::string_view operation, const frontend::TypeDescriptor& type)
    -> std::optional<Opcode> {
  using Kind = frontend::TypeDescriptor::Kind;
  if (type.kind == Kind::String) {
    if (operation == "+") {
      return Opcode::StringConcat;
    }
    if (operation == "==") {
      return Opcode::EqualString;
    }
    if (operation == "!=") {
      return Opcode::NotEqualString;
    }
    return std::nullopt;
  }
  if (type.kind == Kind::Bool) {
    if (operation == "==") {
      return Opcode::EqualBool;
    }
    if (operation == "!=") {
      return Opcode::NotEqualBool;
    }
    return std::nullopt;
  }
  if (operation == "+") {
    return Opcode::AddInt;
  }
  if (operation == "-") {
    return Opcode::SubInt;
  }
  if (operation == "*") {
    return Opcode::MulInt;
  }
  if (operation == "/") {
    return Opcode::DivInt;
  }
  if (operation == "==") {
    return Opcode::EqualInt;
  }
  if (operation == "!=") {
    return Opcode::NotEqualInt;
  }
  if (operation == "<") {
    return Opcode::LessInt;
  }
  if (operation == "<=") {
    return Opcode::LessEqualInt;
  }
  if (operation == ">") {
    return Opcode::GreaterInt;
  }
  if (operation == ">=") {
    return Opcode::GreaterEqualInt;
  }
  return std::nullopt;
}

}  // namespace

auto Lowerer::Lower() -> LoweringResult {
  program_ = {};
  diagnostics_.clear();
  class_ids_.clear();
  function_ids_.clear();
  BuildClassTable();
  BuildFunctionTable();
  for (const frontend::Function& function : source_.functions) {
    LowerFunction(function);
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      LowerFunction(method, &owner);
    }
  }
  return {.program = std::move(program_), .diagnostics = std::move(diagnostics_)};
}

void Lowerer::AddDiagnostic(frontend::SourceLocation location, std::string message) {
  diagnostics_.push_back({.location = location, .message = std::move(message)});
}

void Lowerer::BuildClassTable() {
  for (const frontend::Class& source_class : source_.classes) {
    const auto class_info = model_.classes.find(source_class.name);
    if (class_info == model_.classes.end()) {
      continue;
    }
    const auto class_id = static_cast<ClassId>(program_.classes.size());
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
    function_ids_.emplace(source_.functions.at(index).name, static_cast<std::uint32_t>(index));
  }
  auto next = static_cast<std::uint32_t>(source_.functions.size());
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      function_ids_.emplace(owner.name + "." + method.name, next++);
    }
  }
}

void Lowerer::LowerFunction(const frontend::Function& source, const frontend::Class* owner) {
  Function function;
  function.name = owner == nullptr ? source.name : owner->name + "." + source.name;
  function.owner_class = owner == nullptr ? std::string() : owner->name;
  const frontend::FunctionDescriptor* function_info = nullptr;
  if (owner == nullptr) {
    const auto found = model_.functions.find(source.name);
    if (found == model_.functions.end()) {
      return;
    }
    function_info = &found->second;
  } else {
    const auto class_found = model_.classes.find(owner->name);
    if (class_found == model_.classes.end()) {
      return;
    }
    const auto found = class_found->second.methods.find(source.name);
    if (found == class_found->second.methods.end()) {
      return;
    }
    function_info = &found->second;
  }
  function.return_type = function_info->return_type;
  function.parameter_count =
      static_cast<std::uint16_t>(source.parameters.size() + ((owner != nullptr) ? 1 : 0));
  current_function_ = &function;
  next_register_ = 0;
  scopes_.clear();
  loops_.clear();
  PushScope();

  if (owner != nullptr) {
    const RegisterId this_reg = AllocateRegister(source.location);
    DeclareLocal("this", this_reg, model_.classes.at(owner->name).type, source.location);
  }

  for (const frontend::Parameter& parameter : source.parameters) {
    const auto parameter_info = function_info->parameters.find(parameter.name);
    const frontend::TypeDescriptor type =
        parameter_info == function_info->parameters.end() ? InvalidType() : parameter_info->second;
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
    case frontend::Statement::Kind::VariableDeclaration:
      LowerVariableDeclaration(statement);
      break;
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
    case frontend::Statement::Kind::If:
      LowerIf(statement);
      break;
    case frontend::Statement::Kind::While:
      LowerWhile(statement);
      break;
    case frontend::Statement::Kind::For:
      LowerFor(statement);
      break;
    case frontend::Statement::Kind::Break: {
      if (loops_.empty()) {
        break;
      }
      const std::size_t jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::Jump, .location = statement.location});
      loops_.back().break_jumps.push_back(jump);
      break;
    }
    case frontend::Statement::Kind::Continue: {
      if (loops_.empty()) {
        break;
      }
      const std::size_t jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::Jump, .location = statement.location});
      loops_.back().continue_jumps.push_back(jump);
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

auto Lowerer::LowerExpression(const frontend::Expression& expression,
                              const frontend::TypeDescriptor* expected) -> Lowerer::LoweredValue {
  switch (expression.kind) {
    case frontend::Expression::Kind::IntegerLiteral:
      return LowerIntegerLiteral(expression);
    case frontend::Expression::Kind::BooleanLiteral: {
      const RegisterId reg = AllocateTemporary(expression.location);
      Emit({
          .opcode = Opcode::LoadBool,
          .dst = reg,
          .immediate = expression.boolean_value ? 1 : 0,
          .location = expression.location,
      });
      return {.reg = reg, .type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = "bool"}};
    }
    case frontend::Expression::Kind::FloatLiteral:
      AddDiagnostic(expression.location, "this value type is not supported by the bootstrap VM");
      return {.reg = 0, .type = InvalidType()};
    case frontend::Expression::Kind::StringLiteral:
      return LowerStringLiteral(expression);
    case frontend::Expression::Kind::ArrayLiteral:
      return LowerArrayLiteral(expression);
    case frontend::Expression::Kind::Index:
      return LowerIndex(expression);
    case frontend::Expression::Kind::Name: {
      const Local* local = FindLocal(expression.text);
      if (local == nullptr) {
        AddDiagnostic(expression.location, "unknown local during lowering");
        return {.reg = 0, .type = InvalidType()};
      }
      return {.reg = local->reg, .type = local->type};
    }
    case frontend::Expression::Kind::ShorthandMember:
      return LowerShorthandMember(expression, expected);
    case frontend::Expression::Kind::Unary:
      return LowerUnary(expression);
    case frontend::Expression::Kind::Binary:
      return LowerBinary(expression);
    case frontend::Expression::Kind::Call:
      return LowerCall(expression, expected);
    case frontend::Expression::Kind::Member:
      return LowerMember(expression);
  }
  return {.reg = 0, .type = InvalidType()};
}

auto Lowerer::LowerAssignment(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const auto& left = *expression.left;
  const auto& right = *expression.right;
  if (left.kind == frontend::Expression::Kind::Name) {
    const Local* local = FindLocal(left.text);
    if (local == nullptr) {
      AddDiagnostic(left.location, "unknown local during assignment lowering");
      return {.reg = 0, .type = InvalidType()};
    }
    const LoweredValue value = LowerExpression(right, &local->type);
    Emit({.opcode = Opcode::Move, .dst = local->reg, .lhs = value.reg, .location = left.location});
    return {.reg = local->reg, .type = local->type};
  }
  if (left.kind == frontend::Expression::Kind::Member) {
    const LoweredValue object = LowerExpression(*left.object);
    if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
      AddDiagnostic(left.location, "field assignment requires a class value");
      return {.reg = 0, .type = InvalidType()};
    }
    const frontend::ClassDescriptor* class_info = FindClass(object.type.name);
    const std::optional<FieldId> field =
        class_info == nullptr ? std::nullopt : FindField(*class_info, left.text);
    if (!field.has_value() || class_info == nullptr) {
      AddDiagnostic(left.location, "unknown field during assignment lowering");
      return {.reg = 0, .type = InvalidType()};
    }
    const frontend::TypeDescriptor field_type = class_info->fields.at(left.text);
    const LoweredValue value = LowerExpression(right, &field_type);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object.reg,
        .lhs = value.reg,
        .index = *field,
        .location = left.location,
    });
    return {.reg = value.reg, .type = field_type};
  }
  if (left.kind == frontend::Expression::Kind::Index) {
    const LoweredValue array = LowerExpression(*left.object);
    const LoweredValue index = LowerExpression(*left.index);
    const LoweredValue value = LowerExpression(right);
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = array.reg,
        .lhs = index.reg,
        .rhs = value.reg,
        .location = left.location,
    });
    return {.reg = value.reg, .type = {.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"}};
  }
  AddDiagnostic(left.location, "left side of assignment is not lowerable");
  return {.reg = 0, .type = InvalidType()};
}

auto Lowerer::LowerConstruction(const frontend::Expression& expression,
                                const frontend::TypeDescriptor& expected) -> Lowerer::LoweredValue {
  const auto class_id = class_ids_.find(expression.callee->text);
  const frontend::ClassDescriptor* class_info = FindClass(expression.callee->text);
  if (class_id == class_ids_.end() || class_info == nullptr) {
    AddDiagnostic(expression.location, "unknown class during lowering");
    return {.reg = 0, .type = InvalidType()};
  }

  const RegisterId object = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = class_id->second,
      .location = expression.location,
  });
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
    if (field_index >= class_info->field_order.size() || assigned.at(field_index)) {
      continue;
    }
    assigned.at(field_index) = true;
    const frontend::TypeDescriptor& field_type =
        class_info->fields.at(class_info->field_order.at(field_index));
    const LoweredValue value = LowerExpression(*argument.value, &field_type);
    Emit({
        .opcode = Opcode::SetField,
        .dst = object,
        .lhs = value.reg,
        .index = static_cast<std::uint32_t>(field_index),
        .location = argument.location,
    });
  }
  return {.reg = object, .type = expected};
}

auto Lowerer::LowerMember(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const LoweredValue object = LowerExpression(*expression.object);
  if (object.type.kind != frontend::TypeDescriptor::Kind::Class) {
    AddDiagnostic(expression.location, "member access requires a class value");
    return {.reg = 0, .type = InvalidType()};
  }
  const frontend::ClassDescriptor* class_info = FindClass(object.type.name);
  const std::optional<FieldId> field =
      class_info == nullptr ? std::nullopt : FindField(*class_info, expression.text);
  if (!field.has_value() || class_info == nullptr) {
    AddDiagnostic(expression.location, "unknown field during lowering");
    return {.reg = 0, .type = InvalidType()};
  }
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::GetField,
      .dst = result,
      .lhs = object.reg,
      .index = *field,
      .location = expression.location,
  });
  return {.reg = result, .type = class_info->fields.at(expression.text)};
}

auto Lowerer::LowerIndex(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const LoweredValue array = LowerExpression(*expression.object);
  const LoweredValue index = LowerExpression(*expression.index);
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::GetIndex,
      .dst = result,
      .lhs = array.reg,
      .rhs = index.reg,
      .location = expression.location,
  });
  return {.reg = result, .type = {.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"}};
}

auto Lowerer::LowerCall(const frontend::Expression& expression,
                        const frontend::TypeDescriptor* expected) -> LoweredValue {
  const auto& callee = *expression.callee;
  if (callee.kind == frontend::Expression::Kind::Name) {
    if (callee.text == "print") {
      const auto value = LowerExpression(*expression.arguments.front().value);
      Emit({.opcode = Opcode::Print, .lhs = value.reg, .location = expression.location});
      const RegisterId result = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadNull, .dst = result, .location = expression.location});
      return {
          .reg = result,
          .type = {.kind = frontend::TypeDescriptor::Kind::Void, .name = "void"},
      };
    }
    const auto class_info = model_.classes.find(callee.text);
    if (class_info != model_.classes.end()) {
      return LowerConstruction(expression, class_info->second.type);
    }
    const auto function = model_.functions.find(callee.text);
    if (function != model_.functions.end()) {
      return LowerInvocation(expression, function->second, function_ids_.at(callee.text), nullptr);
    }
  }
  if (callee.kind == frontend::Expression::Kind::Member) {
    const auto receiver = LowerExpression(*callee.object);
    const auto* owner = FindClass(receiver.type.name);
    if (owner != nullptr) {
      const auto method = owner->methods.find(callee.text);
      if (method != owner->methods.end()) {
        const auto method_id = function_ids_.at(receiver.type.name + "." + callee.text);
        return LowerInvocation(expression, method->second, method_id, &receiver);
      }
    }
  }
  AddDiagnostic(expression.location, "function or method is not lowerable");
  return {.reg = 0, .type = expected == nullptr ? InvalidType() : *expected};
}

auto Lowerer::LowerInvocation(const frontend::Expression& expression,
                              const frontend::FunctionDescriptor& function,
                              std::uint32_t function_id,
                              const LoweredValue* receiver) -> LoweredValue {
  const RegisterId base = next_register_;
  const std::size_t receiver_count = receiver == nullptr ? 0 : 1;
  const std::size_t count = receiver_count + function.parameter_order.size();
  for (std::size_t index = 0; index < count; ++index) {
    static_cast<void>(AllocateRegister(expression.location));
  }
  if (receiver != nullptr) {
    Emit({
        .opcode = Opcode::Move,
        .dst = base,
        .lhs = receiver->reg,
        .location = expression.location,
    });
  }
  std::size_t positional = 0;
  for (const auto& argument : expression.arguments) {
    std::size_t index = positional++;
    if (!argument.name.empty()) {
      const auto parameter = std::ranges::find(function.parameter_order, argument.name);
      index = static_cast<std::size_t>(parameter - function.parameter_order.begin());
    }
    const auto& type = function.parameters.at(function.parameter_order.at(index));
    const auto value = LowerExpression(*argument.value, &type);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + receiver_count + index),
        .lhs = value.reg,
        .location = argument.location,
    });
  }
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::Call,
      .dst = result,
      .lhs = base,
      .index = function_id,
      .location = expression.location,
  });
  return {.reg = result, .type = function.return_type};
}

auto Lowerer::FindClass(std::string_view name) const -> const frontend::ClassDescriptor* {
  const auto found = model_.classes.find(name);
  return found == model_.classes.end() ? nullptr : &found->second;
}

auto Lowerer::FindLocal(std::string_view name) const -> const Lowerer::Local* {
  for (const auto& scope : std::views::reverse(scopes_)) {
    const auto found = scope.find(name);
    if (found != scope.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto Lowerer::AllocateRegister(frontend::SourceLocation location) -> RegisterId {
  if (next_register_ == std::numeric_limits<RegisterId>::max()) {
    AddDiagnostic(location, "function uses too many registers");
    return next_register_;
  }
  return next_register_++;
}

auto Lowerer::AllocateTemporary(frontend::SourceLocation location) -> RegisterId {
  return AllocateRegister(location);
}

auto Lowerer::IsSameType(const frontend::TypeDescriptor& left,
                         const frontend::TypeDescriptor& right) -> bool {
  return left.kind == right.kind && left.name == right.name;
}

auto Lowerer::IsInteger(const frontend::TypeDescriptor& type) -> bool {
  return type.kind == frontend::TypeDescriptor::Kind::Int ||
         type.kind == frontend::TypeDescriptor::Kind::Enum;
}

auto Lowerer::IsBoolean(const frontend::TypeDescriptor& type) -> bool {
  return type.kind == frontend::TypeDescriptor::Kind::Bool;
}

auto Lowerer::FindField(const frontend::ClassDescriptor& class_info, std::string_view name)
    -> std::optional<FieldId> {
  const auto field = std::ranges::find(class_info.field_order, name);
  if (field == class_info.field_order.end()) {
    return std::nullopt;
  }
  return static_cast<FieldId>(field - class_info.field_order.begin());
}

void Lowerer::Emit(Instruction instruction) {
  if (current_function_ != nullptr) {
    current_function_->instructions.push_back(instruction);
  }
}

void Lowerer::PushScope() {
  scopes_.emplace_back();
}

void Lowerer::PopScope() {
  scopes_.pop_back();
}

auto Lowerer::DeclareLocal(std::string_view name,
                           RegisterId reg,
                           frontend::TypeDescriptor type,
                           frontend::SourceLocation location) -> bool {
  auto& scope = scopes_.back();
  if (!scope.try_emplace(name, Local{.reg = reg, .type = std::move(type)}).second) {
    AddDiagnostic(location, "duplicate local during lowering");
    return false;
  }
  return true;
}

void Lowerer::LowerVariableDeclaration(const frontend::Statement& statement) {
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
    Emit({
        .opcode = Opcode::Move,
        .dst = local,
        .lhs = initializer.reg,
        .location = statement.location,
    });
  }
  DeclareLocal(statement.name, local, type, statement.location);
}

void Lowerer::LowerIf(const frontend::Statement& statement) {
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
    current_function_->instructions.at(false_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    LowerStatement(*statement.else_branch);
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  } else {
    current_function_->instructions.at(false_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
}

void Lowerer::LowerWhile(const frontend::Statement& statement) {
  const std::size_t loop_start = current_function_->instructions.size();
  const LoweredValue condition = LowerExpression(*statement.condition);
  if (!IsBoolean(condition.type)) {
    AddDiagnostic(statement.condition->location, "while condition must be bool");
  }
  const std::size_t exit_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
  loops_.push_back({});
  LowerStatement(*statement.then_branch);
  for (const std::size_t jump : loops_.back().continue_jumps) {
    current_function_->instructions.at(jump).index = static_cast<std::uint32_t>(loop_start);
  }
  Emit({
      .opcode = Opcode::Jump,
      .index = static_cast<std::uint32_t>(loop_start),
      .location = statement.location,
  });
  current_function_->instructions.at(exit_jump).index =
      static_cast<std::uint32_t>(current_function_->instructions.size());
  for (const std::size_t jump : loops_.back().break_jumps) {
    current_function_->instructions.at(jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  loops_.pop_back();
}

void Lowerer::LowerFor(const frontend::Statement& statement) {
  PushScope();
  if (statement.initializer) {
    LowerStatement(*statement.initializer);
  }
  const std::size_t loop_start = current_function_->instructions.size();
  std::size_t exit_jump = current_function_->instructions.size();
  if (statement.condition) {
    const LoweredValue condition = LowerExpression(*statement.condition);
    exit_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::JumpIfFalse, .lhs = condition.reg, .location = statement.location});
  }
  loops_.push_back({});
  LowerStatement(*statement.then_branch);
  const std::size_t increment_target = current_function_->instructions.size();
  for (const std::size_t jump : loops_.back().continue_jumps) {
    current_function_->instructions.at(jump).index = static_cast<std::uint32_t>(increment_target);
  }
  if (statement.increment) {
    static_cast<void>(LowerExpression(*statement.increment));
  }
  Emit({
      .opcode = Opcode::Jump,
      .index = static_cast<std::uint32_t>(loop_start),
      .location = statement.location,
  });
  const std::size_t loop_end = current_function_->instructions.size();
  if (statement.condition) {
    current_function_->instructions.at(exit_jump).index = static_cast<std::uint32_t>(loop_end);
  }
  for (const std::size_t jump : loops_.back().break_jumps) {
    current_function_->instructions.at(jump).index = static_cast<std::uint32_t>(loop_end);
  }
  loops_.pop_back();
  PopScope();
}

auto Lowerer::LowerIntegerLiteral(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  std::int64_t value = 0;
  const auto result =
      std::from_chars(expression.text.data(), std::to_address(expression.text.end()), value);
  if (result.ec != std::errc()) {
    AddDiagnostic(expression.location, "integer literal is out of range");
    return {.reg = 0, .type = InvalidType()};
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = reg,
      .immediate = value,
      .location = expression.location,
  });
  return {.reg = reg, .type = {.kind = frontend::TypeDescriptor::Kind::Int, .name = "int"}};
}

auto Lowerer::LowerStringLiteral(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  std::string value = expression.text;
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  const auto found = std::ranges::find(program_.strings, value);
  const std::uint32_t index = found == program_.strings.end()
                                  ? static_cast<std::uint32_t>(program_.strings.size())
                                  : static_cast<std::uint32_t>(found - program_.strings.begin());
  if (found == program_.strings.end()) {
    program_.strings.push_back(std::move(value));
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({.opcode = Opcode::LoadString, .dst = reg, .index = index, .location = expression.location});
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = "string"},
  };
}

auto Lowerer::LowerArrayLiteral(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const RegisterId array = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewArray,
      .dst = array,
      .index = static_cast<std::uint32_t>(expression.elements.size()),
      .location = expression.location,
  });
  for (std::size_t index = 0; index < expression.elements.size(); ++index) {
    const LoweredValue element = LowerExpression(*expression.elements.at(index));
    const RegisterId index_reg = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = index_reg,
        .immediate = static_cast<std::int64_t>(index),
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = array,
        .lhs = index_reg,
        .rhs = element.reg,
        .location = expression.location,
    });
  }
  return {
      .reg = array,
      .type = {.kind = frontend::TypeDescriptor::Kind::Array, .name = "array"},
  };
}

auto Lowerer::LowerShorthandMember(const frontend::Expression& expression,
                                   const frontend::TypeDescriptor* expected)
    -> Lowerer::LoweredValue {
  if (expected == nullptr || expected->kind != frontend::TypeDescriptor::Kind::Enum) {
    AddDiagnostic(expression.location, "enum shorthand has no expected enum type");
    return {.reg = 0, .type = InvalidType()};
  }
  const auto members = model_.enum_members.find(expected->name);
  if (members == model_.enum_members.end()) {
    return {.reg = 0, .type = InvalidType()};
  }
  std::uint32_t ordinal = 0;
  bool found = false;
  for (const frontend::Enum& enumeration : source_.enums) {
    if (enumeration.name != expected->name) {
      continue;
    }
    for (std::size_t index = 0; index < enumeration.members.size(); ++index) {
      if (enumeration.members.at(index) == expression.text) {
        ordinal = static_cast<std::uint32_t>(index);
        found = true;
        break;
      }
    }
  }
  if (!found) {
    AddDiagnostic(expression.location, "unknown enum member during lowering");
    return {.reg = 0, .type = InvalidType()};
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = reg,
      .immediate = ordinal,
      .location = expression.location,
  });
  return {.reg = reg, .type = *expected};
}

auto Lowerer::LowerUnary(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const LoweredValue operand = LowerExpression(*expression.operand);
  const RegisterId result = AllocateTemporary(expression.location);
  if (expression.text == "-" && IsInteger(operand.type)) {
    Emit({
        .opcode = Opcode::NegInt,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  if (expression.text == "!" && IsBoolean(operand.type)) {
    Emit({
        .opcode = Opcode::NotBool,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
  AddDiagnostic(expression.location, "unsupported unary operation in bootstrap VM");
  return {.reg = result, .type = InvalidType()};
}

auto Lowerer::LowerBinary(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  if (expression.text == "=") {
    return LowerAssignment(expression);
  }
  if (expression.text == "&&" || expression.text == "||") {
    return LowerLogical(expression);
  }
  const LoweredValue left = LowerExpression(*expression.left);
  const LoweredValue right = LowerExpression(*expression.right, &left.type);
  const RegisterId result = AllocateTemporary(expression.location);
  Opcode opcode = Opcode::AddInt;
  frontend::TypeDescriptor result_type = left.type;
  const auto selected = SelectBinaryOpcode(expression.text, left.type);
  if (!selected) {
    AddDiagnostic(expression.location, "unsupported binary operation in bootstrap VM");
    return {.reg = result, .type = InvalidType()};
  }
  opcode = *selected;
  const bool comparison = expression.text == "==" || expression.text == "!=" ||
                          expression.text == "<" || expression.text == "<=" ||
                          expression.text == ">" || expression.text == ">=";
  if (comparison) {
    result_type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = "bool"};
  }
  const bool comparable_booleans = IsBoolean(left.type) && IsBoolean(right.type) &&
                                   (expression.text == "==" || expression.text == "!=");
  const bool comparable_strings = left.type.kind == frontend::TypeDescriptor::Kind::String &&
                                  right.type.kind == frontend::TypeDescriptor::Kind::String &&
                                  (expression.text == "==" || expression.text == "!=");
  const bool logical = expression.text == "&&" || expression.text == "||";
  const bool string_concat = expression.text == "+" &&
                             left.type.kind == frontend::TypeDescriptor::Kind::String &&
                             right.type.kind == frontend::TypeDescriptor::Kind::String;
  if ((!IsInteger(left.type) || !IsInteger(right.type)) && !comparable_booleans &&
      !comparable_strings && !logical && !string_concat) {
    AddDiagnostic(expression.location, "bootstrap VM arithmetic requires int operands");
    return {.reg = result, .type = InvalidType()};
  }
  Emit({
      .opcode = opcode,
      .dst = result,
      .lhs = left.reg,
      .rhs = right.reg,
      .location = expression.location,
  });
  return {.reg = result, .type = result_type};
}

auto Lowerer::LowerLogical(const frontend::Expression& expression) -> LoweredValue {
  const LoweredValue left = LowerExpression(*expression.left);
  const RegisterId result = AllocateTemporary(expression.location);
  const std::size_t conditional_jump = current_function_->instructions.size();
  Emit({.opcode = Opcode::JumpIfFalse, .lhs = left.reg, .location = expression.location});
  if (expression.text == "&&") {
    const LoweredValue right = LowerExpression(*expression.right);
    Emit({
        .opcode = Opcode::Move,
        .dst = result,
        .lhs = right.reg,
        .location = expression.location,
    });
    const std::size_t end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(conditional_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    const RegisterId false_value = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = false_value,
        .immediate = 0,
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::Move,
        .dst = result,
        .lhs = false_value,
        .location = expression.location,
    });
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  } else {
    const RegisterId true_value = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::LoadBool,
        .dst = true_value,
        .immediate = 1,
        .location = expression.location,
    });
    Emit({
        .opcode = Opcode::Move,
        .dst = result,
        .lhs = true_value,
        .location = expression.location,
    });
    const std::size_t end_jump = current_function_->instructions.size();
    Emit({.opcode = Opcode::Jump, .location = expression.location});
    current_function_->instructions.at(conditional_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
    const LoweredValue right = LowerExpression(*expression.right);
    Emit({
        .opcode = Opcode::Move,
        .dst = result,
        .lhs = right.reg,
        .location = expression.location,
    });
    current_function_->instructions.at(end_jump).index =
        static_cast<std::uint32_t>(current_function_->instructions.size());
  }
  return {
      .reg = result,
      .type = {.kind = frontend::TypeDescriptor::Kind::Bool, .name = "bool"},
  };
}

}  // namespace vela::ir
