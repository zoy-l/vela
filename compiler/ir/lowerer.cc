#include "compiler/ir/lowerer.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "base/callable.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/standard_modules.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

auto InvalidType() -> frontend::TypeDescriptor {
  return {};
}

auto HasAssignments(const frontend::Expression& expression) -> bool {
  if (expression.kind == frontend::Expression::Kind::Binary && expression.text == "=") {
    return true;
  }
  for (const auto* child : {
           expression.left.get(),
           expression.right.get(),
           expression.operand.get(),
           expression.callee.get(),
           expression.object.get(),
           expression.index.get(),
       }) {
    if (child != nullptr && HasAssignments(*child)) {
      return true;
    }
  }
  return std::ranges::any_of(expression.elements,
                             [](const auto& child) -> bool { return HasAssignments(*child); }) ||
         std::ranges::any_of(expression.arms,
                             [](const auto& arm) -> bool { return HasAssignments(*arm.value); }) ||
         std::ranges::any_of(expression.arguments, [](const auto& argument) -> bool {
           return HasAssignments(*argument.value);
         });
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
  array_type_ids_.clear();
  callable_classes_.clear();
  global_ids_.clear();
  interface_method_ids_.clear();
  runtime_type_ids_.clear();
  collection_type_ids_.clear();
  string_constant_ids_.clear();
  BuildFunctionTable();
  BuildClassTable();
  if (!model_.captured_mutables.empty()) {
    class_ids_.emplace("$Cell", static_cast<ClassId>(program_.classes.size()));
    program_.classes.push_back({
        .name = "$Cell",
        .field_order = {"value"},
        .field_types = {{.kind = frontend::TypeDescriptor::Kind::Class, .name = "$Cell"}},
        .callable = std::nullopt,
        .interfaces = {},
        .dispatch_targets = {},
    });
  }
  for (const frontend::Function& function : source_.functions) {
    if (function.role != frontend::Function::Role::RetiredEntry) {
      LowerFunction(function);
    }
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      LowerFunction(method, &owner);
    }
    if (owner.constructor) {
      LowerFunction(*owner.constructor, &owner);
    }
  }
  for (const auto& owner : source_.classes) {
    for (const auto& field : owner.fields) {
      if (field.storage == frontend::MemberStorage::Static) {
        LowerInitializer("$static." + owner.name + "." + field.name, *field.initializer,
                         model_.classes.at(owner.name).static_fields.at(field.name).type,
                         field.location);
      }
    }
  }
  for (const auto& global : source_.globals) {
    LowerInitializer("$global." + global.name, *global.initializer,
                     model_.globals.at(global.name).type, global.location);
  }
  for (const auto& lambda : model_.lambdas) {
    LowerFunction(*lambda.expression->function, nullptr, &lambda);
  }
  return {.program = std::move(program_), .diagnostics = std::move(diagnostics_)};
}

void Lowerer::AddDiagnostic(frontend::SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path =
          source_.sources.empty() ? std::string() : source_.sources.at(location.source_id).path,
  });
}

void Lowerer::BuildInterfaceSelectors() {
  if (persistent_ != nullptr) {
    // 布局一旦用于实例就不能更换；首次接纳类型时必须绑定全部接口方法。
    for (const auto& interface : source_.interfaces) {
      for (const auto& method : interface.methods) {
        const auto key = interface.name + "." + method.name;
        interface_method_ids_.emplace(key, persistent_->selectors.Get(key));
      }
    }
    return;
  }
  // 只为实际接口调用和绑定的方法生成 selector，未使用的接口不扩张分派表。
  for (const auto& [expression, member] : model_.interface_members) {
    interface_method_ids_.try_emplace(member.owner + "." + member.member, 0);
  }
  std::uint32_t selector = 0;
  for (const auto& interface : source_.interfaces) {
    for (const auto& method : interface.methods) {
      const auto selected = interface_method_ids_.find(interface.name + "." + method.name);
      if (selected != interface_method_ids_.end()) {
        selected->second = selector++;
      }
    }
  }
}

