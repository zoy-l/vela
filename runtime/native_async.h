#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/native_abi.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

struct NativeResumeState;
using NativeResumeEntry = std::uint8_t (*)(NativeExecution*,
                                           NativeResumeState*,
                                           HeapObject**) noexcept;

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
                                               NativeValue* result);
extern "C" std::uint32_t vela_native_v1_resume_instruction(NativeResumeState* state) noexcept;
extern "C" void vela_native_v1_resume_value(NativeResumeState* state,
                                            std::uint64_t index,
                                            NativeValue* result) noexcept;
extern "C" void vela_native_v1_resume_release(NativeResumeState* state) noexcept;
extern "C" std::uint8_t vela_native_v1_resume_outcome(NativeExecution* execution,
                                                      NativeResumeState* state,
                                                      NativeValue* result);

}  // namespace vela::runtime
