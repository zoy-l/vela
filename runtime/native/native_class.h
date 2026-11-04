#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/native_binding.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_type.h"

namespace vela::runtime {

class ClassLayout;

struct NativeClassMember final {
  NativeMemberKind kind = NativeMemberKind::Method;
  std::string name;
  std::vector<std::string> parameter_names;
  NativeFunction function;
};

struct NativeClass final {
  NativeType type;
  std::size_t size = 0;
  std::size_t alignment = 0;
  std::vector<NativeClassMember> members;
  std::string contract;

  template <NativeHostClass T>
  [[nodiscard]] static NativeClass Define(std::vector<NativeClassMember>&& members) {
    static_assert(std::is_nothrow_destructible_v<T>);
    return {
        .type = kNativeHostType<T>,
        .size = sizeof(T),
        .alignment = alignof(T),
        .members = std::move(members),
        .contract = {},
    };
  }
};

struct NativeClassBinding final {
  std::shared_ptr<const NativeClass> description;
  ClassLayout* layout = nullptr;
};

}  // namespace vela::runtime
