#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/layout_description.h"
#include "runtime/native/native_type.h"

namespace vela::runtime {

// 由生成的绑定头专门化；标记头本身不依赖 runtime。
template <class T>
struct NativeClassTraits;

namespace internal {

template <class T>
inline constexpr std::byte kNativeClassIdentity{};

template <class T>
inline constexpr std::array<NativeHostIdentity, 1> kNativeHostIdentities{
    {
        {
            .key = NativeClassTraits<std::remove_cv_t<T>>::kKey,
            .identity = &kNativeClassIdentity<std::remove_cv_t<T>>,
        },
    },
};

}  // namespace internal

template <class T>
concept NativeHostClass =
    std::is_class_v<std::remove_cv_t<T>> && !std::is_volatile_v<T> &&
    requires { std::string_view(NativeClassTraits<std::remove_cv_t<T>>::kKey); };

template <NativeHostClass T>
[[nodiscard]] NativeType NativeHostType() {
  static const layout::Program kDescription = [] {
    layout::Program program;
    program.classes.push_back({
        .name = {},
        .native_key = std::string(NativeClassTraits<std::remove_cv_t<T>>::kKey),
        .native_contract = {},
        .kind = layout::Class::Kind::Host,
        .reference_fields = {},
        .callable = {},
        .interfaces = {},
        .dispatch_targets = {},
    });
    return program;
  }();
  return {
      .type = layout::Type{.kind = layout::Type::Kind::Instance},
      .description = &kDescription,
      .hosts = internal::kNativeHostIdentities<T>,
  };
}

template <NativeHostClass T>
[[nodiscard]] NativeType NativeNullableHostType() {
  auto type = NativeHostType<T>();
  if (!type.type) {
    std::unreachable();
  }
  type.type->nullable = true;
  return type;
}

}  // namespace vela::runtime
