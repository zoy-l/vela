#include <vela/bytes.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "runtime/heap/heap.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_builtin.h"

namespace vela {

std::int64_t ByteArrayLength(Local<ByteArray> bytes) noexcept {
  return static_cast<std::int64_t>(
      runtime::Heap::ByteArrayBytes(*internal::HandleAccess::Read(bytes).AsObject()).size());
}

std::int64_t ByteDataLength(Local<ByteData> data) noexcept {
  return static_cast<std::int64_t>(
      runtime::Heap::ByteDataBytes(*internal::HandleAccess::Read(data).AsObject()).size());
}

std::vector<std::uint8_t> CopyBytes(Local<ByteArray> bytes) {
  const auto storage =
      runtime::Heap::ByteArrayBytes(*internal::HandleAccess::Read(bytes).AsObject());
  return {storage.begin(), storage.end()};
}

std::vector<std::uint8_t> CopyBytes(Local<ByteData> data) {
  const auto storage = runtime::Heap::ByteDataBytes(*internal::HandleAccess::Read(data).AsObject());
  return {storage.begin(), storage.end()};
}

Result<Local<ByteArray>> CopyToByteArray(HandleScope& scope, std::span<const std::uint8_t> bytes) {
  auto result = CreateByteArray(scope, static_cast<std::int64_t>(bytes.size()));
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  const auto storage =
      runtime::Heap::ByteArrayBytes(*internal::HandleAccess::Read(*result).AsObject());
  std::ranges::copy(bytes, storage.begin());
  return *result;
}

Result<HostValue<ByteArray>> CreateByteArray(HandleScope& scope, HostValue<std::int64_t> length) {
  return internal::CallHostBuiltin<ByteArray, std::int64_t>(scope, Builtin::NewByteArray, length);
}

Result<HostValue<ByteArray>> ByteArrayFrom(HandleScope& scope,
                                           HostValue<Array<std::int64_t>> values) {
  return internal::CallHostBuiltin<ByteArray, Array<std::int64_t>>(scope, Builtin::ByteArrayFrom,
                                                                   values);
}

Result<HostValue<std::int64_t>> ByteArrayGet(HandleScope& scope,
                                             HostValue<ByteArray> bytes,
                                             HostValue<std::int64_t> index) {
  return internal::CallHostBuiltin<std::int64_t, ByteArray, std::int64_t>(
      scope, Builtin::ByteArrayGetIndex, bytes, index);
}

Result<HostValue<void>> ByteArraySet(HandleScope& scope,
                                     HostValue<ByteArray> bytes,
                                     HostValue<std::int64_t> index,
                                     HostValue<std::int64_t> value) {
  return internal::CallHostBuiltin<void, ByteArray, std::int64_t, std::int64_t>(
      scope, Builtin::ByteArraySetIndex, bytes, index, value);
}

Result<HostValue<ByteArray>> ByteArraySlice(HandleScope& scope,
                                            HostValue<ByteArray> bytes,
                                            HostValue<std::int64_t> start,
                                            HostValue<std::int64_t> end) {
  return internal::CallHostBuiltin<ByteArray, ByteArray, std::int64_t, std::int64_t>(
      scope, Builtin::ByteArraySlice, bytes, start, end);
}

Result<HostValue<ByteData>> CreateByteData(HandleScope& scope,
                                           HostValue<ByteArray> bytes,
                                           HostValue<std::int64_t> offset,
                                           HostValue<Nullable<std::int64_t>> length) {
  return internal::CallHostBuiltin<ByteData, ByteArray, std::int64_t, Nullable<std::int64_t>>(
      scope, Builtin::ByteDataView, bytes, offset, length);
}

Result<std::int64_t> ByteDataGetInt8(HandleScope& scope,
                                     Local<ByteData> data,
                                     std::int64_t offset) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t>(
      scope, Builtin::ByteDataGetInt8, data, offset);
}

