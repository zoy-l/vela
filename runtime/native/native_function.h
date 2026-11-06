#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/callable.h"
#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_host_type.h"
#include "runtime/native/native_type.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::runtime {

class Isolate;
class Future;
class Completer;
class CancellationToken;
struct NativeClassMember;

namespace internal {

template <class T>
struct NativeCodec;

struct NativePlainCodec {
  static constexpr bool kNeedsScope = false;
};

template <class T>
struct NativeLogicalAbi {
  using Type = HeapObject*;
  static constexpr bool kEnum = false;
};
template <>
struct NativeLogicalAbi<String> {
  using Type = StringObject*;
  static constexpr bool kEnum = false;
};
template <>
struct NativeLogicalAbi<vela::Value> {
  using Type = NativeValue;
  static constexpr bool kEnum = false;
};
template <class Tag>
struct NativeLogicalAbi<Enum<Tag>> {
  using Type = std::int64_t;
  static constexpr bool kEnum = true;
};

template <>
struct NativeCodec<std::int64_t> final : NativePlainCodec {
  using AbiType = std::int64_t;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::Integer},
      .hosts = {},
  };
  static NativeResult Encode(NativeContext& /*unused*/, std::int64_t value) {
    return Value::Integer(value);
  }
};

template <>
struct NativeCodec<double> final : NativePlainCodec {
  using AbiType = double;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::Double},
      .hosts = {},
  };
  static NativeResult Encode(NativeContext& /*unused*/, double value) {
    return Value::Double(value);
  }
};

template <>
struct NativeCodec<bool> final : NativePlainCodec {
  using AbiType = std::uint8_t;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::Boolean},
      .hosts = {},
  };
  static NativeResult Encode(NativeContext& /*unused*/, bool value) {
    return Value::Boolean(value);
  }
};

template <>
struct NativeCodec<std::string_view> final : NativePlainCodec {
  using AbiType = StringObject*;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::String},
      .hosts = {},
  };
  static std::string_view Decode(Value value);
  static NativeResult Encode(NativeContext& context, std::string_view text) {
    return context.String(text);
  }
};

template <>
struct NativeCodec<std::string> final : NativePlainCodec {
  using AbiType = StringObject*;
  static constexpr NativeType kType = NativeCodec<std::string_view>::kType;
  static NativeResult Encode(NativeContext& context, const std::string& text) {
    return context.String(text);
  }
};

template <>
struct NativeCodec<void> final : NativePlainCodec {
  using AbiType = std::uint8_t;
  static constexpr NativeType kType{.hosts = {}};
};

template <>
struct NativeCodec<std::optional<std::string>> final : NativePlainCodec {
  using AbiType = StringObject*;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::String, .nullable = true},
      .hosts = {},
  };
  static NativeResult Encode(NativeContext& context, const std::optional<std::string>& text) {
    return text ? context.String(*text) : NativeResult(Value::Null());
  }
};

template <>
struct NativeCodec<CancellationToken> final : NativePlainCodec {
  using AbiType = HeapObject*;
  static constexpr NativeType kType{
      .type = layout::Type{.kind = layout::Type::Kind::CancellationToken},
      .hosts = {},
  };
  static CancellationToken Decode(Value value);
  static NativeResult Encode(NativeContext& context, const CancellationToken& token);
};

template <NativeHostClass T>
struct NativeCodec<T&> final : NativePlainCodec {
  using AbiType = HeapObject*;
  inline static const NativeType kType = NativeHostType<T>();
  static T& Decode(Value value) { return *static_cast<T*>(HostObjectPointer(*value.AsObject())); }
};

template <NativeHostClass T>
struct NativeCodec<T*> final : NativePlainCodec {
  using AbiType = HeapObject*;
  inline static const NativeType kType = NativeNullableHostType<T>();
  static T* Decode(Value value) {
    const auto* object = value.AsObject();
    return object == nullptr ? nullptr : static_cast<T*>(HostObjectPointer(*object));
  }
};

template <NativeHostClass T>
struct NativeCodec<std::unique_ptr<T>> final : NativePlainCodec {
  using AbiType = HeapObject*;
  inline static const NativeType kType = NativeNullableHostType<T>();
  static NativeResult Encode(NativeContext& context, std::unique_ptr<T> value) {
    return context.Own(std::move(value));
  }
};

