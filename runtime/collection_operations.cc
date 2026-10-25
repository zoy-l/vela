#include "runtime/collection_operations.h"

#include <expected>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

auto BuiltinFailure(CollectionError error) -> BuiltinError {
  return error == CollectionError::OutOfMemory ? BuiltinError::OutOfMemory
                                               : BuiltinError::TypeError;
}

auto ExecuteSetOperation(Builtin builtin, HeapObject& receiver, Value other, Heap& heap)
    -> std::expected<Value, BuiltinError> {
  if (!other.IsObject() || other.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto operation = SetOperation::Difference;
  if (builtin == Builtin::SetUnion) {
    operation = SetOperation::Union;
  } else if (builtin == Builtin::SetIntersection) {
    operation = SetOperation::Intersection;
  }
  return heap.CombineSets(receiver, *other.AsObject(), operation)
      .transform([](HeapObject* result) -> Value { return Value::Object(result); })
      .transform_error(BuiltinFailure);
}

}  // namespace

auto ExecuteCollectionBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap)
    -> std::expected<Value, BuiltinError> {
  const auto receiver = arguments.front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto& object = *receiver.AsObject();
  switch (builtin) {
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
      return ExecuteSetOperation(builtin, object, arguments.subspan(1).front(), heap);
    case Builtin::CollectionLength: {
      const auto length = Heap::CollectionLength(object);
      if (!length) {
        return std::unexpected(BuiltinFailure(length.error()));
      }
      return Value::Integer(*length);
    }
    case Builtin::CollectionClear: {
      const auto cleared = Heap::ClearCollection(object);
      if (!cleared) {
        return std::unexpected(BuiltinFailure(cleared.error()));
      }
      return Value::Null();
    }
    case Builtin::SetAdd:
    case Builtin::MapSet: {
      const auto key = arguments.subspan(1).front();
      const auto value = builtin == Builtin::MapSet ? arguments.subspan(2).front() : Value::Null();
      const auto inserted = heap.InsertCollection(object, key, value);
      if (!inserted) {
        return std::unexpected(BuiltinFailure(inserted.error()));
      }
      return builtin == Builtin::SetAdd ? Value::Boolean(*inserted == CollectionInsert::Inserted)
                                        : Value::Null();
    }
    case Builtin::CollectionHas: {
      const auto found = Heap::HasCollectionKey(object, arguments.subspan(1).front());
      if (!found) {
        return std::unexpected(BuiltinFailure(found.error()));
      }
      return Value::Boolean(*found);
    }
    case Builtin::MapGet: {
      return Heap::GetCollectionValue(object, arguments.subspan(1).front())
          .transform_error(BuiltinFailure);
    }
    case Builtin::CollectionRemove: {
      const auto removed = Heap::RemoveCollection(object, arguments.subspan(1).front());
      if (!removed) {
        return std::unexpected(BuiltinFailure(removed.error()));
      }
      return Value::Boolean(*removed == CollectionRemove::Removed);
    }
    case Builtin::CollectionKeys:
    case Builtin::CollectionValues: {
      const auto target = arguments.subspan(1).front();
      if (!target.IsObject() || target.AsObject() == nullptr) {
        return std::unexpected(BuiltinError::TypeError);
      }
      const auto copied =
          heap.CopyCollection(object, *target.AsObject(),
                              builtin == Builtin::CollectionKeys ? CollectionProjection::Keys
                                                                 : CollectionProjection::Values);
      if (!copied) {
        return std::unexpected(BuiltinFailure(copied.error()));
      }
      return target;
    }
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
