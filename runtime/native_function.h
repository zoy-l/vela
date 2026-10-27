#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class Future;
class Completer;
class CancellationToken;

enum class NativeType : std::uint8_t {
  Void,
  Boolean,
  Integer,
  Double,
  String,
  NullableString,
  Future,
  CancellationToken,
};
enum class NativeErrorKind : std::uint8_t { Failure, TypeMismatch, OutOfMemory, InvalidUtf8 };

struct NativeError final {
  NativeErrorKind kind = NativeErrorKind::Failure;
  std::string message;
};

using NativeResult = std::expected<Value, NativeError>;

class NativeContext final {
 public:
  explicit NativeContext(Isolate& isolate) noexcept : isolate_(&isolate) {}
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] NativeResult String(std::string_view text) const;
  [[nodiscard]] std::expected<Value, NativeError> FutureValue(const Future& future) const;
  [[nodiscard]] Completer CreateCompleter() const;
  [[nodiscard]] NativeResult CancellationValue(const CancellationToken& token) const;

 private:
  Isolate* isolate_;
};

namespace internal {

template <class T>
struct NativeCodec;

template <>
struct NativeCodec<std::int64_t> final {
  static constexpr auto kType = NativeType::Integer;
  static std::int64_t Decode(Value value) { return value.AsInteger(); }
  static NativeResult Encode(NativeContext& /*unused*/, std::int64_t value) {
    return Value::Integer(value);
  }
};

template <>
struct NativeCodec<double> final {
  static constexpr auto kType = NativeType::Double;
  static double Decode(Value value) { return value.AsDouble(); }
  static NativeResult Encode(NativeContext& /*unused*/, double value) {
    return Value::Double(value);
  }
};

template <>
struct NativeCodec<bool> final {
  static constexpr auto kType = NativeType::Boolean;
  static bool Decode(Value value) { return value.AsBoolean(); }
  static NativeResult Encode(NativeContext& /*unused*/, bool value) {
    return Value::Boolean(value);
  }
};

template <>
struct NativeCodec<std::string_view> final {
  static constexpr auto kType = NativeType::String;
  static std::string_view Decode(Value value);
  static NativeResult Encode(NativeContext& context, std::string_view text) {
    return context.String(text);
  }
};

template <>
struct NativeCodec<std::string> final {
  static constexpr auto kType = NativeType::String;
  static NativeResult Encode(NativeContext& context, const std::string& text) {
    return context.String(text);
  }
};

template <>
struct NativeCodec<void> final {
  static constexpr auto kType = NativeType::Void;
};

template <>
struct NativeCodec<std::optional<std::string>> final {
  static constexpr auto kType = NativeType::NullableString;
  static NativeResult Encode(NativeContext& context, const std::optional<std::string>& text) {
    return text ? context.String(*text) : NativeResult(Value::Null());
  }
};

template <>
struct NativeCodec<Future> final {
  static constexpr auto kType = NativeType::Future;
  static Future Decode(Value value);
  static NativeResult Encode(NativeContext& context, const Future& future);
};

template <>
struct NativeCodec<CancellationToken> final {
  static constexpr auto kType = NativeType::CancellationToken;
  static CancellationToken Decode(Value value);
  static NativeResult Encode(NativeContext& context, const CancellationToken& token);
};

template <class T>
struct NativeReturn {
  using Type = T;
};

template <class T>
struct NativeReturn<std::expected<T, NativeError>> {
  using Type = T;
};

template <auto Function>
struct NativeAdapter;

template <class R, class... Args, R (*Function)(NativeContext&, Args...) noexcept>
struct NativeAdapter<Function> final {
  using Result = NativeReturn<R>::Type;

  template <std::size_t... Indices>
  static NativeResult Invoke(NativeContext& context,
                             std::span<const Value> values,
                             std::index_sequence<Indices...> /*unused*/) {
    if constexpr (std::is_void_v<R>) {
      Function(context, NativeCodec<Args>::Decode(values.subspan(Indices).front())...);
      return Value::Null();
    } else {
      auto result =
          Function(context, NativeCodec<Args>::Decode(values.subspan(Indices).front())...);
      if constexpr (std::is_same_v<R, std::expected<Result, NativeError>>) {
        if (!result) {
          return std::unexpected(std::move(result.error()));
        }
        if constexpr (std::is_void_v<Result>) {
          return Value::Null();
        } else {
          return NativeCodec<Result>::Encode(context, *result);
        }
      } else {
        return NativeCodec<Result>::Encode(context, result);
      }
    }
  }

  static NativeResult Call(NativeContext& context, std::span<const Value> values) {
    return Invoke(context, values, std::index_sequence_for<Args...>());
  }
};

}  // namespace internal

// 构造器由精确 C++ 函数签名生成；不接受 int/float 隐式缩窄或未检查的 Value 回调。
class NativeFunction final {
 public:
  template <auto Function>
  [[nodiscard]] static NativeFunction Bind(std::string name) {
    return BindSignature<Function>(std::move(name), Function);
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::span<const NativeType> parameters() const noexcept { return parameters_; }
  [[nodiscard]] NativeType result_type() const noexcept { return result_type_; }
  // 参数借用调用帧的根槽；返回值必须在下一次分配之前写入根槽或 RootedValue。
  [[nodiscard]] NativeResult Invoke(Isolate& isolate, std::span<const Value> arguments) const;

 private:
  template <auto Function, class R, class... Args>
  static NativeFunction BindSignature(std::string name,
                                      R (* /*unused*/)(NativeContext&, Args...) noexcept) {
    using Adapter = internal::NativeAdapter<Function>;
    return NativeFunction(std::move(name), {internal::NativeCodec<Args>::kType...},
                          internal::NativeCodec<typename Adapter::Result>::kType, &Adapter::Call);
  }

  using Callback = NativeResult (*)(NativeContext&, std::span<const Value>);
  NativeFunction(std::string name,
                 std::vector<NativeType> parameters,
                 NativeType result_type,
                 Callback callback)
      : name_(std::move(name)),
        parameters_(std::move(parameters)),
        result_type_(result_type),
        callback_(callback) {}
  std::string name_;
  std::vector<NativeType> parameters_;
  NativeType result_type_;
  Callback callback_;
};

enum class NativeRegistrationError : std::uint8_t { InvalidName, DuplicateName };

}  // namespace vela::runtime
