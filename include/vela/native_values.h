#pragma once

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vela/collections.h>
#include <vela/error.h>
#include <vela/handles.h>
#include <vela/objects.h>  // IWYU pragma: keep
#include <vela/strings.h>
#include <vela/types.h>
#include "runtime/native/handle_access.h"
#include "runtime/native/native_context.h"
#include "runtime/value/object.h"
#include "runtime/value/value_operations.h"

namespace vela::bindings {

// 脚本拥有原生值。可复制值按值传参，move-only 值只消费一次，不复制原生树。
template <class T>
struct ValueBox final {
  template <class... Args>
  explicit ValueBox(std::in_place_t /*tag*/, Args&&... args) noexcept
      : value(std::in_place, std::forward<Args>(args)...) {}
  std::optional<T> value;
};

inline Error ValueError(std::string message) {
  return {.message = std::move(message), .stack_trace = {}};
}

template <class T>
struct ValueCodec;

template <class T>
using ValueLogical = ValueCodec<std::remove_cvref_t<T>>::Logical;

template <class T>
struct ValueCodec {
  using Logical = Host<ValueBox<T>>;
  using Argument = Local<Logical>;
  using Output = runtime::NativeConstructed<ValueBox<T>>;

  static Result<T> Decode(runtime::NativeContext& /*context*/, Argument input) {
    auto& object = *vela::internal::HandleAccess::Read(input).AsObject();
    auto& box = *static_cast<ValueBox<T>*>(runtime::HostObjectPointer(object));
    if (!box.value) {
      return std::unexpected(ValueError("Native value has already been consumed"));
    }
    if constexpr (std::copy_constructible<T>) {
      return *box.value;
    } else {
      auto result = std::move(*box.value);
      box.value.reset();
      return result;
    }
  }

  static Result<Output> Encode(runtime::NativeContext& context, T value) {
    return context.Construct<ValueBox<T>>(std::in_place, std::move(value));
  }

  static Result<Argument> ToLogical(runtime::NativeContext& context, T value) {
    auto encoded = Encode(context, std::move(value));
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    auto published = context.Own(std::move(*encoded));
    if (!published) {
      return std::unexpected(std::move(published.error()));
    }
    return vela::internal::HandleAccess::Root<Logical>(context.handles(), *published);
  }
};

template <class T>
  requires std::is_arithmetic_v<T>
struct ValueCodec<T> {
  using Logical = std::conditional_t<std::is_same_v<T, bool>, bool,
      std::conditional_t<std::is_floating_point_v<T>, double, std::int64_t>>;
  using Argument = Logical;
  using Output = Logical;
  static Result<T> Decode(runtime::NativeContext& /*context*/, Logical value) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
      if (!std::in_range<T>(value)) {
        return std::unexpected(ValueError("Integer is outside the native type's range"));
      }
    } else if constexpr (std::is_same_v<T, float>) {
      if (!std::isfinite(value) || value < std::numeric_limits<float>::lowest() ||
          value > std::numeric_limits<float>::max()) {
        return std::unexpected(ValueError("Number is outside the native float range"));
      }
    }
    return static_cast<T>(value);
  }
  static Result<Logical> Encode(runtime::NativeContext& /*context*/, T value) {
    if constexpr (std::is_unsigned_v<T> && !std::is_same_v<T, bool>) {
      if (!std::in_range<std::int64_t>(value)) {
        return std::unexpected(ValueError("Native integer is outside the Vela int range"));
      }
    }
    return static_cast<Logical>(value);
  }
  static Result<Logical> ToLogical(runtime::NativeContext& context, T value) {
    return Encode(context, value);
  }
};

template <>
struct ValueCodec<std::string> {
  using Logical = String;
  using Argument = std::string_view;
  using Output = std::string;
  static Result<std::string> Decode(runtime::NativeContext& /*context*/, Argument value) { return std::string(value); }
  static Result<Output> Encode(runtime::NativeContext& /*context*/, std::string value) { return value; }
  static Result<Local<String>> ToLogical(runtime::NativeContext& context, std::string_view value) {
    return CreateString(context.handles(), value);
  }
};

