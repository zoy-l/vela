#pragma once

#include <cstdint>

#include "runtime/native/native_abi.h"
#include "runtime/value/object.h"

namespace vela::runtime {

class StringObject;

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
extern "C" HeapObject* vela_native_v10_new_object(NativeExecution* execution,
                                                  std::uint32_t index) noexcept;
extern "C" std::uint8_t vela_native_v10_new_callable(NativeExecution* execution,
                                                     std::uint32_t index,
                                                     NativeValue* result) noexcept;
extern "C" HeapObject* vela_native_v10_new_array(NativeExecution* execution,
                                                 std::uint32_t index,
                                                 std::uint64_t length) noexcept;
extern "C" std::uint8_t vela_native_v1_new_collection(NativeExecution* execution,
                                                      std::uint32_t index,
                                                      NativeValue* result) noexcept;
extern "C" void vela_native_v1_write_barrier(NativeExecution* execution,
                                             HeapObject* object,
                                             const NativeValue* value) noexcept;
extern "C" std::uint8_t vela_native_v1_builtin(NativeExecution* execution,
                                               std::uint32_t operation,
                                               const NativeValue* arguments,
                                               NativeValue* result);
extern "C" std::uint64_t vela_native_v9_scalar_text_length(const NativeValue* value);
extern "C" StringObject* vela_native_v10_scalar_to_string(NativeExecution* execution,
                                                          const NativeValue* value) noexcept;
// 类型检查与绑定保证 Map 及键/值的固定布局；动态调用继续在通用 builtin 边界校验。
extern "C" void vela_native_v11_map_get(HeapObject* map,
                                        const NativeValue* key,
                                        NativeValue* result) noexcept;
extern "C" std::uint8_t vela_native_v11_map_set(NativeExecution* execution,
                                                HeapObject* map,
                                                const NativeValue* key,
                                                const NativeValue* value) noexcept;
extern "C" std::uint8_t vela_native_v1_concat(NativeExecution* execution,
                                              const NativeValue* left,
                                              const NativeValue* right,
                                              NativeValue* result);
extern "C" std::uint8_t vela_native_v1_interpolate(NativeExecution* execution,
                                                   const NativeValue* arguments,
                                                   std::uint64_t count,
                                                   NativeValue* result);
extern "C" std::uint8_t vela_native_v2_compare_strings(const NativeValue* left,
                                                       const NativeValue* right) noexcept;
extern "C" std::uint8_t vela_native_v1_equal(const NativeValue* left,
                                             const NativeValue* right) noexcept;
extern "C" void vela_native_v1_print(NativeExecution* execution, const NativeValue* value);

}  // namespace vela::runtime
