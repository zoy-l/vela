#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace vela {

template <class T>
class Local;
template <class T>
class Type;

struct String;
struct MutableString;
struct ByteArray;
struct ByteData;
struct RegExp;
struct RegExpMatch;
struct Timer;
struct CancellationToken;
struct Value;

template <class T>
struct Array;
template <class K, class V>
struct Map;
template <class T>
struct Set;
template <class Signature>
struct Function;
template <class T>
struct Future;
template <class T>
struct Completer;
template <class T>
struct Nullable;
template <class Tag>
struct Object;
template <class Tag>
struct Enum;
template <class T>
struct Host;

enum class ScriptTypeKind : std::uint8_t { Class, Interface, Enum, Host };
enum class Insertion : std::uint8_t { Inserted, Existing };
enum class Removal : std::uint8_t { Removed, Missing };
enum class WaitMode : std::uint8_t { AvailableWork, WaitForHost };

// 脚本公共头文件专门化此契约，以 kModule、kName 和 Arguments 描述源码名义身份。
template <class Tag>
struct ScriptTypeTraits;

namespace internal {

template <class T>
struct LogicalType {
  static constexpr bool kValid =
      std::is_same_v<T, void> || std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
      std::is_same_v<T, double> || std::is_same_v<T, String> || std::is_same_v<T, MutableString> ||
      std::is_same_v<T, ByteArray> || std::is_same_v<T, ByteData> || std::is_same_v<T, RegExp> ||
      std::is_same_v<T, RegExpMatch> || std::is_same_v<T, Timer> ||
      std::is_same_v<T, CancellationToken> || std::is_same_v<T, Value>;
};

template <class T>
inline constexpr bool kValueLogicalType = LogicalType<T>::kValid && !std::is_void_v<T>;

template <class T>
struct LogicalType<Array<T>> {
  static constexpr bool kValid = kValueLogicalType<T>;
};
template <class K, class V>
struct LogicalType<Map<K, V>> {
  static constexpr bool kValid = kValueLogicalType<K> && kValueLogicalType<V>;
};
template <class T>
struct LogicalType<Set<T>> {
  static constexpr bool kValid = kValueLogicalType<T>;
};
template <class T>
struct LogicalType<Nullable<T>> {
  static constexpr bool kValid = kValueLogicalType<T>;
};
template <class R, class... Args>
struct LogicalType<Function<R(Args...)>> {
  static constexpr bool kValid = LogicalType<R>::kValid && (kValueLogicalType<Args> && ...);
};
template <class T>
struct LogicalType<Future<T>> : LogicalType<T> {};
template <class T>
struct LogicalType<Completer<T>> : LogicalType<T> {};
template <class Tag>
struct LogicalType<Object<Tag>> : std::true_type {
  static constexpr bool kValid = true;
};
template <class Tag>
struct LogicalType<Enum<Tag>> : std::true_type {
  static constexpr bool kValid = true;
};
template <class T>
struct LogicalType<Host<T>> : std::true_type {
  static constexpr bool kValid = true;
};

template <class T>
struct HostValueType {
  static_assert(
      LogicalType<T>::kValid,
      "expected a Vela logical type; nested Local/Persistent and C++ containers are invalid");
  using Type = Local<T>;
  static constexpr bool kManaged = true;
};
template <>
struct HostValueType<void> {
  using Type = void;
  static constexpr bool kManaged = false;
};
template <>
struct HostValueType<bool> {
  using Type = bool;
  static constexpr bool kManaged = false;
};
template <>
struct HostValueType<std::int64_t> {
  using Type = std::int64_t;
  static constexpr bool kManaged = false;
};
template <>
struct HostValueType<double> {
  using Type = double;
  static constexpr bool kManaged = false;
};
template <class T>
struct HostValueType<Nullable<T>> {
  static_assert(LogicalType<Nullable<T>>::kValid, "nullable payload must be a Vela value type");
  using Type = std::optional<typename HostValueType<T>::Type>;
  using Payload = T;
  static constexpr bool kManaged = HostValueType<T>::kManaged;
};
template <class T>
struct HostValueType<Nullable<Nullable<T>>> : HostValueType<Nullable<T>> {};

template <class T>
struct ArrayType {
  static constexpr bool kIsArray = false;
};
template <class T>
struct ArrayType<Array<T>> {
  using Element = T;
  static constexpr bool kIsArray = true;
};

template <class T>
struct CollectionType {
  static constexpr bool kIsCollection = false;
  static constexpr bool kIsMap = false;
};
template <class K, class V>
struct CollectionType<Map<K, V>> {
  using Key = K;
  using Mapped = V;
  static constexpr bool kIsCollection = true;
  static constexpr bool kIsMap = true;
};
template <class T>
struct CollectionType<Set<T>> {
  using Key = T;
  static constexpr bool kIsCollection = true;
  static constexpr bool kIsMap = false;
};

template <class T>
struct FunctionType;
template <class R, class... Args>
struct FunctionType<Function<R(Args...)>> {
  using Return = R;
};

template <class T>
struct AsyncType {
  static constexpr bool kIsFuture = false;
  static constexpr bool kIsCompleter = false;
};
template <class T>
struct AsyncType<Future<T>> {
  using Payload = T;
  static constexpr bool kIsFuture = true;
  static constexpr bool kIsCompleter = false;
};
template <class T>
struct AsyncType<Completer<T>> {
  using Payload = T;
  static constexpr bool kIsFuture = false;
  static constexpr bool kIsCompleter = true;
};

}  // namespace internal

template <class T>
using HostValue = internal::HostValueType<T>::Type;

template <class T>
struct NamedArgument final {
  std::string_view name;
  std::optional<HostValue<T>> value;
};

}  // namespace vela
