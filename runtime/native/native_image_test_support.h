#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "runtime/heap/heap.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/value.h"

namespace vela::runtime::test {

// 映像表只借用测试拥有的存储；调用方覆盖实例和捕获版本的整个使用期。
inline NativeText Text(std::string_view value) {
  return {.data = value.data(), .size = value.size()};
}

template <class T, std::size_t Size>
NativeTable<T> Table(const std::array<T, Size>& values) {
  return {.data = values.data(), .size = values.size()};
}

// 借用实例已保根的常量池；实例必须覆盖返回视图的使用期。
inline std::span<const Value> InstanceConstants(const NativeInstance& instance) {
  return Heap::ValueSlots(*instance.program().values[NativeProgramState::kConstants].AsObject());
}

}  // namespace vela::runtime::test
