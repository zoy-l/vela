#pragma once

#include <cstddef>
#include <expected>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/native_abi.h"
#include "runtime/native_image.h"
#include "runtime/native_program_state.h"
#include "runtime/value.h"

namespace vela::runtime::internal {

// 调用方拥有实例借用及发布保护域。初始化器返回存储表示，不能套用导出结果的 enum 装箱。
inline std::expected<Value, ExecutionStatus> InitializeNativeGlobal(
    NativeExecution& execution,
    NativeEntry entry,
    NativeProgramState::GlobalState& state) {
  using State = NativeProgramState::GlobalState;
  if (state == State::Initializing) {
    return std::unexpected(ExecutionStatus::CyclicInitialization);
  }
  state = State::Initializing;
  NativeValue result;
  const auto status = static_cast<ExecutionStatus>(entry(&execution, nullptr, 0, &result));
  if (status != ExecutionStatus::Returned) {
    state = State::Uninitialized;
    return std::unexpected(status);
  }
  return DecodeNativeValue(result);
}

inline void StoreNativeGlobal(Heap& heap,
                              HeapObject& storage,
                              std::size_t index,
                              NativeProgramState::GlobalState& state,
                              Value value) noexcept {
  heap.SetValueSlot(storage, index, value);
  state = NativeProgramState::GlobalState::Ready;
}

}  // namespace vela::runtime::internal
