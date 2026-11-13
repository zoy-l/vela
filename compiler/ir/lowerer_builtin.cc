#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/lowerer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/reflection.h"
#include "base/reflection_operation.h"
#include "base/regexp.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

bool IsRecordConstruction(const frontend::Expression& expression,
                          const frontend::SemanticModel& model,
                          std::string_view name) {
  if (expression.kind != frontend::Expression::Kind::Call) {
    return false;
  }
  const auto& callee = *expression.callee;
  if (callee.kind != frontend::Expression::Kind::Name || model.expression_types.contains(&callee)) {
    return false;
  }
  const auto type = model.types.find(callee.text);
  return type != model.types.end() && type->second.name == name;
}

std::optional<std::span<const std::unique_ptr<frontend::Expression>>> SnapshotItems(
    const frontend::Expression& expression,
    const frontend::SemanticModel& model) {
  if (!IsRecordConstruction(expression, model, reflection::kCallArgumentsName)) {
    return std::nullopt;
  }
  if (expression.arguments.empty()) {
    return std::span<const std::unique_ptr<frontend::Expression>>{};
  }
  const auto& items = *expression.arguments.front().value;
  if (items.kind != frontend::Expression::Kind::ArrayLiteral) {
    return std::nullopt;
  }
  for (const auto& item : items.elements) {
    if (!IsRecordConstruction(*item, model, reflection::kCallArgumentName)) {
      return std::nullopt;
    }
  }
  return items.elements;
}

}  // namespace

std::optional<Lowerer::LoweredValue> Lowerer::LowerReflectedInvocation(
    const frontend::Expression& expression,
    const frontend::FunctionDescriptor& function,
    const LoweredValue& descriptor) {
  const auto operation = function.intrinsic_operation;
  const auto instance = operation == static_cast<std::uint32_t>(reflection::Operation::Invoke);
  if (!instance && operation != static_cast<std::uint32_t>(reflection::Operation::InvokeStatic) &&
      operation != static_cast<std::uint32_t>(reflection::Operation::Construct)) {
    return std::nullopt;
  }
  const auto& order = model_.argument_order.at(&expression);
  const auto package_parameter = instance ? 1U : 0U;
  std::optional<std::span<const std::unique_ptr<frontend::Expression>>> items;
  for (std::size_t index = 0; index < order.size(); ++index) {
    if (order[index] == package_parameter) {
      items = SnapshotItems(*expression.arguments[index].value, model_);
    }
  }
  if (!items) {
    return std::nullopt;
  }
  // 标准记录的构造只写字段与常量默认值；立即消费的字面量图没有可观察身份。
  // 直接快照其值仍按源码求值顺序执行所有表达式，且不跨 await 保留初始化屏障。
  const auto location = expression.location;
  const auto count = kReflectionInvokeHeaderArity + (items->size() * 2);
  const auto base = AllocateRegisterWindow(count, location);
  Emit({.opcode = Opcode::LoadInt, .dst = base, .immediate = *operation, .location = location});
  Emit({.opcode = Opcode::Move, .dst = base + 1, .lhs = descriptor.reg, .location = location});
  if (!instance) {
    Emit({.opcode = Opcode::LoadNull, .dst = base + 2, .location = location});
  }
  for (std::size_t index = 0; index < order.size(); ++index) {
    const auto& argument = expression.arguments[index];
    if (order[index] != package_parameter) {
      const auto value = LowerExpression(*argument.value);
      Emit({
          .opcode = Opcode::Move,
          .dst = base + 2,
          .lhs = value.reg,
          .location = argument.location,
      });
      continue;
    }
    for (std::size_t item = 0; item < items->size(); ++item) {
      const auto& record = *(*items)[item];
      const auto slot = static_cast<RegisterId>(base + kReflectionInvokeHeaderArity + (item * 2));
      Emit({.opcode = Opcode::LoadNull, .dst = slot + 1, .location = record.location});
      const auto& fields = model_.argument_order.at(&record);
      for (std::size_t field = 0; field < fields.size(); ++field) {
        const auto& input = record.arguments[field];
        const auto value = LowerExpression(*input.value);
        Emit({
            .opcode = Opcode::Move,
            .dst = slot + fields[field],
            .lhs = value.reg,
            .location = input.location,
        });
      }
    }
  }
  const auto result = AllocateRegister(location);
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = result,
      .lhs = base,
      .immediate = static_cast<std::int64_t>(count),
      .index = static_cast<std::uint32_t>(Builtin::ReflectionInvoke),
      .location = location,
  });
  return LoweredValue{.reg = result, .type = function.return_type};
}

