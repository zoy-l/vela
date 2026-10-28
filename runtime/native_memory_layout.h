#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "runtime/array_storage.h"
#include "runtime/native_abi.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

// 编译器和运行时使用同一组 C++ 布局事实；生成代码不得复制对象头或猜标准库布局。
// NativeRuntimeAbiIdentity 已包含这些构建输入，旧产物不能与改变布局的运行时混用。
struct NativeMemoryLayout final {
  static constexpr auto kValuePayload = offsetof(Value, payload_);
  static constexpr auto kValueKind = offsetof(Value, kind_);
  static constexpr auto kObjectLength = offsetof(HeapObject, length);
  static constexpr auto kArrayData = sizeof(HeapObject) + offsetof(internal::ArrayStorage, data);
  static constexpr auto kArrayCapacity =
      sizeof(HeapObject) + offsetof(internal::ArrayStorage, capacity);
  static constexpr auto kArrayMutation =
      sizeof(HeapObject) + offsetof(internal::ArrayStorage, mutation);
  static constexpr auto kFrameInstruction = offsetof(NativeFrameAccess, instruction);
  static constexpr auto kFrameRoots = offsetof(NativeFrameAccess, roots);
  static constexpr auto kFrameStorage = offsetof(NativeFrameAccess, storage);
};

static_assert(std::is_standard_layout_v<HeapObject>);
static_assert(std::is_standard_layout_v<internal::ArrayStorage>);
static_assert(std::is_standard_layout_v<NativeFrameAccess>);
static_assert(NativeMemoryLayout::kValuePayload == offsetof(NativeValue, payload));
static_assert(NativeMemoryLayout::kValueKind == offsetof(NativeValue, kind));
static_assert(sizeof(Value) == sizeof(NativeValue));
static_assert(static_cast<std::uint8_t>(ValueKind::Null) ==
              static_cast<std::uint8_t>(NativeValueKind::Null));
static_assert(static_cast<std::uint8_t>(ValueKind::Boolean) ==
              static_cast<std::uint8_t>(NativeValueKind::Boolean));
static_assert(static_cast<std::uint8_t>(ValueKind::Integer) ==
              static_cast<std::uint8_t>(NativeValueKind::Integer));
static_assert(static_cast<std::uint8_t>(ValueKind::Double) ==
              static_cast<std::uint8_t>(NativeValueKind::Double));
static_assert(static_cast<std::uint8_t>(ValueKind::Object) ==
              static_cast<std::uint8_t>(NativeValueKind::Object));
static_assert(static_cast<std::uint8_t>(ValueKind::String) ==
              static_cast<std::uint8_t>(NativeValueKind::String));

}  // namespace vela::runtime