template <NativeHostClass T>
struct NativeCodec<std::shared_ptr<T>> final : NativePlainCodec {
  using AbiType = HeapObject*;
  inline static const NativeType kType = NativeNullableHostType<T>();
  static NativeResult Encode(NativeContext& context, std::shared_ptr<T> value) {
    return context.Own(std::move(value));
  }
};

template <NativeHostClass T>
struct NativeCodec<NativeConstructed<T>> final : NativePlainCodec {
  using AbiType = HeapObject*;
  inline static const NativeType kType = NativeHostType<T>();
  static NativeResult Encode(NativeContext& context, NativeConstructed<T> value) {
    return context.Own(std::move(value));
  }
};

template <class T>
struct NativeCodec<Local<T>> final {
  using AbiType = NativeLogicalAbi<T>::Type;
  static constexpr bool kNeedsScope = true;
  static constexpr bool kEnum = NativeLogicalAbi<T>::kEnum;
  inline static const NativeType kType = NativeTypeOf<T>();

  static Result<Local<T>> Decode(NativeContext& context, Value value) {
    return vela::internal::HandleAccess::Root<T>(context.handles(), value);
  }
  static NativeResult Encode(NativeContext& context, Local<T> value) {
    if (&context.isolate() != &value.isolate()) {
      return std::unexpected(vela::Error::FromStatus(ExecutionStatus::InvalidIsolate));
    }
    return vela::internal::HandleAccess::Read(value);
  }
};

template <class T>
struct NativeCodec<std::optional<Local<T>>> final {
  using AbiType = std::conditional_t<NativeLogicalAbi<T>::kEnum,
                                     NativeValue,
                                     typename NativeCodec<Local<T>>::AbiType>;
  static constexpr bool kNeedsScope = true;
  static constexpr bool kEnum = NativeLogicalAbi<T>::kEnum;
  inline static const NativeType kType = NativeTypeOf<Nullable<T>>();

  static Result<std::optional<Local<T>>> Decode(NativeContext& context, Value value) {
    if (value.IsNull()) {
      return std::nullopt;
    }
    auto decoded = NativeCodec<Local<T>>::Decode(context, value);
    if (!decoded) {
      return std::unexpected(std::move(decoded.error()));
    }
    return *decoded;
  }
  static NativeResult Encode(NativeContext& context, std::optional<Local<T>> value) {
    return value ? NativeCodec<Local<T>>::Encode(context, *value) : NativeResult(Value::Null());
  }
};

template <class T>
  requires(std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>)
struct NativeCodec<std::optional<T>> final : NativePlainCodec {
  using AbiType = NativeValue;
  inline static const NativeType kType = NativeTypeOf<Nullable<T>>();
  static std::optional<T> Decode(Value value) {
    if (value.IsNull()) {
      return std::nullopt;
    }
    if constexpr (std::is_same_v<T, bool>) {
      return value.AsBoolean();
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
      return value.AsInteger();
    } else {
      return value.AsDouble();
    }
  }
  static NativeResult Encode(NativeContext& context, std::optional<T> value) {
    return value ? NativeCodec<T>::Encode(context, *value) : NativeResult(Value::Null());
  }
};

template <class T>
struct NativeReturn {
  using Type = T;
};

template <class T>
struct NativeReturn<std::expected<T, vela::Error>> {
  using Type = T;
};

template <auto Function>
struct NativeAdapter;

[[nodiscard]] NativeResult ValidateNativeResult(NativeResult result);
[[nodiscard]] NativeResult BoxNativeArgument(NativeExecution& execution,
                                             std::uint32_t function,
                                             Value value,
                                             std::size_t argument);
[[nodiscard]] std::expected<Value, ExecutionStatus> CompleteNativeResult(NativeExecution& execution,
                                                                         std::uint32_t function,
                                                                         NativeResult result);

