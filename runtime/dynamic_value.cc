#include "runtime/dynamic_value.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/opcode.h"
#include "runtime/builtins.h"
#include "runtime/byte_operations.h"
#include "runtime/heap.h"
#include "runtime/numeric_operations_internal.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/value.h"
#include "runtime/value_operations.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, ExecutionStatus>;

Result Equality(Opcode operation, internal::NumericOperands operands) {
  const auto& [left, right] = operands;
  if (!left.IsNull() && !right.IsNull()) {
    if (left.kind() != right.kind()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (left.IsObject()) {
      const auto kind = Heap::KindOf(*left.AsObject());
      const auto* layout = Heap::ClassOf(*left.AsObject());
      if (kind == ObjectKind::Array || kind == ObjectKind::MutableString ||
          kind == ObjectKind::Expected || kind == ObjectKind::Collection ||
          (layout != nullptr && layout->callable())) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
    }
  }
  const auto equal = IsEqualValue(left, right);
  return Value::Boolean(operation == Opcode::EqualInt ? equal : !equal);
}

Result Binary(std::span<const Value> arguments, Heap& heap) {
  const auto selector = arguments.front();
  if (!selector.IsInteger() || selector.AsInteger() < 0 ||
      selector.AsInteger() > static_cast<std::int64_t>(Opcode::Return)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<Opcode>(selector.AsInteger());
  const auto left = arguments.subspan(1).front();
  const auto right = arguments.subspan(2).front();
  if (operation == Opcode::EqualInt || operation == Opcode::NotEqualInt) {
    return Equality(operation, {.left = left, .right = right});
  }
  if (operation == Opcode::AddInt && (left.IsString() || right.IsString())) {
    return ConcatenateValues(heap, left, right);
  }
  std::optional<Opcode> floating;
  switch (operation) {
    case Opcode::AddInt:
      floating = Opcode::AddDouble;
      break;
    case Opcode::SubInt:
      floating = Opcode::SubDouble;
      break;
    case Opcode::MulInt:
      floating = Opcode::MulDouble;
      break;
    case Opcode::DivInt:
      floating = Opcode::DivDouble;
      break;
    case Opcode::LessInt:
      floating = Opcode::LessDouble;
      break;
    case Opcode::LessEqualInt:
      floating = Opcode::LessEqualDouble;
      break;
    case Opcode::GreaterInt:
      floating = Opcode::GreaterDouble;
      break;
    case Opcode::GreaterEqualInt:
      floating = Opcode::GreaterEqualDouble;
      break;
    case Opcode::ModInt:
    case Opcode::BitAndInt:
    case Opcode::BitOrInt:
    case Opcode::BitXorInt:
    case Opcode::ShiftLeftInt:
    case Opcode::ShiftRightInt:
    case Opcode::ShiftUnsignedRightInt:
      break;
    default:
      return std::unexpected(ExecutionStatus::TypeError);
  }
  const internal::NumericOperands operands{.left = left, .right = right};
  return left.IsDouble() && right.IsDouble() && floating
             ? internal::DoubleOperation(*floating, operands)
             : internal::IntegerOperation(operation, operands);
}

Result ArrayIndex(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  const auto receiver = arguments.front();
  const auto index = arguments.subspan(1).front();
  if (!index.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& object = *receiver.AsObject();
  const auto& layout = *Heap::ArrayTypeOf(object);
  if (builtin == Builtin::DynamicIndexWrite) {
    const auto original = arguments.subspan(2).front();
    if (!Heap::IsValueOfType(original, layout.value_type())) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    auto value = original;
    const auto* element = layout.element.instance;
    if (!value.IsNull() && element != nullptr && element->IsEnum()) {
      value = *Heap::GetField(*value.AsObject(), 0);
    }
    const auto written = WriteIndex(heap, &object, index.AsInteger(), value);
    return written ? Result(original) : std::unexpected(written.error());
  }
  const auto value = ReadIndex(&object, index.AsInteger());
  return value ? BoxDynamicValue(heap, *value, layout.element) : value;
}

Result Index(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  const auto receiver = arguments.front();
  if (builtin == Builtin::DynamicIndexRead && receiver.IsString()) {
    return ExecuteBuiltin(Builtin::StringIndex, arguments, heap);
  }
  if (!receiver.IsObject()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto kind = Heap::KindOf(*receiver.AsObject());
  if (kind == ObjectKind::ByteArray) {
    const auto writing = builtin == Builtin::DynamicIndexWrite;
    const auto value = ExecuteByteBuiltin(
        writing ? Builtin::ByteArraySetIndex : Builtin::ByteArrayGetIndex, arguments, heap);
    return value && writing ? Result(arguments.subspan(2).front()) : value;
  }
  if (kind == ObjectKind::RegExpMatch && builtin == Builtin::DynamicIndexRead) {
    const auto value = ExecuteRegExpBuiltin(Builtin::RegExpMatchGroup, arguments, heap);
    return value ? Result(*value) : std::unexpected(value.error().status);
  }
  return kind == ObjectKind::Array ? ArrayIndex(builtin, arguments, heap)
                                   : std::unexpected(ExecutionStatus::TypeError);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteDynamicValue(Builtin builtin,
                                                          std::span<const Value> arguments,
                                                          Heap& heap) {
  if (builtin == Builtin::DynamicBinary) {
    return Binary(arguments, heap);
  }
  if (builtin == Builtin::DynamicNegate) {
    const auto operand = arguments.front();
    if (operand.IsInteger()) {
      return internal::IntegerArithmetic(Opcode::SubInt, 0, operand.AsInteger());
    }
    return operand.IsDouble() ? Result(Value::Double(-operand.AsDouble()))
                              : std::unexpected(ExecutionStatus::TypeError);
  }
  return Index(builtin, arguments, heap);
}

}  // namespace vela::runtime
