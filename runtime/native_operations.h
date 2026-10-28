#pragma once

#include <cstdint>

#include "runtime/native_abi.h"
#include "runtime/object.h"

namespace vela::runtime {

// 全部指针与编号来自已实例化的映像；借用参数在整个 helper 调用期间由调用帧保根。
extern "C" void vela_native_v1_constant(NativeExecution* execution,
                                        std::uint32_t index,
                                        NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_type_test(NativeExecution* execution,
                                                 std::uint32_t type,
                                                 const NativeValue* value) noexcept;
// Ready 槽由生成代码读取；position 仅在调用者尚未登记逻辑帧时提供。
extern "C" std::uint8_t vela_native_v1_initialize_global(NativeExecution* execution,
                                                         std::uint32_t index,
                                                         const TracePosition* position,
                                                         NativeValue* result);
extern "C" std::uint8_t vela_native_v1_new_expected(NativeExecution* execution,
                                                    std::uint32_t layout,
                                                    const NativeValue* payload,
                                                    std::uint8_t alternative,
                                                    NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_expected_has_value(HeapObject* object,
                                                          NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_expected_value(HeapObject* object,
                                                      std::uint8_t alternative,
                                                      NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_new_instance(NativeExecution* execution,
                                                    std::uint32_t index,
                                                    NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v1_new_array(NativeExecution* execution,
                                                 std::uint32_t index,
                                                 NativeValue* result,
                                                 std::int64_t length) noexcept;
extern "C" std::uint8_t vela_native_v1_new_collection(NativeExecution* execution,
                                                      std::uint32_t index,
                                                      NativeValue* result) noexcept;
extern "C" void vela_native_v1_write_barrier(NativeExecution* execution,
                                             HeapObject* object,
                                             const NativeValue* value) noexcept;
extern "C" std::uint8_t vela_native_v1_array_grow(NativeExecution* execution,
                                                  HeapObject* array,
                                                  const NativeValue* value) noexcept;
extern "C" std::uint8_t vela_native_v1_builtin(NativeExecution* execution,
                                               std::uint32_t operation,
                                               const NativeValue* arguments,
                                               NativeValue* result);
extern "C" std::uint8_t vela_native_v1_concat(NativeExecution* execution,
                                              const NativeValue* left,
                                              const NativeValue* right,
                                              NativeValue* result);
extern "C" std::uint8_t vela_native_v1_interpolate(NativeExecution* execution,
                                                   const NativeValue* arguments,
                                                   std::uint64_t count,
                                                   NativeValue* result);
extern "C" std::uint8_t vela_native_v1_compare_strings(const NativeValue* left,
                                                       const NativeValue* right,
                                                       NativeValue* result);
extern "C" std::uint8_t vela_native_v1_equal(const NativeValue* left,
                                             const NativeValue* right) noexcept;
extern "C" void vela_native_v1_print(NativeExecution* execution, const NativeValue* value);

}  // namespace vela::runtime
