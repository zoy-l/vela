#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/type.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_collection.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_type.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela {
namespace internal {

template <class T>
Result<Local<T>> CreateBuiltinContainer(HandleScope& scope) {
  return ReceiveHostValue<T>(scope, [&](runtime::Value& output) -> Result<void> {
    const auto type =
        runtime::ResolveBuiltinNativeType(scope.isolate(), runtime::internal::NativeTypeOf<T>());
    if (!type) {
      return std::unexpected(Error{.status = type.error(), .message = {}, .stack_trace = {}});
    }
    return CreateHostContainer(scope, *type, output);
  });
}

template <class T>
Result<Local<T>> CreateTypedContainer(HandleScope& scope, const Type<T>& type) {
  if (&scope.isolate() != &type.isolate()) {
    return std::unexpected(
        Error{.status = ExecutionStatus::InvalidIsolate, .message = {}, .stack_trace = {}});
  }
  return ReceiveHostValue<T>(scope, [&](runtime::Value& output) {
    return CreateHostContainer(scope, TypeAccess::Read(type).constraint(), output);
  });
}

template <class T>
Result<Local<Array<T>>> ProjectCollection(HandleScope& scope,
                                          runtime::Value collection,
                                          runtime::CollectionProjection projection) {
  return ReceiveHostValue<Array<T>>(scope, [&](runtime::Value& output) {
    return ProjectHostCollection(scope, collection, projection, output);
  });
}

}  // namespace internal

template <class T>
[[nodiscard]] Result<Local<Array<T>>> CreateArray(HandleScope& scope) {
  return internal::CreateBuiltinContainer<Array<T>>(scope);
}

template <class T>
[[nodiscard]] Result<Local<Array<T>>> CreateArray(HandleScope& scope, const Type<Array<T>>& type) {
  return internal::CreateTypedContainer(scope, type);
}

template <class K, class V>
[[nodiscard]] Result<Local<Map<K, V>>> CreateMap(HandleScope& scope) {
  return internal::CreateBuiltinContainer<Map<K, V>>(scope);
}

template <class K, class V>
[[nodiscard]] Result<Local<Map<K, V>>> CreateMap(HandleScope& scope, const Type<Map<K, V>>& type) {
  return internal::CreateTypedContainer(scope, type);
}

template <class T>
[[nodiscard]] Result<Local<Set<T>>> CreateSet(HandleScope& scope) {
  return internal::CreateBuiltinContainer<Set<T>>(scope);
}

template <class T>
[[nodiscard]] Result<Local<Set<T>>> CreateSet(HandleScope& scope, const Type<Set<T>>& type) {
  return internal::CreateTypedContainer(scope, type);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<HostValue<typename internal::ArrayType<U>::Element>> Local<T>::Get(
    HandleScope& scope,
    std::int64_t index) const {
  auto receiver = internal::EncodeHostValue<T>(scope, *this);
  if (!receiver) {
    return std::unexpected(std::move(receiver.error()));
  }
  using Element = internal::ArrayType<U>::Element;
  return internal::ReceiveHostValue<Element>(scope, [&](runtime::Value& output) {
    return internal::ReadHostArray(scope, *receiver, index, output);
  });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<void> Local<T>::Set(HandleScope& scope,
                           std::int64_t index,
                           HostValue<typename internal::ArrayType<U>::Element> value) const {
  using Element = internal::ArrayType<U>::Element;
  auto arguments = internal::EncodeHostArguments<T, Element>(scope, *this, value);
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  return internal::WriteHostArray(scope, arguments->front(), index, arguments->back());
}

template <class T>
std::int64_t Local<T>::Size() const noexcept
  requires(internal::ArrayType<T>::kIsArray || internal::CollectionType<T>::kIsCollection)
{
  const auto& object = *internal::HandleAccess::Read(*this).AsObject();
  if constexpr (internal::ArrayType<T>::kIsArray) {
    return *runtime::Heap::ArrayLength(object);
  } else {
    return *runtime::Heap::CollectionLength(object);
  }
}

template <class T>
std::int64_t Local<T>::Capacity() const noexcept
  requires internal::ArrayType<T>::kIsArray
{
  return internal::HostArrayCapacity(internal::HandleAccess::Read(*this));
}

template <class T>
Result<void> Local<T>::Reserve(HandleScope& scope, std::int64_t capacity) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<void, T, std::int64_t>(scope, Builtin::ArrayReserve, *this,
                                                            capacity);
}

template <class T>
Result<void> Local<T>::Clear(HandleScope& scope) const
  requires(internal::ArrayType<T>::kIsArray || internal::CollectionType<T>::kIsCollection)
{
  constexpr auto kBuiltin =
      internal::ArrayType<T>::kIsArray ? Builtin::ArrayClear : Builtin::CollectionClear;
  return internal::CallHostContainer<void, T>(scope, kBuiltin, *this);
}

template <class T>
Result<void> Local<T>::Reverse(HandleScope& scope) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<void, T>(scope, Builtin::ArrayReverse, *this);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<std::int64_t> Local<T>::Push(
    HandleScope& scope,
    HostValue<typename internal::ArrayType<U>::Element> value) const {
  return internal::CallHostContainer<std::int64_t, T, typename internal::ArrayType<U>::Element>(
      scope, Builtin::ArrayPush, *this, value);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<std::optional<HostValue<typename internal::ArrayType<U>::Element>>> Local<T>::Pop(
    HandleScope& scope) const {
  return internal::TakeHostElement(scope, Builtin::ArrayPop, *this);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<std::optional<HostValue<typename internal::ArrayType<U>::Element>>> Local<T>::Shift(
    HandleScope& scope) const {
  return internal::TakeHostElement(scope, Builtin::ArrayShift, *this);
}

template <class T>
Result<Local<T>> Local<T>::Slice(HandleScope& scope, std::int64_t start, std::int64_t end) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<T, T, std::int64_t, std::int64_t>(scope, Builtin::ArraySlice,
                                                                       *this, start, end);
}

template <class T>
Result<Local<T>> Local<T>::Splice(HandleScope& scope,
                                  std::int64_t start,
                                  std::int64_t count,
                                  Local<T> items) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<T, T, std::int64_t, std::int64_t, T>(
      scope, Builtin::ArraySplice, *this, start, count, items);
}

template <class T>
Result<std::int64_t> Local<T>::PushMany(HandleScope& scope, Local<T> items) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<std::int64_t, T, T>(scope, Builtin::ArrayPushMany, *this,
                                                         items);
}

