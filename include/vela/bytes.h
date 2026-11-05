#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vela/error.h>
#include <vela/handles.h>
#include <vela/types.h>

#include "base/byte_access.h"

namespace vela {

[[nodiscard]] std::int64_t ByteArrayLength(Local<ByteArray> bytes) noexcept;
[[nodiscard]] std::int64_t ByteDataLength(Local<ByteData> data) noexcept;
[[nodiscard]] std::vector<std::uint8_t> CopyBytes(Local<ByteArray> bytes);
[[nodiscard]] std::vector<std::uint8_t> CopyBytes(Local<ByteData> data);
[[nodiscard]] Result<Local<ByteArray>> CopyToByteArray(HandleScope& scope,
                                                       std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<HostValue<ByteArray>> CreateByteArray(HandleScope& scope,
                                                           HostValue<std::int64_t> length);
[[nodiscard]] Result<HostValue<ByteArray>> ByteArrayFrom(HandleScope& scope,
                                                         HostValue<Array<std::int64_t>> values);
[[nodiscard]] Result<HostValue<std::int64_t>> ByteArrayGet(HandleScope& scope,
                                                           HostValue<ByteArray> bytes,
                                                           HostValue<std::int64_t> index);
[[nodiscard]] Result<HostValue<void>> ByteArraySet(HandleScope& scope,
                                                   HostValue<ByteArray> bytes,
                                                   HostValue<std::int64_t> index,
                                                   HostValue<std::int64_t> value);
[[nodiscard]] Result<HostValue<ByteArray>> ByteArraySlice(HandleScope& scope,
                                                          HostValue<ByteArray> bytes,
                                                          HostValue<std::int64_t> start,
                                                          HostValue<std::int64_t> end);
[[nodiscard]] Result<HostValue<ByteData>> CreateByteData(
    HandleScope& scope,
    HostValue<ByteArray> bytes,
    HostValue<std::int64_t> offset = 0,
    HostValue<Nullable<std::int64_t>> length = std::nullopt);
[[nodiscard]] Result<std::int64_t> ByteDataGetInt8(HandleScope& scope,
                                                   Local<ByteData> data,
                                                   std::int64_t offset);
[[nodiscard]] Result<void> ByteDataSetInt8(HandleScope& scope,
                                           Local<ByteData> data,
                                           std::int64_t offset,
                                           std::int64_t value);
[[nodiscard]] Result<std::int64_t> ByteDataGetUint8(HandleScope& scope,
                                                    Local<ByteData> data,
                                                    std::int64_t offset);
[[nodiscard]] Result<void> ByteDataSetUint8(HandleScope& scope,
                                            Local<ByteData> data,
                                            std::int64_t offset,
                                            std::int64_t value);
[[nodiscard]] Result<std::int64_t> ByteDataGetInt16(HandleScope& scope,
                                                    Local<ByteData> data,
                                                    std::int64_t offset,
                                                    ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetInt16(HandleScope& scope,
                                            Local<ByteData> data,
                                            std::int64_t offset,
                                            std::int64_t value,
                                            ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<std::int64_t> ByteDataGetUint16(HandleScope& scope,
                                                     Local<ByteData> data,
                                                     std::int64_t offset,
                                                     ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetUint16(HandleScope& scope,
                                             Local<ByteData> data,
                                             std::int64_t offset,
                                             std::int64_t value,
                                             ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<std::int64_t> ByteDataGetInt32(HandleScope& scope,
                                                    Local<ByteData> data,
                                                    std::int64_t offset,
                                                    ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetInt32(HandleScope& scope,
                                            Local<ByteData> data,
                                            std::int64_t offset,
                                            std::int64_t value,
                                            ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<std::int64_t> ByteDataGetUint32(HandleScope& scope,
                                                     Local<ByteData> data,
                                                     std::int64_t offset,
                                                     ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetUint32(HandleScope& scope,
                                             Local<ByteData> data,
                                             std::int64_t offset,
                                             std::int64_t value,
                                             ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<std::int64_t> ByteDataGetInt64(HandleScope& scope,
                                                    Local<ByteData> data,
                                                    std::int64_t offset,
                                                    ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetInt64(HandleScope& scope,
                                            Local<ByteData> data,
                                            std::int64_t offset,
                                            std::int64_t value,
                                            ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<double> ByteDataGetFloat32(HandleScope& scope,
                                                Local<ByteData> data,
                                                std::int64_t offset,
                                                ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetFloat32(HandleScope& scope,
                                              Local<ByteData> data,
                                              std::int64_t offset,
                                              double value,
                                              ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<double> ByteDataGetFloat64(HandleScope& scope,
                                                Local<ByteData> data,
                                                std::int64_t offset,
                                                ByteOrder order = ByteOrder::Big);
[[nodiscard]] Result<void> ByteDataSetFloat64(HandleScope& scope,
                                              Local<ByteData> data,
                                              std::int64_t offset,
                                              double value,
                                              ByteOrder order = ByteOrder::Big);

}  // namespace vela