Result<void> ByteDataSetInt8(HandleScope& scope,
                             Local<ByteData> data,
                             std::int64_t offset,
                             std::int64_t value) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetInt8, data, offset, value);
}

Result<std::int64_t> ByteDataGetUint8(HandleScope& scope,
                                      Local<ByteData> data,
                                      std::int64_t offset) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t>(
      scope, Builtin::ByteDataGetUint8, data, offset);
}

Result<void> ByteDataSetUint8(HandleScope& scope,
                              Local<ByteData> data,
                              std::int64_t offset,
                              std::int64_t value) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetUint8, data, offset, value);
}

Result<std::int64_t> ByteDataGetInt16(HandleScope& scope,
                                      Local<ByteData> data,
                                      std::int64_t offset,
                                      ByteOrder order) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetInt16, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetInt16(HandleScope& scope,
                              Local<ByteData> data,
                              std::int64_t offset,
                              std::int64_t value,
                              ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetInt16, data, offset, value, static_cast<std::int64_t>(order));
}

Result<std::int64_t> ByteDataGetUint16(HandleScope& scope,
                                       Local<ByteData> data,
                                       std::int64_t offset,
                                       ByteOrder order) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetUint16, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetUint16(HandleScope& scope,
                               Local<ByteData> data,
                               std::int64_t offset,
                               std::int64_t value,
                               ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetUint16, data, offset, value, static_cast<std::int64_t>(order));
}

Result<std::int64_t> ByteDataGetInt32(HandleScope& scope,
                                      Local<ByteData> data,
                                      std::int64_t offset,
                                      ByteOrder order) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetInt32, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetInt32(HandleScope& scope,
                              Local<ByteData> data,
                              std::int64_t offset,
                              std::int64_t value,
                              ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetInt32, data, offset, value, static_cast<std::int64_t>(order));
}

Result<std::int64_t> ByteDataGetUint32(HandleScope& scope,
                                       Local<ByteData> data,
                                       std::int64_t offset,
                                       ByteOrder order) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetUint32, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetUint32(HandleScope& scope,
                               Local<ByteData> data,
                               std::int64_t offset,
                               std::int64_t value,
                               ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetUint32, data, offset, value, static_cast<std::int64_t>(order));
}

Result<std::int64_t> ByteDataGetInt64(HandleScope& scope,
                                      Local<ByteData> data,
                                      std::int64_t offset,
                                      ByteOrder order) {
  return internal::CallHostBuiltin<std::int64_t, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetInt64, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetInt64(HandleScope& scope,
                              Local<ByteData> data,
                              std::int64_t offset,
                              std::int64_t value,
                              ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataSetInt64, data, offset, value, static_cast<std::int64_t>(order));
}

Result<double> ByteDataGetFloat32(HandleScope& scope,
                                  Local<ByteData> data,
                                  std::int64_t offset,
                                  ByteOrder order) {
  return internal::CallHostBuiltin<double, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetFloat32, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetFloat32(HandleScope& scope,
                                Local<ByteData> data,
                                std::int64_t offset,
                                double value,
                                ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, double, std::int64_t>(
      scope, Builtin::ByteDataSetFloat32, data, offset, value, static_cast<std::int64_t>(order));
}

Result<double> ByteDataGetFloat64(HandleScope& scope,
                                  Local<ByteData> data,
                                  std::int64_t offset,
                                  ByteOrder order) {
  return internal::CallHostBuiltin<double, ByteData, std::int64_t, std::int64_t>(
      scope, Builtin::ByteDataGetFloat64, data, offset, static_cast<std::int64_t>(order));
}

Result<void> ByteDataSetFloat64(HandleScope& scope,
                                Local<ByteData> data,
                                std::int64_t offset,
                                double value,
                                ByteOrder order) {
  return internal::CallHostBuiltin<void, ByteData, std::int64_t, double, std::int64_t>(
      scope, Builtin::ByteDataSetFloat64, data, offset, value, static_cast<std::int64_t>(order));
}

}  // namespace vela