void Lowerer::BuildClassTable() {
  BuildInterfaceSelectors();
  for (const frontend::Class& source_class : source_.classes) {
    const auto class_info = model_.classes.find(source_class.name);
    if (class_info == model_.classes.end()) {
      continue;
    }
    const auto class_id = static_cast<ClassId>(program_.classes.size());
    class_ids_.emplace(source_class.name, class_id);

    Class output_class;
    output_class.name = source_class.name;
    if (!source_.sources.empty() &&
        source_.sources.at(source_class.location.source_id).path == frontend::kErrorModuleName) {
      output_class.kind = Class::Kind::RuntimeError;
    }
    if (!class_info->second.interfaces.empty()) {
      output_class.dispatch_targets.resize(interface_method_ids_.size(), kInvalidFunctionTarget);
    }
    for (const auto& name : class_info->second.interfaces) {
      const auto& interface = model_.interfaces.at(name);
      output_class.interfaces.push_back(interface.index);
      for (const auto& [method_name, signature] : interface.methods) {
        auto qualified_method = name;
        qualified_method += '.';
        qualified_method += method_name;
        const auto selected = interface_method_ids_.find(qualified_method);
        if (selected != interface_method_ids_.end()) {
          output_class.dispatch_targets.at(selected->second) =
              function_ids_.at(source_class.name + "." + method_name);
        }
      }
    }
    output_class.field_order = class_info->second.field_order;
    output_class.field_types.reserve(output_class.field_order.size());
    for (const std::string& field_name : output_class.field_order) {
      const auto field = class_info->second.fields.find(field_name);
      output_class.field_types.push_back(
          field == class_info->second.fields.end() ? InvalidType() : field->second.type);
    }
    program_.classes.push_back(std::move(output_class));
  }
}

auto Lowerer::AddFunctionId(const std::string& name, frontend::Function::Role role)
    -> std::uint32_t {
  const auto& identity = role == frontend::Function::Role::InteractiveEntry ? "$entry" : name;
  const auto target = persistent_ == nullptr ? static_cast<std::uint32_t>(function_ids_.size())
                                             : persistent_->functions.Get(identity);
  function_ids_.emplace(name, target);
  return target;
}

void Lowerer::AddGlobalId(const std::string& name, GlobalKind kind) {
  const auto slot = persistent_ == nullptr
                        ? static_cast<std::uint32_t>(program_.global_initializers.size())
                        : persistent_->globals.Get(name);
  global_ids_.emplace(name, slot);
  const auto initializer = (kind == GlobalKind::StaticField ? "$static." : "$global.") + name;
  const auto function = AddFunctionId(initializer);
  if (program_.global_initializers.size() <= slot) {
    program_.global_initializers.resize(static_cast<std::size_t>(slot) + 1);
  }
  program_.global_initializers.at(slot) = function;
}

void Lowerer::BuildFunctionTable() {
  for (const auto& function : source_.functions) {
    if (function.role != frontend::Function::Role::RetiredEntry) {
      static_cast<void>(AddFunctionId(function.name, function.role));
    }
  }
  for (const frontend::Class& owner : source_.classes) {
    for (const frontend::Function& method : owner.methods) {
      const auto name = owner.name + "." + method.name;
      static_cast<void>(AddFunctionId(name));
    }
    if (owner.constructor) {
      const auto name = owner.name + ".$constructor";
      static_cast<void>(AddFunctionId(name));
    }
  }
  for (const auto& owner : source_.classes) {
    for (const auto& field : owner.fields) {
      if (field.storage == frontend::MemberStorage::Static) {
        const auto key = owner.name + "." + field.name;
        AddGlobalId(key, GlobalKind::StaticField);
      }
    }
  }
  for (const auto& global : source_.globals) {
    AddGlobalId(global.name, GlobalKind::Module);
  }
  lambda_ids_.clear();
  for (const auto& lambda : model_.lambdas) {
    const auto target = persistent_ == nullptr
                            ? static_cast<std::uint32_t>(function_ids_.size())
                            : persistent_->functions.Get(lambda.expression->function->name);
    lambda_ids_.push_back(target);
    function_ids_.emplace("$lambda" + std::to_string(target), target);
  }
  program_.functions.resize(persistent_ == nullptr ? function_ids_.size()
                                                   : persistent_->functions.size());
}

