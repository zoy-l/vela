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
enum class NativeGlobalFrame : std::uint8_t { Current, Leaf };

// Ready 槽由生成代码读取；叶函数调用者需要在初始化期间临时登记逻辑帧。
extern "C" std::uint8_t vela_native_v2_initialize_global(NativeExecution* execution,
                                                         std::uint32_t index,
                                                         std::uint64_t position,
                                                         NativeGlobalFrame frame_kind,
                                                         NativeValue* result);
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
