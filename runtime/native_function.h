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

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/native_abi.h"
#include "runtime/register_storage.h"
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
  static bool IsCompatible(Value value) { return value.IsInteger(); }
  static std::int64_t Decode(Value value) { return value.AsInteger(); }
  static NativeResult Encode(NativeContext& /*unused*/, std::int64_t value) {
    return Value::Integer(value);
  }
};

template <>
struct NativeCodec<double> final {
  static constexpr auto kType = NativeType::Double;
  static bool IsCompatible(Value value) { return value.IsDouble(); }
  static double Decode(Value value) { return value.AsDouble(); }
  static NativeResult Encode(NativeContext& /*unused*/, double value) {
    return Value::Double(value);
  }
};

template <>
struct NativeCodec<bool> final {
  static constexpr auto kType = NativeType::Boolean;
  static bool IsCompatible(Value value) { return value.IsBoolean(); }
  static bool Decode(Value value) { return value.AsBoolean(); }
  static NativeResult Encode(NativeContext& /*unused*/, bool value) {
    return Value::Boolean(value);
  }
};

template <>
struct NativeCodec<std::string_view> final {
  static constexpr auto kType = NativeType::String;
  static bool IsCompatible(Value value) { return value.IsString(); }
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
  static bool IsCompatible(Value value);
  static Future Decode(Value value);
  static NativeResult Encode(NativeContext& context, const Future& future);
};

template <>
struct NativeCodec<CancellationToken> final {
  static constexpr auto kType = NativeType::CancellationToken;
  static bool IsCompatible(Value value);
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

[[nodiscard]] NativeResult ValidateNativeResult(NativeResult result);
[[nodiscard]] std::expected<Value, ExecutionStatus> CompleteNativeResult(NativeExecution& execution,
                                                                         std::uint32_t function,
                                                                         NativeResult result);

// ABI 仅传固定宽度标量和托管指针；C++ 字符串、Future 等在同一 codec 边界转换。
template <class T>
struct NativeAbiCodec final {
  static constexpr auto kType = NativeCodec<T>::kType;
  using Type = std::conditional_t<
      kType == NativeType::Integer,
      std::int64_t,
      std::conditional_t<
          kType == NativeType::Double,
          double,
          std::conditional_t<
              kType == NativeType::Boolean || kType == NativeType::Void,
              std::uint8_t,
              std::conditional_t<kType == NativeType::String || kType == NativeType::NullableString,
                                 StringObject*,
                                 HeapObject*>>>>;

  static T Decode(Type value) {
    if constexpr (kType == NativeType::Integer || kType == NativeType::Double) {
      return value;
    } else if constexpr (kType == NativeType::Boolean) {
      return value != 0;
    } else if constexpr (kType == NativeType::String) {
      return NativeCodec<T>::Decode(Value::String(value));
    } else {
      return NativeCodec<T>::Decode(Value::Object(value));
    }
  }