// ABI 仅传固定宽度标量和托管指针；C++ 字符串、Future 等在同一 codec 边界转换。
template <class T>
struct NativeAbiCodec final {
  using Type = NativeCodec<T>::AbiType;
  using Argument = std::conditional_t<std::is_same_v<Type, NativeValue>, const NativeValue*, Type>;
  using Decoded = std::conditional_t<std::is_lvalue_reference_v<T>,
                                     std::reference_wrapper<std::remove_reference_t<T>>,
                                     T>;

  static T Decode(Argument value) {
    if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>) {
      return value;
    } else if constexpr (std::is_same_v<T, bool>) {
      return value != 0;
    } else if constexpr (std::is_same_v<Type, NativeValue>) {
      return NativeCodec<T>::Decode(DecodeNativeValue(*value));
    } else if constexpr (std::is_same_v<Type, StringObject*>) {
      return NativeCodec<T>::Decode(Value::String(value));
    } else {
      return NativeCodec<T>::Decode(Value::Object(value));
    }
  }

  static Result<Decoded> DecodeChecked(NativeExecution& execution,
                                       std::uint32_t function,
                                       std::size_t index,
                                       Argument value) {
    auto& context = execution.context();
    if constexpr (NativeCodec<T>::kNeedsScope) {
      auto decoded = [&] {
        if constexpr (std::is_same_v<Type, NativeValue>) {
          return DecodeNativeValue(*value);
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
          return Value::Integer(value);
        } else if constexpr (std::is_same_v<Type, StringObject*>) {
          return value == nullptr ? Value::Null() : Value::String(value);
        } else {
          return value == nullptr ? Value::Null() : Value::Object(value);
        }
      }();
      if constexpr (NativeCodec<T>::kEnum) {
        auto boxed = BoxNativeArgument(execution, function, decoded, index);
        if (!boxed) {
          return std::unexpected(std::move(boxed.error()));
        }
        decoded = *boxed;
      }
      return NativeCodec<T>::Decode(context, decoded);
    } else {
      return Decode(value);
    }
  }

  static Type Encode(Value value) {
    if constexpr (std::is_same_v<Type, NativeValue>) {
      if constexpr (NativeCodec<T>::kNeedsScope) {
        if constexpr (NativeCodec<T>::kEnum) {
          value = value.IsNull() ? value : Value::Integer(EnumOrdinal(*value.AsObject()));
        }
      }
      return EncodeNativeValue(value);
    } else if constexpr (std::is_same_v<Type, std::int64_t>) {
      if constexpr (NativeCodec<T>::kNeedsScope) {
        return EnumOrdinal(*value.AsObject());
      }
      return value.AsInteger();
    } else if constexpr (std::is_same_v<T, double>) {
      return value.AsDouble();
    } else if constexpr (std::is_same_v<T, bool>) {
      return value.AsBoolean() ? 1 : 0;
    } else if constexpr (std::is_void_v<T>) {
      return 0;
    } else if constexpr (std::is_same_v<Type, StringObject*>) {
      return value.IsNull() ? nullptr : value.AsString();
    } else {
      return value.IsNull() ? nullptr : value.AsObject();
    }
  }
};

template <class T>
struct NativeDirectArgument {
  using Codec = NativeAbiCodec<T>;
  using Type = Codec::Type;
  static T Decode(Codec::Argument value) { return Codec::Decode(value); }
};

template <>
struct NativeDirectArgument<bool> final {
  using Type = bool;
  static bool Decode(Type value) noexcept { return value; }
};

template <NativeHostClass T>
struct NativeDirectArgument<T&> final {
  using Type = T*;
  static T& Decode(Type value) noexcept { return *value; }
};

template <NativeHostClass T>
struct NativeDirectArgument<T*> final {
  using Type = T*;
  static T* Decode(Type value) noexcept { return value; }
};

