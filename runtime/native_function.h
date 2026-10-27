#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/register_storage.h"
#include "runtime/value.h"

namespace vela::runtime {

class Isolate;
class NativeFrame;
class NativeContext;
extern "C" std::uint8_t vela_native_frame_enter(NativeContext* context,
                                                std::uint64_t root_count) noexcept;
extern "C" void vela_native_frame_exit(NativeContext* context) noexcept;
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
  explicit NativeContext(Isolate& isolate) noexcept;
  ~NativeContext();

  NativeContext(const NativeContext&) = delete;
  NativeContext& operator=(const NativeContext&) = delete;
  NativeContext(NativeContext&&) = delete;
  NativeContext& operator=(NativeContext&&) = delete;
  [[nodiscard]] Isolate& isolate() const noexcept { return *isolate_; }
  [[nodiscard]] std::size_t frame_count() const noexcept;
  [[nodiscard]] std::span<Value> current_roots() noexcept;
  [[nodiscard]] NativeResult String(std::string_view text) const;
  [[nodiscard]] std::expected<Value, NativeError> FutureValue(const Future& future) const;
  [[nodiscard]] Completer CreateCompleter() const;
  [[nodiscard]] NativeResult CancellationValue(const CancellationToken& token) const;

 private:
  friend class NativeFrame;
  friend std::uint8_t vela_native_frame_enter(NativeContext* context,
                                              std::uint64_t root_count) noexcept;
  friend void vela_native_frame_exit(NativeContext* context) noexcept;
  Isolate* isolate_;
  std::vector<std::unique_ptr<NativeFrame>> frames_;
};

namespace internal {

template <class T>
struct NativeCodec;

template <>
struct NativeCodec<std::int64_t> final {
  static constexpr auto kType = NativeType::Integer;
  static bool IsCompatible(RegisterReference value) { return value.IsInteger(); }
  static std::int64_t Decode(RegisterReference value) { return value.AsInteger(); }
  static NativeResult Encode(NativeContext& /*unused*/, std::int64_t value) {
    return Value::Integer(value);
  }
};

template <>
struct NativeCodec<double> final {
  static constexpr auto kType = NativeType::Double;
  static bool IsCompatible(RegisterReference value) { return value.IsDouble(); }
  static double Decode(RegisterReference value) { return value.AsDouble(); }
  static NativeResult Encode(NativeContext& /*unused*/, double value) {
    return Value::Double(value);
  }
};

template <>
struct NativeCodec<bool> final {
  static constexpr auto kType = NativeType::Boolean;
  static bool IsCompatible(RegisterReference value) { return value.IsBoolean(); }
  static bool Decode(RegisterReference value) { return value.AsBoolean(); }
  static NativeResult Encode(NativeContext& /*unused*/, bool value) {
    return Value::Boolean(value);
  }
};

template <>
struct NativeCodec<std::string_view> final {
  static constexpr auto kType = NativeType::String;
  static bool IsCompatible(RegisterReference value) { return value.IsString(); }
  static std::string_view Decode(RegisterReference value);
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
  static bool IsCompatible(RegisterReference value);
  static Future Decode(RegisterReference value);
  static NativeResult Encode(NativeContext& context, const Future& future);
};

template <>
struct NativeCodec<CancellationToken> final {
  static constexpr auto kType = NativeType::CancellationToken;
  static bool IsCompatible(RegisterReference value);
  static CancellationToken Decode(RegisterReference value);
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
                             RegisterView values,
                             std::index_sequence<Indices...> /*unused*/) {
    // Program 验证实参数量；载荷类型仍须在宿主边界检查，原始字节码不保证类型流正确。
    if (!(NativeCodec<Args>::IsCompatible(values.subspan(Indices).front()) && ...)) {
      return std::unexpected(NativeError{
          .kind = NativeErrorKind::TypeMismatch,
          .message = "native argument type mismatch",
      });
    }
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

  static NativeResult Call(NativeContext& context, RegisterView values) {
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
  // 实参数量须等于 parameters().size()，参数窗口须由调用者持续保根。
  // 不复制参数或重复注册根；返回引用须在下一次分配前写入根槽或 RootedValue。
  [[nodiscard]] NativeResult Invoke(Isolate& isolate, RegisterView arguments) const;

 private:
  template <auto Function, class R, class... Args>
  static NativeFunction BindSignature(std::string name,
                                      R (* /*unused*/)(NativeContext&, Args...) noexcept) {
    using Adapter = internal::NativeAdapter<Function>;
    return NativeFunction(std::move(name), {internal::NativeCodec<Args>::kType...},
                          internal::NativeCodec<typename Adapter::Result>::kType, &Adapter::Call);
  }

  using Callback = NativeResult (*)(NativeContext&, RegisterView);
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
