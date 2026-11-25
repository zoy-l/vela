#pragma once

#include <cstdint>

#include "runtime/heap/heap.h"
#include "runtime/native/native_abi.h"
#include "runtime/value/object.h"

namespace vela::runtime {

class StringObject;

// 全部指针与编号来自已实例化的映像；借用参数在整个 helper 调用期间由调用帧保根。
extern "C" std::uint8_t vela_native_v1_type_test(NativeExecution* execution,
                                                 std::uint32_t type,
                                                 const NativeValue* value) noexcept;
enum class NativeGlobalFrame : std::uint8_t { Current, Leaf };
enum class NativeGlobalIndex : std::uint32_t {};
enum class NativeArrayLayoutIndex : std::uint32_t {};

// Ready 槽由生成代码读取；叶函数调用者需要在初始化期间临时登记逻辑帧。
extern "C" std::uint8_t vela_native_v2_initialize_global(NativeExecution* execution,
                                                         NativeGlobalIndex index,
                                                         std::uint64_t position,
                                                         NativeGlobalFrame frame_kind,
                                                         NativeValue* result);
extern "C" HeapObject* vela_native_v10_new_object(NativeExecution* execution,
                                                  std::uint32_t index) noexcept;
extern "C" std::uint8_t vela_native_v10_new_callable(NativeExecution* execution,
                                                     std::uint32_t index,
                                                     NativeValue* result) noexcept;
extern "C" HeapObject* vela_native_v10_new_array(NativeExecution* execution,
                                                 NativeArrayLayoutIndex index,
                                                 std::uint64_t length) noexcept;
extern "C" std::uint8_t vela_native_v1_push_record(NativeExecution* execution,
                                                   HeapObject* array,
                                                   HeapObject* value,
                                                   NativeValue* result) noexcept;
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
extern "C" std::uint8_t vela_native_v1_numeric_parse(std::uint32_t operation,
                                                     StringObject* source,
                                                     const NativeValue* radix,
                                                     NativeValue* result);
// 生成代码已验证 index <= length；结果始终为合法字节边界，不分配或触发 GC。
extern "C" std::int64_t vela_native_v1_string_byte_offset(StringObject* string,
                                                          std::uint32_t index) noexcept;
extern "C" std::uint8_t vela_native_v1_string_slice_bytes(NativeExecution* execution,
                                                          StringObject* string,
                                                          std::int64_t start,
                                                          std::int64_t end,
                                                          StringObject** result);
extern "C" StringObject* vela_native_v1_string_trim(NativeExecution* execution,
                                                    StringObject* string);
enum class NativeStringAffix : std::uint8_t { Prefix, Suffix };
extern "C" std::uint8_t vela_native_v1_string_affix(StringObject* string,
                                                    StringObject* affix,
                                                    NativeStringAffix kind) noexcept;
extern "C" std::uint8_t vela_native_v12_reflection_invoke(NativeExecution* execution,
                                                          const NativeValue* arguments,
                                                          std::uint64_t count,
                                                          NativeValue* result);
extern "C" std::uint64_t vela_native_v1_integer_text_length(std::int64_t value);
extern "C" std::uint64_t vela_native_v1_double_text_length(double value);
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
extern "C" std::uint8_t vela_native_v1_string_int_map_get(HeapObject* map,
                                                          StringObject* key,
                                                          std::int64_t* result,
                                                          Heap::MapLookup* lookup) noexcept;
extern "C" std::uint8_t vela_native_v1_string_int_map_set(NativeExecution* execution,
                                                          HeapObject* map,
                                                          StringObject* key,
                                                          std::int64_t value,
                                                          const Heap::MapLookup* lookup) noexcept;
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
