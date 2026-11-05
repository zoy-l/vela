#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <vela/types.h>
#include "base/layout_description.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_type.h"

namespace vela::runtime::internal {

struct NativeTypeDescription final {
  layout::Program layouts;
  std::optional<layout::Type> type = std::nullopt;
  std::vector<NativeHostIdentity> hosts;

  [[nodiscard]] NativeType View() const noexcept {
    return {.type = type, .description = &layouts, .hosts = hosts};
  }
};

[[nodiscard]] NativeTypeDescription DescribeNativeArray(NativeType element);
[[nodiscard]] NativeTypeDescription DescribeNativeCollection(NativeType key,
                                                             std::optional<NativeType> value);
[[nodiscard]] NativeTypeDescription DescribeNativeSignature(layout::Type::Kind kind,
                                                            NativeType result,
                                                            std::span<const NativeType> parameters);
[[nodiscard]] NativeTypeDescription DescribeNativeNominal(std::string_view module,
                                                          std::string_view name,
                                                          ScriptTypeKind kind,
                                                          std::span<const NativeType> arguments);
[[nodiscard]] NativeTypeDescription DescribeNativeHost(std::string_view key);

template <class T>
struct NativeLogicalType;

template <class T>
[[nodiscard]] NativeType NativeTypeOf() {
  return NativeLogicalType<T>::Get();
}

template <layout::Type::Kind Kind>
struct NativeLeafType {
  [[nodiscard]] static constexpr NativeType Get() {
    return {.type = layout::Type{.kind = Kind}, .hosts = {}};
  }
};

template <>
struct NativeLogicalType<void> final {
  [[nodiscard]] static constexpr NativeType Get() { return {.hosts = {}}; }
};
template <>
struct NativeLogicalType<bool> final : NativeLeafType<layout::Type::Kind::Boolean> {};
template <>
struct NativeLogicalType<std::int64_t> final : NativeLeafType<layout::Type::Kind::Integer> {};
template <>
struct NativeLogicalType<double> final : NativeLeafType<layout::Type::Kind::Double> {};
template <>
struct NativeLogicalType<String> final : NativeLeafType<layout::Type::Kind::String> {};
template <>
struct NativeLogicalType<MutableString> final : NativeLeafType<layout::Type::Kind::MutableString> {
};
template <>
struct NativeLogicalType<ByteArray> final : NativeLeafType<layout::Type::Kind::ByteArray> {};
template <>
struct NativeLogicalType<ByteData> final : NativeLeafType<layout::Type::Kind::ByteData> {};
template <>
struct NativeLogicalType<RegExp> final : NativeLeafType<layout::Type::Kind::RegExp> {};
template <>
struct NativeLogicalType<RegExpMatch> final : NativeLeafType<layout::Type::Kind::RegExpMatch> {};
template <>
struct NativeLogicalType<vela::Timer> final : NativeLeafType<layout::Type::Kind::Timer> {};
template <>
struct NativeLogicalType<vela::CancellationToken> final
    : NativeLeafType<layout::Type::Kind::CancellationToken> {};
template <>
struct NativeLogicalType<vela::Value> final : NativeLeafType<layout::Type::Kind::Dynamic> {};

template <class T>
struct NativeLogicalType<Nullable<T>> final {
  [[nodiscard]] static NativeType Get() {
    auto type = NativeTypeOf<T>();
    static_assert(!std::is_void_v<T>, "void cannot be nullable");
    assert(type.type.has_value());
    if (type.type->kind != layout::Type::Kind::Dynamic) {
      type.type->nullable = true;
    }
    return type;
  }
};

template <class T>
struct NativeLogicalType<Array<T>> final {
  [[nodiscard]] static NativeType Get() {
    static_assert(!std::is_void_v<T>, "array elements cannot be void");
    static const auto kDescription = DescribeNativeArray(NativeTypeOf<T>());
    return kDescription.View();
  }
};

template <class K, class V>
struct NativeLogicalType<Map<K, V>> final {
  [[nodiscard]] static NativeType Get() {
    static_assert(!std::is_void_v<K> && !std::is_void_v<V>, "map entries cannot be void");
    static const auto kDescription = DescribeNativeCollection(NativeTypeOf<K>(), NativeTypeOf<V>());
    return kDescription.View();
  }
};

template <class T>
struct NativeLogicalType<Set<T>> final {
  [[nodiscard]] static NativeType Get() {
    static_assert(!std::is_void_v<T>, "set elements cannot be void");
    static const auto kDescription = DescribeNativeCollection(NativeTypeOf<T>(), std::nullopt);
    return kDescription.View();
  }
};

template <class R, class... Args>
struct NativeLogicalType<Function<R(Args...)>> final {
  [[nodiscard]] static NativeType Get() {
    static const auto kDescription =
        DescribeNativeSignature(layout::Type::Kind::Callable, NativeTypeOf<R>(),
                                std::array<NativeType, sizeof...(Args)>{NativeTypeOf<Args>()...});
    return kDescription.View();
  }
};

template <class T>
struct NativeLogicalType<vela::Future<T>> final {
  [[nodiscard]] static NativeType Get() {
    static const auto kDescription =
        DescribeNativeSignature(layout::Type::Kind::Future, NativeTypeOf<T>(), {});
    return kDescription.View();
  }
};

template <class T>
struct NativeLogicalType<vela::Completer<T>> final {
  [[nodiscard]] static NativeType Get() {
    static const auto kDescription =
        DescribeNativeSignature(layout::Type::Kind::Completer, NativeTypeOf<T>(), {});
    return kDescription.View();
  }
};

template <class Tag>
struct NativeLogicalType<Object<Tag>> final {
  [[nodiscard]] static NativeType Get() {
    using Traits = ScriptTypeTraits<Tag>;
    static_assert(Traits::kKind != ScriptTypeKind::Enum, "use Enum<Tag> for enum declarations");
    static const auto kDescription = []<class... Args>(std::tuple<Args...>*) {
      return DescribeNativeNominal(
          Traits::kModule, Traits::kName, Traits::kKind,
          std::array<NativeType, sizeof...(Args)>{NativeTypeOf<Args>()...});
    }(static_cast<Traits::Arguments*>(nullptr));
    return kDescription.View();
  }
};

template <class Tag>
struct NativeLogicalType<Enum<Tag>> final {
  [[nodiscard]] static NativeType Get() {
    using Traits = ScriptTypeTraits<Tag>;
    static_assert(Traits::kKind == ScriptTypeKind::Enum, "Enum<Tag> requires an enum declaration");
    static const auto kDescription = []<class... Args>(std::tuple<Args...>*) {
      return DescribeNativeNominal(
          Traits::kModule, Traits::kName, Traits::kKind,
          std::array<NativeType, sizeof...(Args)>{NativeTypeOf<Args>()...});
    }(static_cast<Traits::Arguments*>(nullptr));
    return kDescription.View();
  }
};

template <class T>
struct NativeLogicalType<Host<T>> final {
  [[nodiscard]] static NativeType Get() {
    if constexpr (NativeHostClass<T>) {
      return NativeHostType<T>();
    } else {
      using Traits = ScriptTypeTraits<T>;
      static_assert(Traits::kKind == ScriptTypeKind::Host);
      static const auto kDescription = DescribeNativeHost(Traits::kNativeKey);
      return kDescription.View();
    }
  }
};

}  // namespace vela::runtime::internal