template <>
struct ValueCodec<std::string_view> : ValueCodec<std::string> {
  static Result<std::string_view> Decode(runtime::NativeContext& /*context*/, Argument value) { return value; }
};

template <class T>
Result<T> DecodeValue(runtime::NativeContext& context, HostValue<ValueLogical<T>> input) {
  if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::string> ||
                std::is_same_v<std::remove_cvref_t<T>, std::string_view>) {
    return ValueCodec<T>::Decode(context, input.Utf8());
  } else {
    return ValueCodec<T>::Decode(context, input);
  }
}

template <class T>
struct ValueCodec<std::optional<T>> {
  using Logical = Nullable<ValueLogical<T>>;
  using Argument = HostValue<Logical>;
  using Output = Argument;
  static Result<std::optional<T>> Decode(runtime::NativeContext& context, Argument value) {
    if (!value) {
      return std::nullopt;
    }
    auto decoded = DecodeValue<T>(context, *value);
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    return std::optional<T>(std::move(*decoded));
  }
  static Result<Output> Encode(runtime::NativeContext& context, std::optional<T> value) {
    if (!value) {
      return std::nullopt;
    }
    auto encoded = ValueCodec<T>::ToLogical(context, std::move(*value));
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    return Output(std::move(*encoded));
  }
  static Result<Output> ToLogical(runtime::NativeContext& context, std::optional<T> value) {
    return Encode(context, std::move(value));
  }
};

template <class T, class Allocator>
struct ValueCodec<std::vector<T, Allocator>> {
  using Logical = Array<ValueLogical<T>>;
  using Argument = Local<Logical>;
  using Output = Argument;
  static Result<std::vector<T, Allocator>> Decode(runtime::NativeContext& context, Argument input) {
    std::vector<T, Allocator> result;
    const auto length = input.Size();
    result.reserve(length);
    for (std::int64_t index = 0; index < length; ++index) {
      auto value = input.Get(context.handles(), index);
      if (!value) {
        return std::unexpected(std::move(value.error()));
      }
      auto decoded = DecodeValue<T>(context, *value);
      if (!decoded) {
        return std::unexpected(std::move(decoded.error()));
      }
      result.push_back(std::move(*decoded));
    }
    return result;
  }
  static Result<Output> Encode(runtime::NativeContext& context, std::vector<T, Allocator> values) {
    auto result = CreateArray<ValueLogical<T>>(context.handles());
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    for (auto& value : values) {
      auto encoded = ValueCodec<T>::ToLogical(context, std::move(value));
      if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
      }
      auto pushed = result->Push(context.handles(), *encoded);
      if (!pushed) {
        return std::unexpected(std::move(pushed.error()));
      }
    }
    return *result;
  }
  static Result<Output> ToLogical(runtime::NativeContext& context, std::vector<T, Allocator> values) {
    return Encode(context, std::move(values));
  }
};

// 枚举保留 C++ 值与 Vela ordinal 的独立映射，不假定 C++ 枚举连续。
template <class T>
struct EnumValues;

template <class T>
  requires std::is_enum_v<T>
struct ValueCodec<T> {
  using Logical = Enum<T>;
  using Argument = Local<Logical>;
  using Output = Argument;
  static Result<T> Decode(runtime::NativeContext& /*context*/, Argument input) {
    const auto ordinal = runtime::EnumOrdinal(*vela::internal::HandleAccess::Read(input).AsObject());
    return EnumValues<T>::values[static_cast<std::size_t>(ordinal)].second;
  }
  static Result<Output> Encode(runtime::NativeContext& context, T input) {
    const auto type = context.BindType<Logical>();
    if (!type) {
      return std::unexpected(std::move(type.error()));
    }
    for (const auto& [name, value] : EnumValues<T>::values) {
      if (value == input) {
        return type->Member(context.handles(), name);
      }
    }
    return std::unexpected(ValueError("Native enum has no corresponding Vela member"));
  }
  static Result<Output> ToLogical(runtime::NativeContext& context, T value) { return Encode(context, value); }
};

}  // namespace vela::bindings