template <class T>
Result<std::int64_t> Local<T>::Unshift(HandleScope& scope, Local<T> items) const
  requires internal::ArrayType<T>::kIsArray
{
  return internal::CallHostContainer<std::int64_t, T, T>(scope, Builtin::ArrayUnshift, *this,
                                                         items);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::ArrayType<U>::kIsArray)
Result<void> Local<T>::Fill(HandleScope& scope,
                            HostValue<typename internal::ArrayType<U>::Element> value,
                            std::int64_t start,
                            std::int64_t end) const {
  return internal::CallHostContainer<void, T, typename internal::ArrayType<U>::Element,
                                     std::int64_t, std::int64_t>(scope, Builtin::ArrayFill, *this,
                                                                 value, start, end);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
Result<std::optional<HostValue<typename internal::CollectionType<U>::Mapped>>> Local<T>::Get(
    HandleScope& scope,
    HostValue<typename internal::CollectionType<U>::Key> key) const {
  auto arguments = internal::EncodeHostArguments<T, typename internal::CollectionType<U>::Key>(
      scope, *this, key);
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  return internal::ReceiveHostValue<typename internal::CollectionType<U>::Mapped>(
      scope, [&](runtime::Value& output) {
        return internal::ReadHostMap(scope, *arguments->front().AsObject(), arguments->back(),
                                     output);
      });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
Result<void> Local<T>::Set(HandleScope& scope,
                           HostValue<typename internal::CollectionType<U>::Key> key,
                           HostValue<typename internal::CollectionType<U>::Mapped> value) const {
  return internal::CallHostContainer<void, T, typename internal::CollectionType<U>::Key,
                                     typename internal::CollectionType<U>::Mapped>(
      scope, Builtin::MapSet, *this, key, value);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection &&
           !internal::CollectionType<U>::kIsMap)
Result<Insertion> Local<T>::Add(HandleScope& scope,
                                HostValue<typename internal::CollectionType<U>::Key> key) const {
  return internal::CallHostContainer<bool, T, typename internal::CollectionType<U>::Key>(
             scope, Builtin::SetAdd, *this, key)
      .transform(
          [](bool inserted) { return inserted ? Insertion::Inserted : Insertion::Existing; });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
Result<bool> Local<T>::Has(HandleScope& scope,
                           HostValue<typename internal::CollectionType<U>::Key> key) const {
  return internal::CallHostContainer<bool, T, typename internal::CollectionType<U>::Key>(
      scope, Builtin::CollectionHas, *this, key);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
Result<Removal> Local<T>::Delete(HandleScope& scope,
                                 HostValue<typename internal::CollectionType<U>::Key> key) const {
  return internal::CallHostContainer<bool, T, typename internal::CollectionType<U>::Key>(
             scope, Builtin::CollectionDelete, *this, key)
      .transform([](bool removed) { return removed ? Removal::Removed : Removal::Missing; });
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsCollection)
Result<Local<Array<typename internal::CollectionType<U>::Key>>> Local<T>::Keys(
    HandleScope& scope) const {
  auto receiver = internal::EncodeHostValue<T>(scope, *this);
  if (!receiver) {
    return std::unexpected(std::move(receiver.error()));
  }
  return internal::ProjectCollection<typename internal::CollectionType<U>::Key>(
      scope, *receiver, runtime::CollectionProjection::Keys);
}

template <class T>
template <class U>
  requires(std::is_same_v<U, T> && internal::CollectionType<U>::kIsMap)
Result<Local<Array<typename internal::CollectionType<U>::Mapped>>> Local<T>::Values(
    HandleScope& scope) const {
  auto receiver = internal::EncodeHostValue<T>(scope, *this);
  if (!receiver) {
    return std::unexpected(std::move(receiver.error()));
  }
  return internal::ProjectCollection<typename internal::CollectionType<U>::Mapped>(
      scope, *receiver, runtime::CollectionProjection::Values);
}

template <class T>
Result<Local<T>> Local<T>::Union(HandleScope& scope, Local<T> other) const
  requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap)
{
  return internal::CallHostContainer<T, T, T>(scope, Builtin::SetUnion, *this, other);
}

template <class T>
Result<Local<T>> Local<T>::Intersection(HandleScope& scope, Local<T> other) const
  requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap)
{
  return internal::CallHostContainer<T, T, T>(scope, Builtin::SetIntersection, *this, other);
}

template <class T>
Result<Local<T>> Local<T>::Difference(HandleScope& scope, Local<T> other) const
  requires(internal::CollectionType<T>::kIsCollection && !internal::CollectionType<T>::kIsMap)
{
  return internal::CallHostContainer<T, T, T>(scope, Builtin::SetDifference, *this, other);
}

}  // namespace vela
