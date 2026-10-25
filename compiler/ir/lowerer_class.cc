#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

std::uint32_t Lowerer::RegisterRuntimeType(const frontend::TypeDescriptor& type) {
  auto found = runtime_type_ids_.find(type.name);
  std::uint32_t index = 0;
  if (found == runtime_type_ids_.end()) {
    index = static_cast<std::uint32_t>(program_.runtime_types.size());
    program_.runtime_types.push_back(type);
    runtime_type_ids_.emplace(type.name, index);
    if (type.kind == frontend::TypeDescriptor::Kind::Array) {
      static_cast<void>(RegisterArrayType(type));
    }
    if (type.kind == frontend::TypeDescriptor::Kind::Expected) {
      static_cast<void>(RegisterExpectedType(type));
    }
    if (type.kind == frontend::TypeDescriptor::Kind::Set ||
        type.kind == frontend::TypeDescriptor::Kind::Map) {
      static_cast<void>(RegisterCollectionType(type));
    }
  } else {
    index = found->second;
  }
  return index;
}

Lowerer::LoweredValue Lowerer::LowerTypeOperation(const frontend::Expression& expression) {
  const auto index = RegisterRuntimeType(model_.type_operands.at(&expression));
  const auto value = LowerExpression(*expression.operand);
  const auto result = AllocateTemporary(expression.location);
  Emit({
      .opcode = expression.kind == frontend::Expression::Kind::TypeTest ? Opcode::IsType
                                                                        : Opcode::CheckedCast,
      .dst = result,
      .lhs = value.reg,
      .index = index,
      .location = expression.location,
  });
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

void Lowerer::LowerInitializer(std::string name,
                               const frontend::Expression& initializer,
                               const frontend::TypeDescriptor& type,
                               frontend::SourceLocation location) {
  Function function;
  function.name = std::move(name);
  function.return_type = type;
  current_function_ = &function;
  binding_registers_.clear();
  next_register_ = 0;
  scopes_.clear();
  controls_.clear();
  finalizers_.clear();
  current_handler_.reset();
  double_constant_ids_.clear();
  const auto value = LowerExpression(initializer);
  Emit({.opcode = Opcode::Return, .dst = value.reg, .location = location});
  function.register_count = next_register_;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

Lowerer::LoweredValue Lowerer::LowerConstruction(const frontend::Expression& expression,
                                                 const frontend::TypeDescriptor& expected) {
  const auto& info = model_.classes.at(expected.name);
  const auto& source = *info.declaration;
  const auto& signature = info.constructor;
  const bool generated = source.constructor_kind == frontend::ConstructorKind::Generated;
  std::vector<std::optional<RegisterId>> arguments(generated ? 0 : signature.parameters.size());
  std::vector<bool> provided(info.field_order.size(), false);
  const auto& order = model_.argument_order.at(&expression);
  const auto object = AllocateTemporary(expression.location);
  Emit({
      .opcode = Opcode::NewInstance,
      .dst = object,
      .index = class_ids_.at(expected.name),
      .location = expression.location,
  });
  // param 实参依源码顺序直接写入尚未逸出的对象，避免为全部字段保留寄存器。
  for (std::size_t index = 0; index < expression.arguments.size(); ++index) {
    const auto& argument = expression.arguments.at(index);
    const auto parameter = order.at(index);
    const auto value = LowerExpression(*argument.value);
    if (generated) {
      const auto field = info.fields.at(signature.parameters.at(parameter).name).index;
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = value.reg,
          .index = field,
          .location = argument.location,
      });
      provided.at(field) = true;
    } else {
      const auto saved = AllocateTemporary(argument.location);
      Emit({.opcode = Opcode::Move, .dst = saved, .lhs = value.reg, .location = argument.location});
      arguments.at(parameter) = saved;
    }
  }
  for (const auto& field : source.fields) {
    if (field.storage == frontend::MemberStorage::Static) {
      continue;
    }
    const auto field_index = info.fields.at(field.name).index;
    if (provided.at(field_index)) {
      continue;
    }
    std::optional<RegisterId> value;
    if (field.initializer) {
      // 字段初始化器在声明处检查，不可意外读取构造调用者的同名局部变量。
      auto caller_scopes = std::move(scopes_);
      scopes_.clear();
      value = LowerExpression(*field.initializer).reg;
      scopes_ = std::move(caller_scopes);
    }
    if (value) {
      Emit({
          .opcode = Opcode::SetField,
          .dst = object,
          .lhs = *value,
          .index = field_index,
          .location = field.location,
      });
    }
  }
  if (source.constructor) {
    const auto count =
        source.constructor_kind == frontend::ConstructorKind::Explicit ? arguments.size() : 0;
    const auto base = next_register_;
    for (std::size_t index = 0; index <= count; ++index) {
      static_cast<void>(AllocateRegister(expression.location));
    }
    Emit({.opcode = Opcode::Move, .dst = base, .lhs = object, .location = expression.location});
    for (std::size_t index = 0; index < count; ++index) {
      Emit({
          .opcode = Opcode::Move,
          .dst = static_cast<RegisterId>(base + index + 1),
          .lhs = *arguments.at(index),
          .location = expression.location,
      });
    }
    const auto ignored = AllocateTemporary(expression.location);
    Emit({
        .opcode = Opcode::Call,
        .dst = ignored,
        .lhs = base,
        .index = function_ids_.at(expected.name + ".$constructor"),
        .location = expression.location,
    });
  }
  return {.reg = object, .type = expected};
}

}  // namespace vela::ir
