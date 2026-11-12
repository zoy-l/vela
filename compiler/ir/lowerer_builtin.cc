#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "base/builtin.h"
#include "base/reflection_operation.h"
#include "base/regexp.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/ir/declaration_bindings.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

void Lowerer::LowerIntrinsicFunction(Function function,
                                     const frontend::Function& source,
                                     const frontend::Class* owner,
                                     Builtin builtin) {
  BeginFunctionBody(function, owner);
  auto result = function.parameter_count;
  RegisterId argument_start = 0;
  const auto is_type_info = builtin == Builtin::ReflectionTypeInfo;
  const auto is_find_attributed =
      source.intrinsic_operation &&
      *source.intrinsic_operation ==
          static_cast<std::uint32_t>(reflection::Operation::FindAttributedMethods);
  const auto* reflection_signature = is_type_info || is_find_attributed
                                         ? &model_.classes.at(owner->name).methods.at(source.name)
                                         : nullptr;
  if (is_type_info) {
    const auto& type = reflection_signature->type_arguments.front();
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = result++,
        .immediate = DescribeReflectionArgument(type),
        .location = source.location,
    });
  }
  if (source.intrinsic_operation) {
    argument_start = result;
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = result++,
        .immediate = *source.intrinsic_operation,
        .location = source.location,
    });
    if (is_find_attributed) {
      const auto& signature = *reflection_signature;
      for (const auto type : {signature.type_arguments.front(), signature.return_type}) {
        Emit({
            .opcode = Opcode::LoadInt,
            .dst = result++,
            .immediate = RegisterRuntimeType(type),
            .location = source.location,
        });
      }
    }
    for (RegisterId parameter = 0; parameter < function.parameter_count; ++parameter) {
      Emit({
          .opcode = Opcode::Move,
          .dst = result++,
          .lhs = parameter,
          .location = source.location,
      });
    }
    while (result - argument_start < BuiltinArity(builtin)) {
      Emit({.opcode = Opcode::LoadNull, .dst = result++, .location = source.location});
    }
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
