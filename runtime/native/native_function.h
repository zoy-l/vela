#pragma once

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

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_context.h"
#include "runtime/value/value.h"

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
namespace internal {

template <class T>
struct NativeCodec;

template <>
struct NativeCodec<std::int64_t> final {
  static constexpr auto kType = NativeType::Integer;
  static NativeResult Encode(NativeContext& /*unused*/, std::int64_t value) {
    return Value::Integer(value);
  }
};

template <>
struct NativeCodec<double> final {
  static constexpr auto kType = NativeType::Double;
  static NativeResult Encode(NativeContext& /*unused*/, double value) {
    return Value::Double(value);
  }
};

template <>
struct NativeCodec<bool> final {
  static constexpr auto kType = NativeType::Boolean;
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

template <auto Function, class R, class... Args>
struct NativeAdapterBase {
  using Result = NativeReturn<R>::Type;
  static constexpr auto kResultType = NativeCodec<Result>::kType;
  static constexpr bool kDirect =
      std::is_same_v<R, Result> &&
      (kResultType == NativeType::Void || kResultType == NativeType::Boolean ||
       kResultType == NativeType::Integer || kResultType == NativeType::Double);
  using DirectResult =
      std::conditional_t<std::is_void_v<R>, void, typename NativeAbiCodec<Result>::Type>;
  static constexpr bool kNeedsContext =
      std::is_invocable_v<decltype(Function), NativeContext&, Args...>;
  static constexpr bool kExactAbi =
      std::is_same_v<R, DirectResult> &&
      (std::is_same_v<Args, typename NativeAbiCodec<Args>::Type> && ...);
  static constexpr bool kDirectContext = kNeedsContext || !kExactAbi;

  static std::vector<NativeType> Parameters() { return {NativeCodec<Args>::kType...}; }

  static R InvokeFunction(NativeContext& context, Args... arguments) noexcept {
    if constexpr (kNeedsContext) {
      return Function(context, std::forward<Args>(arguments)...);
    } else {
      return Function(std::forward<Args>(arguments)...);
    }
  }

  static NativeResult InvokeDecoded(NativeContext& context, Args... arguments) {
    if constexpr (std::is_void_v<R>) {
      InvokeFunction(context, std::forward<Args>(arguments)...);
      return Value::Null();
    } else {
      auto result = InvokeFunction(context, std::forward<Args>(arguments)...);
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

  static std::uint8_t AotCall(NativeExecution* execution,
                              std::uint32_t function,
                              NativeAbiCodec<Args>::Type... arguments,
                              NativeAbiCodec<Result>::Type* output) {
    auto result = CompleteNativeResult(
        *execution, function,
        InvokeDecoded(execution->context(), NativeAbiCodec<Args>::Decode(arguments)...));
    if (!result) {
      return static_cast<std::uint8_t>(result.error());
    }
    *output = NativeAbiCodec<Result>::Encode(*result);
    return static_cast<std::uint8_t>(ExecutionStatus::Returned);
  }

  static DirectResult DirectCall(NativeContext& context,
                                 NativeAbiCodec<Args>::Type... arguments) noexcept {
    if constexpr (std::is_void_v<R>) {
      InvokeFunction(context, NativeAbiCodec<Args>::Decode(arguments)...);
    } else {
      return static_cast<DirectResult>(
          InvokeFunction(context, NativeAbiCodec<Args>::Decode(arguments)...));
    }
  }

  static constexpr auto DirectEntry() {
    // 基本标量签名完全一致时直接保留原函数指针；bool 和托管参数仍由 codec 转换。
    if constexpr (kExactAbi) {
      return Function;
    } else {
      return &DirectCall;
    }
  }
};

template <class R, class... Args, R (*Function)(NativeContext&, Args...) noexcept>
struct NativeAdapter<Function> final : NativeAdapterBase<Function, R, Args...> {};

template <class R, class... Args, R (*Function)(Args...) noexcept>
struct NativeAdapter<Function> final : NativeAdapterBase<Function, R, Args...> {};

}  // namespace internal

// 构造器由精确 C++ 函数签名生成；不接受 int/float 隐式缩窄或未检查的 Value 回调。
class NativeFunction final {
 public:
  // 指向静态的精确函数指针槽；只擦除槽的对象类型，不转换函数指针类型。
  // 生成代码从槽读取入口，并按实例化时验证的签名调用。
  using AotEntrySlot = const void*;
  enum class AotCallKind : std::uint8_t { Checked, Direct, DirectWithContext };
  enum class Intrinsic : std::uint8_t { None, SquareRoot };
  struct AotEntry final {
    AotEntrySlot slot = nullptr;
    AotCallKind kind = AotCallKind::Checked;
    Intrinsic intrinsic = Intrinsic::None;
  };
  template <auto Function>
  [[nodiscard]] static NativeFunction Bind(std::string name) {
    using Adapter = internal::NativeAdapter<Function>;
    static constexpr AotEntry kEntry = [] {
      if constexpr (Adapter::kDirect) {
        static constexpr auto kTarget = Adapter::DirectEntry();
        return AotEntry{
            .slot = static_cast<AotEntrySlot>(&kTarget),
            .kind = Adapter::kDirectContext ? AotCallKind::DirectWithContext : AotCallKind::Direct,
        };
      } else {
        static constexpr auto kTarget = &Adapter::AotCall;
        return AotEntry{.slot = static_cast<AotEntrySlot>(&kTarget)};
      }
    }();
    return NativeFunction(std::move(name), Adapter::Parameters(),
                          internal::NativeCodec<typename Adapter::Result>::kType, kEntry);
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::span<const NativeType> parameters() const noexcept { return parameters_; }
  [[nodiscard]] NativeType result_type() const noexcept { return result_type_; }
  [[nodiscard]] const AotEntry& aot_entry() const noexcept { return aot_entry_; }
  [[nodiscard]] bool IsSignatureCompatible(const CallSignature& signature,
                                           std::span<const layout::Type> types) const;

 private:
  friend std::shared_ptr<const NativeFunction> FindStandardNative(std::string_view name);
  NativeFunction(std::string name,
                 std::vector<NativeType> parameters,
                 NativeType result_type,
                 AotEntry aot_entry)
      : name_(std::move(name)),
        parameters_(std::move(parameters)),
        result_type_(result_type),
        aot_entry_(aot_entry) {}
  std::string name_;
  std::vector<NativeType> parameters_;
  NativeType result_type_;
  AotEntry aot_entry_;
};

enum class NativeRegistrationError : std::uint8_t { InvalidName, DuplicateName, BindingsFrozen };

extern "C" const NativeFunction::AotEntry* vela_native_v1_import(NativeExecution* execution,
                                                                 std::uint32_t function) noexcept;

}  // namespace vela::runtime