RegisterId Lowerer::EmitIntrinsicOperationArguments(Builtin builtin,
                                                    std::uint32_t operation,
                                                    std::span<const std::uint32_t> prefix,
                                                    RegisterId argument_start,
                                                    frontend::SourceLocation location,
                                                    std::size_t argument_count,
                                                    const frontend::TypeDescriptor& return_type) {
  const auto arity = BuiltinArity(builtin);
  const auto base = AllocateRegisterWindow(arity, location);
  auto next = base;
  Emit({.opcode = Opcode::LoadInt, .dst = next++, .immediate = operation, .location = location});
  for (const auto value : prefix) {
    Emit({.opcode = Opcode::LoadInt, .dst = next++, .immediate = value, .location = location});
  }
  for (std::size_t index = 0; index < argument_count; ++index) {
    Emit({
        .opcode = Opcode::Move,
        .dst = next++,
        .lhs = static_cast<RegisterId>(argument_start + index),
        .location = location,
    });
  }
  while (next < base + arity - 1) {
    Emit({.opcode = Opcode::LoadNull, .dst = next++, .location = location});
  }
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = next,
      .immediate = DescribeReflectionArgument(return_type),
      .location = location,
  });
  return base;
}

RegisterId Lowerer::EmitReflectionCall(const frontend::FunctionDescriptor& signature,
                                       RegisterId arguments,
                                       frontend::SourceLocation location,
                                       std::size_t count) {
  if (!signature.intrinsic) {
    std::unreachable();
  }
  const auto builtin = *signature.intrinsic;
  RegisterId base = 0;
  if (builtin == Builtin::ReflectionTypeInfo) {
    base = AllocateRegisterWindow(BuiltinArity(builtin), location);
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = base,
        .immediate = DescribeReflectionArgument(signature.type_arguments.front()),
        .location = location,
    });
  } else {
    if (!signature.intrinsic_operation) {
      std::unreachable();
    }
    const auto operation = *signature.intrinsic_operation;
    std::array<std::uint32_t, 2> prefix{};
    std::size_t prefix_count = 0;
    if (operation == static_cast<std::uint32_t>(reflection::Operation::FindAttributedMethods)) {
      prefix[prefix_count++] = RegisterRuntimeType(signature.type_arguments.front());
      prefix[prefix_count++] = RegisterRuntimeType(signature.return_type);
    }
    base =
        EmitIntrinsicOperationArguments(builtin, operation, std::span(prefix).first(prefix_count),
                                        arguments, location, count, signature.return_type);
  }
  if (builtin == Builtin::ReflectionTypeInfo) {
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = static_cast<RegisterId>(base + BuiltinArity(builtin) - 1),
        .immediate = DescribeReflectionArgument(signature.return_type),
        .location = location,
    });
  }
  const auto result = AllocateRegister(location);
  RecordValueType(result, signature.return_type);
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = result,
      .lhs = base,
      .index = static_cast<std::uint32_t>(builtin),
      .location = location,
  });
  return result;
}

void Lowerer::LowerIntrinsicFunction(Function function,
                                     const frontend::Function& source,
                                     const frontend::Class* owner,
                                     Builtin builtin) {
  BeginFunctionBody(function, owner);
  next_register_ = function.parameter_count;
  if (builtin == Builtin::Reflection || builtin == Builtin::ReflectionTypeInfo) {
    const auto& signature = model_.classes.at(owner->name).methods.at(source.name);
    const auto result = EmitReflectionCall(signature, 0, source.location, function.parameter_count);
    Emit({.opcode = Opcode::Return, .dst = result, .location = source.location});
    function.register_count = next_register_;
    StoreFunction(std::move(function));
    current_function_ = nullptr;
    return;
  }
  auto result = function.parameter_count;
  RegisterId argument_start = 0;
  if (source.intrinsic_operation) {
    argument_start = EmitIntrinsicOperationArguments(builtin, *source.intrinsic_operation, {}, 0,
                                                     source.location, function.parameter_count,
                                                     function.return_type);
    result = argument_start + static_cast<RegisterId>(BuiltinArity(builtin));
  }
  if (builtin == Builtin::JsonTypedParse || builtin == Builtin::JsonSafeParse) {
    const auto key = QualifiedMemberKey({.owner = owner->name, .name = source.name});
    const auto plan = model_.json_decode_entries.at(key);
    function.json_decode_plan = plan;
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = result,
        .immediate = plan,
        .location = source.location,
    });
    ++result;
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = result,
        .immediate = class_ids_.at(frontend::kJsonParseErrorTypeName),
        .location = source.location,
    });
    ++result;
    if (builtin == Builtin::JsonSafeParse) {
      Emit({
          .opcode = Opcode::LoadInt,
          .dst = result,
          .immediate = class_ids_.at(function.return_type.name),
          .location = source.location,
      });
      ++result;
    }
  }
  if (builtin == Builtin::DateToUTC) {
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = result,
        .immediate = class_ids_.at("DateParts"),
        .location = source.location,
    });
    ++result;
  }
  Emit({
      .opcode = Opcode::BuiltinCall,
      .dst = result,
      .lhs = argument_start,
      .index = static_cast<std::uint32_t>(builtin),
      .location = source.location,
  });
  Emit({.opcode = Opcode::Return, .dst = result, .location = source.location});
  function.register_count = result + 1;
  StoreFunction(std::move(function));
  current_function_ = nullptr;
}