template <auto Function, class R, class... Args>
struct NativeAdapterBase {
  using Result = NativeReturn<R>::Type;
  static constexpr bool kNeedsContext =
      std::is_invocable_v<decltype(Function), NativeContext&, Args...>;
  static constexpr bool kNeedsScope =
      kNeedsContext || NativeCodec<Result>::kNeedsScope || (NativeCodec<Args>::kNeedsScope || ...);
  static constexpr bool kDirect =
      std::is_same_v<R, Result> &&
      (std::is_void_v<Result> || std::is_same_v<Result, bool> ||
       std::is_same_v<Result, std::int64_t> || std::is_same_v<Result, double>) &&
      !(NativeCodec<Args>::kNeedsScope || ...) &&
      !(std::is_same_v<typename NativeAbiCodec<Args>::Type, NativeValue> || ...);
  struct EmptyScope final {
    explicit EmptyScope(NativeContext& /*unused*/) noexcept {}
  };
  using Scope = std::conditional_t<kNeedsScope, NativeContextScope, EmptyScope>;

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
      if constexpr (std::is_same_v<R, std::expected<Result, vela::Error>>) {
        if (!result) {
          return std::unexpected(std::move(result.error()));
        }
        if constexpr (std::is_void_v<Result>) {
          return Value::Null();
        } else {
          return NativeCodec<Result>::Encode(context, std::move(*result));
        }
      } else {
        return NativeCodec<Result>::Encode(context, std::move(result));
      }
    }
  }

  static std::uint8_t AotCall(NativeExecution* execution,
                              std::uint32_t function,
                              NativeAbiCodec<Args>::Argument... arguments,
                              NativeAbiCodec<Result>::Type* output) {
    auto& context = execution->context();
    Scope scope(context);
    auto abi = std::tuple<typename NativeAbiCodec<Args>::Argument...>(arguments...);
    auto result =
        CompleteNativeResult(*execution, function, InvokeArguments<0>(*execution, function, abi));
    if (!result) {
      return static_cast<std::uint8_t>(result.error());
    }
    *output = NativeAbiCodec<Result>::Encode(*result);
    return static_cast<std::uint8_t>(ExecutionStatus::Returned);
  }

  template <std::size_t Index, class... Decoded>
  static NativeResult InvokeArguments(
      NativeExecution& execution,
      std::uint32_t function,
      std::tuple<typename NativeAbiCodec<Args>::Argument...>& arguments,
      Decoded&&... decoded) {
    if constexpr (Index == sizeof...(Args)) {
      return InvokeDecoded(execution.context(), std::forward<Decoded>(decoded)...);
    } else {
      using Argument = std::tuple_element_t<Index, std::tuple<Args...>>;
      auto next = NativeAbiCodec<Argument>::DecodeChecked(execution, function, Index,
                                                          std::get<Index>(arguments));
      if (!next) {
        return std::unexpected(std::move(next.error()));
      }
      return InvokeArguments<Index + 1>(execution, function, arguments,
                                        std::forward<Decoded>(decoded)..., std::move(*next));
    }
  }

  static R DirectCall(NativeContext& context,
                      NativeDirectArgument<Args>::Type... arguments) noexcept {
    Scope scope(context);
    if constexpr (std::is_void_v<R>) {
      InvokeFunction(context, NativeDirectArgument<Args>::Decode(arguments)...);
    } else {
      return InvokeFunction(context, NativeDirectArgument<Args>::Decode(arguments)...);
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
  NativeFunction(const NativeFunction&) = delete;
  NativeFunction& operator=(const NativeFunction&) = delete;
  NativeFunction(NativeFunction&&) noexcept = default;
  NativeFunction& operator=(NativeFunction&&) noexcept = default;
  ~NativeFunction() = default;
  // 指向静态的精确函数指针槽；只擦除槽的对象类型，不转换函数指针类型。
  // 生成代码从槽读取入口，并按实例化时验证的签名调用。
  using AotEntrySlot = const void*;
  enum class AotCallKind : std::uint8_t { Checked, Direct };
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
        // 直接入口统一携带上下文；适配器在编译期决定 C++ 函数是否消费它。
        static constexpr auto kTarget = &Adapter::DirectCall;
        return AotEntry{
            .slot = static_cast<AotEntrySlot>(&kTarget),
            .kind = AotCallKind::Direct,
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
                                           const layout::Program& layouts) const;

 private:
  friend class Isolate;
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
  const NativeClassMember* member_ = nullptr;
};

extern "C" const NativeFunction::AotEntry* vela_native_v1_import(NativeExecution* execution,
                                                                 std::uint32_t function) noexcept;

}  // namespace vela::runtime
