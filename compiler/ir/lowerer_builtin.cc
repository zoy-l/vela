#include "compiler/ir/lowerer.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "base/builtin.h"
#include "base/regexp.h"
#include "compiler/frontend/ast.h"
#include "compiler/ir/ir.h"

namespace vela::ir {

Lowerer::LoweredValue Lowerer::LowerBuiltin(const frontend::Expression& expression) {
  const auto builtin = model_.builtin_calls.at(&expression);
  if (builtin == Builtin::ArrayUnshift || builtin == Builtin::ArraySplice ||
      (builtin == Builtin::ArrayPush && expression.arguments.size() != 1)) {
    return LowerArrayMutation(expression);
  }
  if (builtin == Builtin::StringCodePoints || builtin == Builtin::StringGraphemes ||
      builtin == Builtin::CollectionKeys || builtin == Builtin::CollectionValues) {
    return LowerSnapshotArray(expression);
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
      null_jump = current_function_->instructions.size();
      Emit({.opcode = Opcode::JumpIfNull, .lhs = base, .location = expression.location});
    }
  }
  LowerBuiltinDefaults(builtin, expression, base);
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
  if (HasRegExpArrayLayout(builtin)) {
    auto type = model_.expression_types.at(&expression);
    type.nullable = false;
    if (type.name.ends_with('?')) {
      type.name = type.name.substr(0, type.name.size() - 1);
    }
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = static_cast<RegisterId>(base + arity - 1),
        .immediate = static_cast<std::int64_t>(RegisterArrayType(type)),
        .location = expression.location,
    });
  }
  const auto result = AllocateTemporary(expression.location);
  EmitBuiltinCall(builtin, result, base, expression.location);
  FinishNullSafe(null_jump, result, expression.location);
  return {.reg = result, .type = model_.expression_types.at(&expression)};
}

void Lowerer::LowerBuiltinDefaults(Builtin builtin,
                                   const frontend::Expression& expression,
                                   RegisterId base) {
  const auto arity = BuiltinArity(builtin);
  if (builtin == Builtin::NewRegExp) {
    for (std::size_t index = 1; index < arity; ++index) {
      Emit({
          .opcode = Opcode::LoadBool,
          .dst = static_cast<RegisterId>(base + index),
          .immediate = index == 2 ? 1 : 0,
          .location = expression.location,
      });
    }
  }
  if (builtin == Builtin::RegExpAllMatches || builtin == Builtin::RegExpMatchAsPrefix) {
    Emit({.opcode = Opcode::LoadInt, .dst = base + 2, .location = expression.location});
  }
  if (builtin == Builtin::ArrayJoin && expression.arguments.empty()) {
    frontend::Expression separator;
    separator.text = ",";
    separator.location = expression.location;
    const auto value = LowerStringLiteral(separator);
    Emit({
        .opcode = Opcode::Move,
        .dst = base + 1,
        .lhs = value.reg,
        .location = expression.location,
    });
  } else if (builtin == Builtin::ArraySlice || builtin == Builtin::ArrayFill ||
             builtin == Builtin::ByteArraySlice) {
    const auto start = base + (builtin == Builtin::ArrayFill ? 2 : 1);
    Emit({.opcode = Opcode::LoadInt, .dst = start, .location = expression.location});
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = start + 1,
        .immediate = std::numeric_limits<std::int64_t>::max(),
        .location = expression.location,
    });
  }
  if (builtin >= Builtin::ByteDataGetInt16 && builtin <= Builtin::ByteDataSetFloat64) {
    Emit({
        .opcode = Opcode::LoadInt,
        .dst = static_cast<RegisterId>(base + arity - 1),
        .location = expression.location,
    });
  }
}

}  // namespace vela::ir
