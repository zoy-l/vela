#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/native_abi.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

class NativeSuspendedFrame;
// 固定 ABI 视图；存储由整次 async 激活持有，恢复调用只借用完成结果。
struct NativeResumeState final {
  Value* saved = nullptr;
  const FutureResult* completion = nullptr;
  NativeSuspendedFrame* frame = nullptr;
  std::uint32_t instruction = 0;
};
using NativeResumeEntry = std::uint8_t (*)(NativeExecution*, NativeResumeState*) noexcept;

[[nodiscard]] std::expected<Value, ExecutionStatus> ExecuteNativeDynamicAsync(
    NativeExecution& execution,
    std::span<const Value> arguments);

extern "C" std::uint8_t vela_native_v1_new_future(NativeExecution* execution,
                                                  const NativeValue* payload,
                                                  std::uint8_t error,
                                                  NativeValue* result);
extern "C" std::uint8_t vela_native_v1_sleep(NativeExecution* execution,
                                             std::int64_t milliseconds,
                                             NativeValue* result);
extern "C" std::uint8_t vela_native_v1_async_call(NativeExecution* execution,
                                                  std::uint32_t operation,
                                                  const NativeValue* arguments,
                                                  NativeValue* result);
extern "C" std::uint8_t vela_native_v1_async_return(NativeExecution* execution,
                                                    std::uint32_t function,
                                                    const NativeValue* payload,
                                                    NativeValue* result);
extern "C" std::uint8_t vela_native_v1_async_error(NativeExecution* execution,
                                                   std::uint32_t function,
                                                   NativeValue* result);
extern "C" std::uint8_t vela_native_v1_suspend(NativeExecution* execution,
                                               std::uint32_t function,
                                               NativeResumeEntry entry,
                                               std::uint32_t instruction,
                                               const NativeValue* source,
                                               const NativeValue* saved,
                                               std::uint64_t count,
                                               std::uint64_t capacity,
                                               NativeValue* result);
extern "C" void vela_native_v1_resuspend(NativeResumeState* state,
                                         std::uint32_t instruction,
                                         const NativeValue* source,
                                         const NativeValue* saved,
                                         std::uint64_t count);
extern "C" std::uint8_t vela_native_v1_resume_return(NativeExecution* execution,
                                                     NativeResumeState* state,
                                                     std::uint32_t function,
                                                     const NativeValue* payload);
extern "C" std::uint8_t vela_native_v1_resume_outcome(NativeExecution* execution,
                                                      NativeResumeState* state,
                                                      NativeValue* result);

}  // namespace vela::runtime
