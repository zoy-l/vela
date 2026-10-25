#include "runtime/array_operations.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

BuiltinError Error(ArrayAccessError error) {
  switch (error) {
    case ArrayAccessError::NotArray:
    case ArrayAccessError::ElementTypeMismatch:
      return BuiltinError::TypeError;
    case ArrayAccessError::OutOfBounds:
      return BuiltinError::IndexOutOfBounds;
    case ArrayAccessError::OutOfMemory:
      return BuiltinError::OutOfMemory;
  }
  std::unreachable();
}

template <class T, class Convert>
std::expected<Value, BuiltinError> Result(std::expected<T, ArrayAccessError> result,
                                          const Convert& convert) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return convert(*result);
}

std::expected<Value, BuiltinError> MutationResult(std::expected<void, ArrayAccessError> result) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return Value::Null();
}

std::expected<std::uint32_t, BuiltinError> Index(Value value) {
  if (!value.IsInteger()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto index = value.AsInteger();
  if (index < 0 || std::cmp_greater(index, std::numeric_limits<std::uint32_t>::max())) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  return static_cast<std::uint32_t>(index);
}

std::expected<Value, BuiltinError> CopyArray(Builtin builtin,
                                             HeapObject& array,
                                             std::span<const Value> arguments,
                                             Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(Error(length.error()));
  }
  std::uint32_t start = 0;
  auto end = *length;
  if (builtin == Builtin::ArraySlice) {
    const auto first = arguments.subspan(1).front();
    const auto last = arguments.subspan(2).front();
    if (!first.IsInteger() || !last.IsInteger()) {
      return std::unexpected(BuiltinError::TypeError);
    }
    const auto begin_index = first.AsInteger();
    const auto end_index = last.AsInteger();
    if (begin_index < 0 || end_index < begin_index || std::cmp_greater(end_index, *length)) {
      return std::unexpected(BuiltinError::IndexOutOfBounds);
    }
    start = static_cast<std::uint32_t>(begin_index);
    end = static_cast<std::uint32_t>(end_index);
  }
  return Result(heap.SliceArray(array, start, end),
                [](HeapObject* copy) -> Value { return Value::Object(copy); });
}

}  // namespace

std::expected<Value, BuiltinError> ExecuteArrayBuiltin(Builtin builtin,
                                                       std::span<const Value> arguments,
                                                       Heap& heap) {
  const auto receiver = arguments.front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto& array = *receiver.AsObject();
  const auto integer = [](auto value) -> Value {
    return Value::Integer(static_cast<std::int64_t>(value));
  };
  const auto identity = [](Value value) -> Value { return value; };
  switch (builtin) {
    case Builtin::ArrayCopy:
    case Builtin::ArraySlice:
      return CopyArray(builtin, array, arguments, heap);
    case Builtin::ArrayFill:
      return MutationResult(heap.FillArray(array, arguments.subspan(1).front()));
    case Builtin::ArrayReverse:
      return MutationResult(Heap::ReverseArray(array));
    case Builtin::ArrayCapacity:
      return Result(Heap::ArrayCapacity(array), integer);
    case Builtin::ArrayPush:
      return Result(heap.PushArray(array, arguments.subspan(1).front()), integer);
    case Builtin::ArrayPop:
      return Result(Heap::PopArray(array), identity);
    case Builtin::ArrayClear:
      return MutationResult(Heap::ClearArray(array));
    case Builtin::ArrayReserve: {
      const auto capacity = arguments.subspan(1).front();
      if (!capacity.IsInteger()) {
        return std::unexpected(BuiltinError::TypeError);
      }
      if (capacity.AsInteger() < 0) {
        return std::unexpected(BuiltinError::IndexOutOfBounds);
      }
      return MutationResult(
          heap.ReserveArray(array, static_cast<std::size_t>(capacity.AsInteger())));
    }
    case Builtin::ArrayInsert:
    case Builtin::ArrayRemoveAt: {
      const auto index = Index(arguments.subspan(1).front());
      if (!index) {
        return std::unexpected(index.error());
      }
      if (builtin == Builtin::ArrayInsert) {
        return MutationResult(heap.InsertArray(array, *index, arguments.subspan(2).front()));
      }
      return Result(Heap::RemoveArrayElement(array, *index), identity);
    }
    case Builtin::ArrayMutation:
      return Result(Heap::ArrayMutation(array), [](std::uint64_t mutation) -> Value {
        return Value::Integer(std::bit_cast<std::int64_t>(mutation));
      });
    case Builtin::ArrayCheckMutation: {
      const auto mutation = Heap::ArrayMutation(array);
      const auto expected = arguments.subspan(1).front();
      if (!mutation || !expected.IsInteger()) {
        return std::unexpected(BuiltinError::TypeError);
      }
      if (*mutation != std::bit_cast<std::uint64_t>(expected.AsInteger())) {
        return std::unexpected(BuiltinError::ConcurrentModification);
      }
      return Value::Null();
    }
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