Lowerer::LoweredValue Lowerer::LowerBuiltin(const frontend::Expression& expression) {
  const auto& result_type = model_.expression_types.at(&expression);
  const auto builtin = model_.builtin_calls.at(&expression);
  if (builtin == Builtin::ArrayUnshift || builtin == Builtin::ArraySplice ||
      (builtin == Builtin::ArrayPush && expression.arguments.size() != 1)) {
    return LowerArrayMutation(expression);
  }
  if (const auto count_builtin = SnapshotCountBuiltin(builtin)) {
    return LowerSnapshotArray(expression, *count_builtin);
  }
  const auto arity = BuiltinArity(builtin);
  const auto base = AllocateRegisterWindow(arity, expression.location);
  const auto kind = expression.kind;
  const bool indexed = kind == frontend::Expression::Kind::Index;
  const bool property = kind == frontend::Expression::Kind::Member;
  const bool method = kind == frontend::Expression::Kind::Call &&
                      expression.callee->kind == frontend::Expression::Kind::Member;
  const std::size_t receiver_count = indexed || property || method ? 1 : 0;
  std::optional<std::size_t> null_jump;
  if (receiver_count != 0) {
    const auto& object = method ? *expression.callee->object : *expression.object;
    const auto receiver = LowerExpression(object);
    Emit({.opcode = Opcode::Move, .dst = base, .lhs = receiver.reg, .location = object.location});
    if ((method && expression.callee->null_safe) || (!method && expression.null_safe)) {
      null_jump = BeginNullSafe(base, expression.location);
    }
  }
  LowerBuiltinDefaults(builtin, base, expression.location);
  if (indexed) {
    const auto index = LowerExpression(*expression.index);
    Emit({
        .opcode = Opcode::Move,
        .dst = static_cast<RegisterId>(base + 1),
        .lhs = index.reg,
        .location = expression.location,
    });
  } else {
    LowerArguments(expression, static_cast<RegisterId>(base + receiver_count));
  }
  EmitRegExpLayoutArgument(builtin, base, result_type, expression.location);
  const auto result = AllocateRegister(expression.location);
  EmitBuiltinCall(builtin, result, base, expression.location);
  FinishNullSafe(null_jump, result, expression.location);
  return {.reg = result, .type = result_type};
}

void Lowerer::EmitRegExpLayoutArgument(Builtin builtin,
                                       RegisterId base,
                                       frontend::TypeDescriptor type,
                                       frontend::SourceLocation location) {
  if (!HasRegExpArrayLayout(builtin)) {
    return;
  }
  type.name = frontend::NonNullableTypeName(type);
  type.nullable = false;
  Emit({
      .opcode = Opcode::LoadInt,
      .dst = static_cast<RegisterId>(base + BuiltinArity(builtin) - 1),
      .immediate = RegisterArrayType(type),
      .location = location,
  });
}

void Lowerer::LowerBuiltinDefaults(Builtin builtin,
                                   RegisterId base,
                                   frontend::SourceLocation location) {
  const auto arity = BuiltinArity(builtin);
  if (builtin == Builtin::NewRegExp) {
    for (std::size_t index = 1; index < arity; ++index) {
      Emit({
          .opcode = Opcode::LoadBool,
          .dst = static_cast<RegisterId>(base + index),
          .immediate = index == 2 ? 1 : 0,
          .location = location,
      });
    }
  }
  if (builtin == Builtin::RegExpAllMatches || builtin == Builtin::RegExpMatchAsPrefix) {
    Emit({.opcode = Opcode::LoadInt, .dst = base + 2, .location = location});
  }
  if (builtin == Builtin::ArrayJoin) {
    const auto value = EmitStringConstant(",", location);
    Emit({
        .opcode = Opcode::Move,
        .dst = base + 1,
        .lhs = value.reg,
        .location = location,
    });
  } else if (builtin == Builtin::ArraySlice || builtin == Builtin::ArrayFill ||
             builtin == Builtin::ByteArraySlice) {
    const auto start = base + (builtin == Builtin::ArrayFill ? 2 : 1);
    Emit({.opcode = Opcode::LoadInt, .dst = start, .location = location});
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = start + 1,
        .immediate = std::numeric_limits<std::int64_t>::max(),
        .location = location,
    });
  }
  if (builtin >= Builtin::ByteDataGetInt16 && builtin <= Builtin::ByteDataSetFloat64) {
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = static_cast<RegisterId>(base + arity - 1),
        .location = location,
    });
  }
}

}  // namespace vela::ir
