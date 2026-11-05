#include "runtime/native/host_collection.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/collections/array_operations.h"
#include "runtime/collections/collection_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/isolate/layout_transaction.h"
#include "runtime/native/host_value.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::internal {
namespace {

Result<runtime::Value> StoreElementValue(runtime::Value value, const runtime::ArrayLayout& layout) {
  const auto* nominal = layout.element.nominal;
  if (nominal != nullptr && nominal->IsEnum() &&
      !runtime::Heap::IsValueOfType(value, layout.value_type())) {
    return std::unexpected(
        Error{.status = ExecutionStatus::TypeError, .message = {}, .stack_trace = {}});
  }
  return runtime::UnboxDynamicValue(value, layout.element);
}

Result<void> ReceiveStoredElement(HandleScope& scope,
                                  runtime::Value value,
                                  const runtime::ArrayLayout& layout,
                                  runtime::Value& output) {
  auto& heap = scope.isolate().heap();
  const auto boxed = runtime::BoxDynamicValue(heap, value, layout.element);
  if (!boxed) {
    return std::unexpected(Error{.status = boxed.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(heap, output, *boxed);
  return {};
}

}  // namespace

Result<void> ReadHostArray(HandleScope& scope,
                           runtime::Value array,
                           std::int64_t index,
                           runtime::Value& output) {
  const auto value = runtime::ReadIndex(*array.AsObject(), index);
  if (!value) {
    return std::unexpected(Error{.status = value.error(), .message = {}, .stack_trace = {}});
  }
  return ReceiveStoredElement(scope, *value, *runtime::Heap::ArrayTypeOf(*array.AsObject()),
                              output);
}

Result<void> WriteHostArray(HandleScope& scope,
                            runtime::Value array,
                            std::int64_t index,
                            runtime::Value value) {
  const auto& layout = *runtime::Heap::ArrayTypeOf(*array.AsObject());
  auto stored = StoreElementValue(value, layout);
  if (!stored) {
    return std::unexpected(std::move(stored.error()));
  }
  const auto written =
      runtime::WriteIndex(scope.isolate().heap(), *array.AsObject(), index, *stored);
  if (!written) {
    return std::unexpected(Error{.status = written.error(), .message = {}, .stack_trace = {}});
  }
  return {};
}

std::int64_t HostArrayCapacity(runtime::Value array) noexcept {
  return *runtime::Heap::ArrayCapacity(*array.AsObject());
}

Result<void> CreateHostContainer(HandleScope& scope,
                                 runtime::TypeConstraint type,
                                 runtime::Value& output) {
  auto& heap = scope.isolate().heap();
  const runtime::RootRange type_root(heap, std::span(&type, 1));
  auto* object = type.array != nullptr ? heap.AllocateArray(*type.array, 0)
                                       : heap.AllocateCollection(*type.collection);
  if (object == nullptr) {
    return std::unexpected(
        Error{.status = ExecutionStatus::OutOfMemory, .message = {}, .stack_trace = {}});
  }
  StoreHostResult(heap, output, runtime::Value::Object(object));
  return {};
}

Result<void> ExecuteHostArray(HandleScope& scope,
                              Builtin builtin,
                              std::span<const runtime::Value> arguments,
                              runtime::Value& output) {
  auto& heap = scope.isolate().heap();
  std::array<runtime::Value, BuiltinArity(Builtin::ArrayFill)> stored;
  if (builtin == Builtin::ArrayPush || builtin == Builtin::ArrayFill) {
    const auto& layout = *runtime::Heap::ArrayTypeOf(*arguments.front().AsObject());
    auto value = StoreElementValue(arguments[1], layout);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    std::ranges::copy(arguments, stored.begin());
    stored[1] = *value;
    arguments = std::span(stored).first(arguments.size());
  }
  const auto result = runtime::ExecuteArrayBuiltin(builtin, arguments, heap);
  if (!result) {
    return std::unexpected(Error{.status = result.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(heap, output, *result);
  return {};
}

Result<HostPresence> TakeHostArray(HandleScope& scope,
                                   Builtin builtin,
                                   runtime::Value array,
                                   runtime::Value& output) {
  auto& object = *array.AsObject();
  if (*runtime::Heap::ArrayLength(object) == 0) {
    return HostPresence::Missing;
  }
  const auto value =
      runtime::ExecuteArrayBuiltin(builtin, std::span(&array, 1), scope.isolate().heap());
  if (!value) {
    return std::unexpected(Error{.status = value.error(), .message = {}, .stack_trace = {}});
  }
  auto result = ReceiveStoredElement(scope, *value, *runtime::Heap::ArrayTypeOf(object), output);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return HostPresence::Present;
}

Result<void> ExecuteHostCollection(HandleScope& scope,
                                   Builtin builtin,
                                   std::span<const runtime::Value> arguments,
                                   runtime::Value& output) {
  const auto& layout = *runtime::Heap::CollectionTypeOf(*arguments.front().AsObject());
  std::array<runtime::Value, BuiltinArity(Builtin::MapSet)> stored;
  if (builtin == Builtin::SetAdd || builtin == Builtin::MapSet ||
      builtin == Builtin::CollectionHas || builtin == Builtin::CollectionDelete) {
    auto key = StoreElementValue(arguments[1], layout.keys);
    if (!key) {
      return std::unexpected(std::move(key.error()));
    }
    std::ranges::copy(arguments, stored.begin());
    stored[1] = *key;
    if (builtin == Builtin::MapSet) {
      auto value = StoreElementValue(arguments[2], *layout.values);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      stored[2] = *value;
    }
    arguments = std::span(stored).first(arguments.size());
  }
  const auto result = runtime::ExecuteCollectionBuiltin(builtin, arguments, scope.isolate().heap());
  if (!result) {
    return std::unexpected(Error{.status = result.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(scope.isolate().heap(), output, *result);
  return {};
}

Result<HostPresence> ReadHostMap(HandleScope& scope,
                                 const runtime::HeapObject& map,
                                 runtime::Value key,
                                 runtime::Value& output) {
  const auto& layout = *runtime::Heap::CollectionTypeOf(map);
  auto stored = StoreElementValue(key, layout.keys);
  if (!stored) {
    return std::unexpected(std::move(stored.error()));
  }
  const auto value = runtime::Heap::FindCollectionValue(map, *stored);
  if (!value) {
    return std::unexpected(Error{.status = value.error(), .message = {}, .stack_trace = {}});
  }
  if (!*value) {
    return HostPresence::Missing;
  }
  auto result = ReceiveStoredElement(scope, **value, *layout.values, output);
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  return HostPresence::Present;
}

Result<void> ProjectHostCollection(HandleScope& scope,
                                   runtime::Value collection,
                                   runtime::CollectionProjection projection,
                                   runtime::Value& output) {
  auto& isolate = scope.isolate();
  auto& heap = isolate.heap();
  const auto& object = *collection.AsObject();
  const auto& layout = *runtime::Heap::CollectionTypeOf(object);
  runtime::LayoutTransaction transaction(isolate, runtime::LayoutLifetime::Collectible);
  const auto* elements = layout.element_layout(projection);
  const auto* array_type = isolate.CreateArrayLayout(*elements);
  auto* array = heap.AllocateArray(*array_type, *runtime::Heap::CollectionLength(object));
  if (array == nullptr) {
    return std::unexpected(
        Error{.status = ExecutionStatus::OutOfMemory, .message = {}, .stack_trace = {}});
  }
  // 数组已经引用新布局；即使后续填充失败，布局也须随该对象一起经 GC 退休。
  transaction.CommitStructural();
  const auto result = heap.CopyCollection(object, *array, projection);
  if (!result) {
    return std::unexpected(Error{.status = result.error(), .message = {}, .stack_trace = {}});
  }
  StoreHostResult(heap, output, runtime::Value::Object(array));
  return {};
}

}  // namespace vela::internal