void Lowerer::StoreFunction(Function function) {
  const auto target = function_ids_.at(function.name);
  program_.functions.at(target) = std::move(function);
}

void Lowerer::LowerFunction(const frontend::Function& source,
                            const frontend::Class* owner,
                            const frontend::LambdaDescriptor* lambda) {
  Function function;
  function.name = owner == nullptr ? source.name : owner->name + "." + source.name;
  function.owner_class = owner == nullptr ? std::string() : owner->name;
  const frontend::FunctionDescriptor* function_info = nullptr;
  if (lambda != nullptr) {
    function.name =
        "$lambda" + std::to_string(lambda_ids_.at(model_.lambda_indices.at(lambda->expression)));
    function_info = &lambda->signature;
  } else if (owner == nullptr) {
    function_info = &model_.functions.at(source.name);
  } else {
    function_info = &model_.classes.at(owner->name).methods.at(source.name);
  }
  function.return_type = function_info->return_type;
  const bool has_receiver = owner != nullptr && source.storage == frontend::MemberStorage::Instance;
  function.parameter_count = static_cast<std::uint32_t>(
      source.parameters.size() + ((has_receiver || lambda != nullptr) ? 1 : 0));
  if (!source.native_symbol.empty()) {
    function.native_symbol = source.native_symbol;
    function.register_count = function.parameter_count + 1;
    function.instructions = {
        {
            .opcode = Opcode::NativeCall,
            .dst = function.parameter_count,
            .immediate = function.parameter_count,
            .location = source.location,
        },
        {.opcode = Opcode::Return, .dst = function.parameter_count, .location = source.location},
    };
    StoreFunction(std::move(function));
    return;
  }
  current_function_ = &function;
  binding_registers_.clear();
  next_register_ = 0;
  double_constant_ids_.clear();
  scopes_.clear();
  controls_.clear();
  finalizers_.clear();
  current_handler_.reset();
  PushScope();

  if (lambda != nullptr) {
    static_cast<void>(AllocateRegister(source.location));
  }

  if (has_receiver) {
    const RegisterId this_reg = AllocateRegister(source.location);
    DeclareLocal("this", this_reg, model_.classes.at(owner->name).type, source.location, &source);
  }

  for (const frontend::Parameter& parameter : source.parameters) {
    const auto type =
        function_info->parameters.at(function_info->parameter_indices.at(parameter.name)).type;
    const RegisterId reg = AllocateRegister(parameter.location);
    DeclareLocal(parameter.name, reg, type, parameter.location);
  }
  // 完整参数窗口建立后，再为可捕获的参数分配共享槽位。
  for (const auto& parameter : source.parameters) {
    const auto reg = scopes_.back().at(parameter.name).reg;
    scopes_.back().erase(parameter.name);
    DeclareLocal(
        parameter.name, reg,
        function_info->parameters.at(function_info->parameter_indices.at(parameter.name)).type,
        parameter.location, &parameter);
  }
  if (lambda != nullptr) {
    for (std::size_t index = 0; index < lambda->captures.size(); ++index) {
      const auto& capture = lambda->captures.at(index);
      const auto reg = AllocateTemporary(source.location);
      Emit({
          .opcode = Opcode::GetField,
          .dst = reg,
          .lhs = 0,
          .index = static_cast<FieldId>(index),
          .location = source.location,
      });
      scopes_.back().emplace(capture.name,
                             Local{
                                 .reg = reg,
                                 .type = capture.type,
                                 .cell = capture.kind == frontend::BindingKind::Mutable,
                             });
      binding_registers_.emplace(capture.binding, reg);
    }
  }

  if (source.body != nullptr) {
    for (const auto& child : source.body->statements) {
      LowerStatement(*child);
    }
  }
  PopScope();

  if (function.return_type.kind == frontend::TypeDescriptor::Kind::Void &&
      model_.control_flow.at(source.body.get()).can_fall_through) {
    const RegisterId result = AllocateTemporary(source.location);
    Emit({.opcode = Opcode::LoadNull, .dst = result, .location = source.location});
    Emit({.opcode = Opcode::Return, .dst = result, .location = source.location});
  }
  function.register_count = next_register_;

  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

void Lowerer::LowerStatement(const frontend::Statement& statement) {
  switch (statement.kind) {
    case frontend::Statement::Kind::VariableDeclaration:
      LowerVariableDeclaration(statement);
      break;
    case frontend::Statement::Kind::Return:
      LowerReturn(statement);
      break;
    case frontend::Statement::Kind::Throw: {
      const auto value = LowerExpression(*statement.expression);
      Emit({.opcode = Opcode::Throw, .lhs = value.reg, .location = statement.location});
      break;
    }
    case frontend::Statement::Kind::Try:
      LowerTry(statement);
      break;
    case frontend::Statement::Kind::Expression:
      if (statement.expression != nullptr) {
        const auto value = LowerExpression(*statement.expression);
        if (statement.echo && value.type.kind != frontend::TypeDescriptor::Kind::Void) {
          Emit({.opcode = Opcode::Print, .lhs = value.reg, .location = statement.location});
        }
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
    case frontend::Statement::Kind::Switch:
      LowerSwitch(statement);
      break;
    case frontend::Statement::Kind::Break:
    case frontend::Statement::Kind::Continue:
      LowerControlExit(statement);
      break;
  }
}

void Lowerer::LowerBlock(const frontend::Statement& statement) {
  PushScope();
  for (const std::unique_ptr<frontend::Statement>& child : statement.statements) {
    LowerStatement(*child);
  }
  PopScope();
}

auto Lowerer::LowerExpression(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  if (model_.collection_constructions.contains(&expression)) {
    return LowerCollectionConstruction(expression);
  }
  if (model_.expected_calls.contains(&expression)) {
    return LowerExpected(expression);
  }
  if (model_.builtin_calls.contains(&expression)) {
    return LowerBuiltin(expression);
  }
  switch (expression.kind) {
    case frontend::Expression::Kind::NullLiteral: {
      const auto reg = AllocateTemporary(expression.location);
      Emit({.opcode = Opcode::LoadNull, .dst = reg, .location = expression.location});
      return {.reg = reg, .type = {.kind = frontend::TypeDescriptor::Kind::Null, .name = "null"}};
    }
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
    case frontend::Expression::Kind::DoubleLiteral:
      return LowerDoubleLiteral(expression);
    case frontend::Expression::Kind::StringLiteral:
      return LowerStringLiteral(expression);
    case frontend::Expression::Kind::StringInterpolation:
      return LowerStringInterpolation(expression);
    case frontend::Expression::Kind::ArrayLiteral:
      return LowerArrayLiteral(expression);
    case frontend::Expression::Kind::Index:
      return LowerIndex(expression);
    case frontend::Expression::Kind::Name: {
      const Local* local = FindLocal(expression.text);
      if (local == nullptr && model_.globals.contains(expression.text)) {
        const auto result = AllocateTemporary(expression.location);
        Emit({
            .opcode = Opcode::GetGlobal,
            .dst = result,
            .index = global_ids_.at(expression.text),
            .location = expression.location,
        });
        return {.reg = result, .type = model_.globals.at(expression.text).type};
      }
      if (local == nullptr) {
        return LowerFunctionValue(expression);
      }
      auto value = ReadLocal(*local, expression.location);
      value.type = model_.expression_types.at(&expression);
      return value;
    }
    case frontend::Expression::Kind::Lambda:
      return LowerFunctionValue(expression);
    case frontend::Expression::Kind::TypeTest:
    case frontend::Expression::Kind::TypeCast:
      return LowerTypeOperation(expression);
    case frontend::Expression::Kind::Match:
      return LowerMatch(expression);
    case frontend::Expression::Kind::Unary:
      return LowerUnary(expression);
    case frontend::Expression::Kind::Binary:
      return LowerBinary(expression);
    case frontend::Expression::Kind::Call:
      return LowerCall(expression);
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
    if (local == nullptr && model_.globals.contains(left.text)) {
      const auto value = LowerExpression(right);
      Emit({
          .opcode = Opcode::SetGlobal,
          .lhs = value.reg,
          .index = global_ids_.at(left.text),
          .location = left.location,
      });
      return {.reg = value.reg, .type = model_.globals.at(left.text).type};
    }
    if (local == nullptr) {
      AddDiagnostic(left.location, "unknown local during assignment lowering");
      return {.reg = 0, .type = InvalidType()};
    }
    const LoweredValue value = LowerExpression(right);
    Emit({
        .opcode = local->cell ? Opcode::SetField : Opcode::Move,
        .dst = local->reg,
        .lhs = value.reg,
        .location = left.location,
    });
    return {.reg = local->cell ? value.reg : local->reg, .type = local->type};
  }
  if (left.kind == frontend::Expression::Kind::Member) {
    const auto member = model_.static_members.find(&left);
    if (member != model_.static_members.end()) {
      const auto value = LowerExpression(right);
      Emit({
          .opcode = Opcode::SetGlobal,
          .lhs = value.reg,
          .index = global_ids_.at(member->second.owner + "." + member->second.member),
          .location = left.location,
      });
      return value;
    }
    const LoweredValue object = SnapshotBefore(LowerExpression(*left.object), right);
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
    const frontend::TypeDescriptor field_type = class_info->fields.at(left.text).type;
    const LoweredValue value = LowerExpression(right);
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
    const auto array_before_index = SnapshotBefore(LowerExpression(*left.object), *left.index);
    const auto array = SnapshotBefore(array_before_index, right);
    const auto index = SnapshotBefore(LowerExpression(*left.index), right);
    const auto& element_type = model_.array_elements.at(array.type.name);
    const LoweredValue value = LowerExpression(right);
    Emit({
        .opcode = Opcode::SetIndex,
        .dst = array.reg,
        .lhs = index.reg,
        .rhs = value.reg,
        .location = left.location,
    });
    return {.reg = value.reg, .type = element_type};
  }
  AddDiagnostic(left.location, "left side of assignment is not lowerable");
  return {.reg = 0, .type = InvalidType()};
}

auto Lowerer::LowerMember(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  if (model_.interface_members.contains(&expression)) {
    return LowerFunctionValue(expression);
  }
  const auto static_member = model_.static_members.find(&expression);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    const auto& owner = model_.classes.at(member.owner);
    if (owner.methods.contains(member.member)) {
      return LowerFunctionValue(expression);
    }
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::GetGlobal,
        .dst = result,
        .index = global_ids_.at(member.owner + "." + member.member),
        .location = expression.location,
    });
    return {.reg = result, .type = model_.expression_types.at(&expression)};
  }
  if (model_.expression_types.at(&expression).kind == frontend::TypeDescriptor::Kind::Function) {
    const auto& object_type = model_.expression_types.at(expression.object.get());
    const auto* owner = FindClass(object_type.name);
    if (owner != nullptr && owner->methods.contains(expression.text)) {
      return LowerFunctionValue(expression);
    }
  }
  if (expression.object->kind == frontend::Expression::Kind::Name &&
      FindLocal(expression.object->text) == nullptr) {
    const auto type = model_.types.find(expression.object->text);
    if (type != model_.types.end() && type->second.kind == frontend::TypeDescriptor::Kind::Enum) {
      return LowerEnumMember(expression, type->second);
    }
  }
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
  return {.reg = result, .type = class_info->fields.at(expression.text).type};
}

