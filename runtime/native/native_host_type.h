#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>

#include "runtime/native/native_type.h"

namespace vela::runtime {

// 由生成的绑定头专门化；标记头本身不依赖 runtime。
template <class T>
struct NativeClassTraits;

namespace internal {

template <class T>
inline constexpr std::byte kNativeClassIdentity{};

}  // namespace internal

template <class T>
concept NativeHostClass =
    std::is_class_v<std::remove_cv_t<T>> && !std::is_volatile_v<T> &&
    requires { std::string_view(NativeClassTraits<std::remove_cv_t<T>>::kKey); };

template <NativeHostClass T>
inline constexpr NativeType kNativeHostType{
    .kind = NativeKind::Host,
    .host_key = NativeClassTraits<std::remove_cv_t<T>>::kKey,
    .cpp_identity = &internal::kNativeClassIdentity<std::remove_cv_t<T>>,
};

template <NativeHostClass T>
inline constexpr NativeType kNativeNullableHostType = [] {
  auto type = kNativeHostType<T>;
  type.nullable = true;
  return type;
}();

}  // namespace vela::runtime