  static Type Encode(Value value) {
    if constexpr (kType == NativeType::Integer) {
      return value.AsInteger();
    } else if constexpr (kType == NativeType::Double) {
      return value.AsDouble();
    } else if constexpr (kType == NativeType::Boolean) {
      return value.AsBoolean() ? 1 : 0;
    } else if constexpr (kType == NativeType::Void) {
      return 0;
    } else if constexpr (kType == NativeType::String || kType == NativeType::NullableString) {
      return value.IsNull() ? nullptr : value.AsString();
    } else {
      return value.AsObject();
    }
  }
};

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
    return InvokeDecoded(context, NativeCodec<Args>::Decode(values.subspan(Indices).front())...);
  }

  static NativeResult InvokeDecoded(NativeContext& context, Args... arguments) {
    if constexpr (std::is_void_v<R>) {
      Function(context, std::forward<Args>(arguments)...);
      return Value::Null();
    } else {
      auto result = Function(context, std::forward<Args>(arguments)...);
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

  static std::uint8_t AotCall(NativeExecution* execution,
                              std::uint32_t function,
                              NativeAbiCodec<Args>::Type... arguments,
                              NativeAbiCodec<Result>::Type* output) {
    NativeContext context(execution->isolate());
    constexpr auto kResultType = NativeCodec<Result>::kType;
    if constexpr (std::is_same_v<R, Result> &&
                  (kResultType == NativeType::Void || kResultType == NativeType::Boolean ||
                   kResultType == NativeType::Integer || kResultType == NativeType::Double)) {
      // 精确签名在实例化时验证。不可失败的基本值已是 ABI 表示，无需再装箱为 Value、
      // 查询结果类型或执行仅用于错误/Future 的完成检查。
      if constexpr (std::is_void_v<R>) {
        Function(context, NativeAbiCodec<Args>::Decode(arguments)...);
        *output = 0;
      } else {
        *output = static_cast<NativeAbiCodec<Result>::Type>(
            Function(context, NativeAbiCodec<Args>::Decode(arguments)...));
      }
    } else {
      auto result = CompleteNativeResult(
          *execution, function, InvokeDecoded(context, NativeAbiCodec<Args>::Decode(arguments)...));
      if (!result) {
        return static_cast<std::uint8_t>(result.error());
      }
      *output = NativeAbiCodec<Result>::Encode(*result);
    }
    return static_cast<std::uint8_t>(ExecutionStatus::Returned);
  }

  static constexpr auto kAotEntry = &AotCall;
};

}  // namespace internal

// 构造器由精确 C++ 函数签名生成；不接受 int/float 隐式缩窄或未检查的 Value 回调。
class NativeFunction final {
 public:
  // 指向静态的精确函数指针槽；只擦除槽的对象类型，不转换函数指针类型。
  // 生成代码从槽读取入口，并按实例化时验证的签名调用。
  using AotEntrySlot = const void*;
  template <auto Function>
  [[nodiscard]] static NativeFunction Bind(std::string name) {
    return BindSignature<Function>(std::move(name), Function);
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::span<const NativeType> parameters() const noexcept { return parameters_; }
  [[nodiscard]] NativeType result_type() const noexcept { return result_type_; }
  [[nodiscard]] AotEntrySlot aot_entry_slot() const noexcept { return aot_entry_slot_; }
  [[nodiscard]] bool IsSignatureCompatible(const CallSignature& signature,
                                           std::span<const layout::Type> types) const;
  // 实参数量须等于 parameters().size()，参数窗口须由调用者持续保根。
  // 不复制参数或重复注册根；返回引用须在下一次分配前写入根槽或 RootedValue。
  [[nodiscard]] NativeResult Invoke(Isolate& isolate, RegisterView arguments) const;

 private:
  template <auto Function, class R, class... Args>
  static NativeFunction BindSignature(std::string name,
                                      R (* /*unused*/)(NativeContext&, Args...) noexcept) {
    using Adapter = internal::NativeAdapter<Function>;
    return NativeFunction(std::move(name), {internal::NativeCodec<Args>::kType...},
                          internal::NativeCodec<typename Adapter::Result>::kType, &Adapter::Call,
                          static_cast<const void*>(&Adapter::kAotEntry));
  }

  using Callback = NativeResult (*)(NativeContext&, RegisterView);
  NativeFunction(std::string name,
                 std::vector<NativeType> parameters,
                 NativeType result_type,
                 Callback callback,
                 AotEntrySlot aot_entry_slot)
      : name_(std::move(name)),
        parameters_(std::move(parameters)),
        result_type_(result_type),
        callback_(callback),
        aot_entry_slot_(aot_entry_slot) {}
  std::string name_;
  std::vector<NativeType> parameters_;
  NativeType result_type_;
  Callback callback_;
  AotEntrySlot aot_entry_slot_;
};

enum class NativeRegistrationError : std::uint8_t { InvalidName, DuplicateName };

extern "C" NativeFunction::AotEntrySlot vela_native_v1_import(NativeExecution* execution,
                                                              std::uint32_t function) noexcept;

}  // namespace vela::runtime