auto Lowerer::LowerCall(const frontend::Expression& expression) -> LoweredValue {
  if (model_.interface_members.contains(&expression)) {
    return LowerInterfaceCall(expression);
  }
  const auto static_member = model_.static_members.find(&expression);
  if (static_member != model_.static_members.end()) {
    const auto& member = static_member->second;
    const auto& signature = model_.classes.at(member.owner).methods.at(member.member);
    return LowerInvocation(expression, signature,
                           function_ids_.at(member.owner + "." + member.member), nullptr);
  }
  const auto& callee = *expression.callee;
  const auto callee_type = model_.expression_types.find(&callee);
  if (callee_type != model_.expression_types.end() &&
      callee_type->second.kind == frontend::TypeDescriptor::Kind::Function) {
    return LowerIndirectCall(expression);
  }
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
    const auto alias = model_.types.find(callee.text);
    if (alias != model_.types.end() &&
        alias->second.kind == frontend::TypeDescriptor::Kind::Class) {
      return LowerConstruction(expression, alias->second);
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
  return {.reg = 0, .type = InvalidType()};
}

auto Lowerer::LowerInvocation(const frontend::Expression& expression,
                              const frontend::FunctionDescriptor& function,
                              std::uint32_t function_id,
                              const LoweredValue* receiver,
                              Opcode opcode) -> LoweredValue {
  const RegisterId base = next_register_;
  const std::size_t receiver_count = receiver == nullptr ? 0 : 1;
  const std::size_t count = receiver_count + function.parameters.size();
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
  const auto& order = model_.argument_order.at(&expression);
  for (std::size_t source_index = 0; source_index < expression.arguments.size(); ++source_index) {
    const auto& argument = expression.arguments.at(source_index);
    const auto index = order.at(source_index);
    const auto value = LowerExpression(*argument.value);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + receiver_count + index),
        .lhs = value.reg,
        .location = argument.location,
    });
  }
  const RegisterId result = AllocateTemporary(expression.location);
  Emit({
      .opcode = opcode,
      .dst = result,
      .lhs = base,
      .immediate = static_cast<std::int64_t>(count),
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

auto Lowerer::IsInteger(const frontend::TypeDescriptor& type) -> bool {
  return type.kind == frontend::TypeDescriptor::Kind::Int ||
         type.kind == frontend::TypeDescriptor::Kind::Enum;
}

auto Lowerer::IsBoolean(const frontend::TypeDescriptor& type) -> bool {
  return type.kind == frontend::TypeDescriptor::Kind::Bool;
}

auto Lowerer::FindField(const frontend::ClassDescriptor& class_info, std::string_view name)
    -> std::optional<FieldId> {
  const auto field = class_info.fields.find(name);
  if (field == class_info.fields.end()) {
    return std::nullopt;
  }
  return field->second.index;
}

void Lowerer::Emit(Instruction instruction) {
  if (current_function_ != nullptr) {
    instruction.handler = current_handler_;
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
                           frontend::SourceLocation location,
                           const void* declaration) -> bool {
  const auto binding = model_.bindings.find(declaration);
  const auto binding_id = binding == model_.bindings.end() ? 0 : binding->second;
  const bool cell =
      binding != model_.bindings.end() && model_.captured_mutables.contains(binding_id);
  if (cell) {
    const auto boxed = AllocateTemporary(location);
    Emit({
        .opcode = Opcode::NewInstance,
        .dst = boxed,
        .index = class_ids_.at("$Cell"),
        .location = location,
    });
    Emit({.opcode = Opcode::SetField, .dst = boxed, .lhs = reg, .location = location});
    reg = boxed;
  }
  auto& scope = scopes_.back();
  if (!scope.try_emplace(name, Local{.reg = reg, .type = std::move(type), .cell = cell}).second) {
    AddDiagnostic(location, "duplicate local during lowering");
    return false;
  }
  if (binding != model_.bindings.end()) {
    binding_registers_.insert_or_assign(binding_id, reg);
  }
  return true;
}

void Lowerer::LowerVariableDeclaration(const frontend::Statement& statement) {
  const RegisterId local = AllocateRegister(statement.location);
  const auto& type = model_.local_types.at(&statement);
  const auto initializer = LowerExpression(*statement.expression);
  Emit({
      .opcode = Opcode::Move,
      .dst = local,
      .lhs = initializer.reg,
      .location = statement.location,
  });
  DeclareLocal(statement.name, local, type, statement.location, &statement);
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
  const auto& value = expression.text;
  const auto [found, inserted] =
      string_constant_ids_.try_emplace(value, static_cast<std::uint32_t>(program_.strings.size()));
  const auto index = found->second;
  if (inserted) {
    program_.strings.push_back(value);
  }
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({.opcode = Opcode::LoadString, .dst = reg, .index = index, .location = expression.location});
  return {
      .reg = reg,
      .type = {.kind = frontend::TypeDescriptor::Kind::String, .name = "string"},
  };
}

auto Lowerer::LowerEnumMember(const frontend::Expression& expression,
                              const frontend::TypeDescriptor& type) -> Lowerer::LoweredValue {
  const auto ordinal = model_.enum_values.at(type.name + "." + expression.text);
  const RegisterId reg = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = reg,
      .immediate = ordinal,
      .location = expression.location,
  });
  return {.reg = reg, .type = type};
}

auto Lowerer::LowerUnary(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  const LoweredValue operand = LowerExpression(*expression.operand);
  const RegisterId result = AllocateTemporary(expression.location);
  if (expression.text == "-" && operand.type.kind == frontend::TypeDescriptor::Kind::Double) {
    Emit({
        .opcode = Opcode::NegDouble,
        .dst = result,
        .lhs = operand.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = operand.type};
  }
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

auto Lowerer::SnapshotBefore(LoweredValue value, const frontend::Expression& later)
    -> LoweredValue {
  if (HasAssignments(later)) {
    const auto snapshot = AllocateTemporary(later.location);
    Emit({.opcode = Opcode::Move, .dst = snapshot, .lhs = value.reg, .location = later.location});
    value.reg = snapshot;
  }
  return value;
}

auto Lowerer::LowerBinary(const frontend::Expression& expression) -> Lowerer::LoweredValue {
  if (expression.text == "=") {
    return LowerAssignment(expression);
  }
  if (expression.text == "&&" || expression.text == "||") {
    return LowerLogical(expression);
  }
  const auto left = SnapshotBefore(LowerExpression(*expression.left), *expression.right);
  const LoweredValue right = LowerExpression(*expression.right);
  using Kind = frontend::TypeDescriptor::Kind;
  if ((expression.text == "==" || expression.text == "!=") &&
      (left.type.nullable || right.type.nullable || left.type.kind == Kind::Null ||
       right.type.kind == Kind::Null || left.type.kind == Kind::Class ||
       left.type.kind == Kind::Interface || left.type.kind == Kind::Function)) {
    const auto result = AllocateTemporary(expression.location);
    Emit({
        .opcode = expression.text == "==" ? Opcode::EqualValue : Opcode::NotEqualValue,
        .dst = result,
        .lhs = left.reg,
        .rhs = right.reg,
        .location = expression.location,
    });
    return {.reg = result, .type = {.kind = Kind::Bool, .name = "bool"}};
  }
  if (left.type.kind == frontend::TypeDescriptor::Kind::Double) {
    return LowerDoubleBinary(expression, left, right);
  }
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
