#pragma once

#include <cstdint>
#include <expected>

#include "base/execution_status.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/value.h"

namespace vela::runtime::internal {

// 只在成功时写入 ABI 输出；失败保留原错误类别及调用方的输出槽。
inline std::uint8_t StoreNativeResult(NativeValue* output,
                                      std::expected<Value, ExecutionStatus> result) {
  if (!result) {
    return static_cast<std::uint8_t>(result.error());
  }
  *output = EncodeNativeValue(*result);
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

}  // namespace vela::runtime::internal
