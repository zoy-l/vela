#include "compiler/frontend/builtins/builtins.h"

#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/regexp.h"
#include "compiler/frontend/builtins/byte_builtins.h"
#include "compiler/frontend/builtins/regexp_builtins.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

template <typename Member>
std::optional<Member> FindMember(std::span<const std::pair<std::string_view, Member>> members,
                                 std::string_view name) {
  for (const auto& [member, builtin] : members) {
    if (member == name) {
      return builtin;
    }
  }
  return std::nullopt;
}

const auto kSignatures = [] {
  const TypeDescriptor text{.kind = Kind::String, .name = kStringTypeName};
  const TypeDescriptor integer{.kind = Kind::Int, .name = kIntTypeName};
  const TypeDescriptor boolean{.kind = Kind::Bool, .name = kBoolTypeName};
  absl::flat_hash_map<Builtin, FunctionDescriptor> signatures = {
      {Builtin::ScalarToString, MakeBuiltinSignature(text, {})},
      {Builtin::ArrayJoin, MakeBuiltinSignature(text, {{.name = "separator", .type = text}})},
      {
          Builtin::NewMutableString,
          MakeBuiltinSignature({.kind = Kind::MutableString, .name = kMutableStringTypeName}, {}),
      },
      {
          Builtin::MutableStringAppend,
          MakeBuiltinSignature({.kind = Kind::Void, .name = kVoidTypeName},
                               {{.name = "text", .type = text}}),
      },
      {Builtin::MutableStringToString, MakeBuiltinSignature(text, {})},
      {Builtin::StringLength, MakeBuiltinSignature(integer, {})},
      {Builtin::StringByteLength, MakeBuiltinSignature(integer, {})},
      {
          Builtin::StringSlice,
          MakeBuiltinSignature(text,
                               {
                                   {.name = "start", .type = integer},
                                   {.name = "end", .type = integer},
                               }),
      },
      {Builtin::StringIndexOf, MakeBuiltinSignature(integer, {{.name = "text", .type = text}})},
      {Builtin::StringIncludes, MakeBuiltinSignature(boolean, {{.name = "text", .type = text}})},
      {
          Builtin::StringStartsWith,
          MakeBuiltinSignature(boolean, {{.name = "prefix", .type = text}}),
      },
      {Builtin::StringEndsWith, MakeBuiltinSignature(boolean, {{.name = "suffix", .type = text}})},
      {Builtin::StringIndex, MakeBuiltinSignature(text, {{.name = "index", .type = integer}})},
      {Builtin::NumericToInt, MakeBuiltinSignature(integer, {})},
      {
          Builtin::NumericToDouble,
          MakeBuiltinSignature({.kind = Kind::Double, .name = kDoubleTypeName}, {}),
      },
      {Builtin::ArrayLength, MakeBuiltinSignature(integer, {})},
      {
          Builtin::StringCodePoints,
          MakeBuiltinSignature({.kind = Kind::Array, .name = kIntArrayTypeName}, {}),
      },
      {
          Builtin::StringGraphemes,
          MakeBuiltinSignature({.kind = Kind::Array, .name = kStringArrayTypeName}, {}),
      },
      {Builtin::StringGraphemeLength, MakeBuiltinSignature(integer, {})},
  };
  signatures.at(Builtin::ArrayJoin).optional_parameters = {"separator"};
  return signatures;
}();

}  // namespace

FunctionDescriptor MakeBuiltinSignature(TypeDescriptor result,
                                        std::initializer_list<BuiltinParameter> parameters) {
  FunctionDescriptor signature;
  signature.return_type = result;
  for (const auto& parameter : parameters) {
    signature.AddParameter(std::string(parameter.name), parameter.type);
  }
  return signature;
}

bool CanFormatString(const TypeDescriptor& type) noexcept {
  return type.kind == Kind::String || type.kind == Kind::Int || type.kind == Kind::Double ||
         type.kind == Kind::Bool || type.kind == Kind::Null || type.kind == Kind::Dynamic;
}

std::span<const std::pair<std::string_view, ArrayAlgorithm>> ArrayAlgorithms() noexcept {
  static constexpr auto kAlgorithms = std::to_array<std::pair<std::string_view, ArrayAlgorithm>>({
      {"sort", ArrayAlgorithm::Sort},
      {"forEach", ArrayAlgorithm::ForEach},
      {"some", ArrayAlgorithm::Some},
      {"every", ArrayAlgorithm::Every},
      {"findIndex", ArrayAlgorithm::FindIndex},
      {"reduce", ArrayAlgorithm::Reduce},
      {"map", ArrayAlgorithm::Map},
  });
  return kAlgorithms;
}

std::span<const std::pair<std::string_view, Builtin>> CollectionMethods(Kind receiver) noexcept {
  static constexpr auto kSet = std::to_array<std::pair<std::string_view, Builtin>>({
      {"has", Builtin::CollectionHas},
      {"delete", Builtin::CollectionDelete},
      {"clear", Builtin::CollectionClear},
      {"values", Builtin::CollectionKeys},
      {"add", Builtin::SetAdd},
      {"union", Builtin::SetUnion},
      {"intersection", Builtin::SetIntersection},
      {"difference", Builtin::SetDifference},
  });
  static constexpr auto kMap = std::to_array<std::pair<std::string_view, Builtin>>({
      {"has", Builtin::CollectionHas},
      {"delete", Builtin::CollectionDelete},
      {"clear", Builtin::CollectionClear},
      {"values", Builtin::CollectionValues},
      {"keys", Builtin::CollectionKeys},
      {"get", Builtin::MapGet},
      {"set", Builtin::MapSet},
  });
  if (receiver == Kind::Set) {
    return kSet;
  }
  return receiver == Kind::Map ? std::span(kMap)
                               : std::span<const std::pair<std::string_view, Builtin>>{};
}

std::span<const std::pair<std::string_view, Builtin>> ArrayMethods() noexcept {
  static constexpr auto kMethods = std::to_array<std::pair<std::string_view, Builtin>>({
      {"slice", Builtin::ArraySlice},
      {"fill", Builtin::ArrayFill},
      {"reverse", Builtin::ArrayReverse},
      {"push", Builtin::ArrayPush},
      {"pop", Builtin::ArrayPop},
      {"shift", Builtin::ArrayShift},
      {"unshift", Builtin::ArrayUnshift},
      {"splice", Builtin::ArraySplice},
      {"clear", Builtin::ArrayClear},
      {"reserve", Builtin::ArrayReserve},
  });
  return kMethods;
}

FunctionDescriptor ArrayMethodSignature(Builtin builtin, const TypeDescriptor& element) {
  const auto receiver = MakeArrayType(element);
  FunctionDescriptor signature;
  signature.return_type = {.kind = Kind::Void, .name = kVoidTypeName};
  const TypeDescriptor integer{.kind = Kind::Int, .name = kIntTypeName};
  switch (builtin) {
    case Builtin::ArraySlice:
      signature.return_type = receiver;
      signature.AddParameter("start", integer);
      signature.AddParameter("end", integer);
      signature.optional_parameters = {"start", "end"};
      break;
    case Builtin::ArrayPush:
    case Builtin::ArrayUnshift:
      signature.return_type = integer;
      signature.AddParameter("items", element);
      signature.parameters.back().rest = true;
      break;
    case Builtin::ArrayPop:
    case Builtin::ArrayShift:
      signature.return_type = MakeNullableType(element);
      break;
    case Builtin::ArraySplice:
      signature.return_type = receiver;
      signature.AddParameter("start", integer);
      signature.AddParameter("deleteCount", integer);
      signature.AddParameter("items", element);
      signature.parameters.back().rest = true;
      signature.optional_parameters = {"start", "deleteCount"};
      break;
    case Builtin::ArrayFill:
      signature.return_type = receiver;
      signature.AddParameter("value", element);
      signature.AddParameter("start", integer);
      signature.AddParameter("end", integer);
      signature.optional_parameters = {"start", "end"};
      break;
    case Builtin::ArrayReserve:
      signature.AddParameter("capacity", integer);
      break;
    case Builtin::ArrayReverse:
      signature.return_type = receiver;
      break;
    case Builtin::ArrayClear:
      break;
    default:
      std::unreachable();
  }
  return signature;
}

std::optional<ArrayAlgorithm> FindArrayAlgorithm(std::string_view name) {
  return FindMember(ArrayAlgorithms(), name);
}

std::optional<Builtin> FindArrayMethod(std::string_view name) {
  return FindMember(ArrayMethods(), name);
}

FunctionDescriptor ArrayCallbackSignature(ArrayAlgorithm algorithm,
                                          const TypeDescriptor& element,
                                          const TypeDescriptor& accumulator) {
  FunctionDescriptor callback;
  callback.return_type = {.kind = Kind::Void, .name = kVoidTypeName};
  const auto parameter = [&callback](const TypeDescriptor& type) -> void {
    callback.AddParameter(std::to_string(callback.parameters.size()), type);
  };
  if (algorithm == ArrayAlgorithm::Sort) {
    parameter(element);
    callback.return_type = {.kind = Kind::Int, .name = kIntTypeName};
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    parameter(accumulator);
    callback.return_type = accumulator;
  } else if (algorithm == ArrayAlgorithm::Map) {
    callback.return_type = accumulator;
  } else if (algorithm != ArrayAlgorithm::ForEach) {
    callback.return_type = {.kind = Kind::Bool, .name = kBoolTypeName};
  }
  parameter(element);
  if (algorithm != ArrayAlgorithm::Sort) {
    parameter({.kind = Kind::Int, .name = kIntTypeName});
    parameter(MakeArrayType(element));
  }
  return callback;
}

FunctionDescriptor ArrayAlgorithmSignature(ArrayAlgorithm algorithm,
                                           const FunctionDescriptor& callback) {
  FunctionDescriptor method;
  method.return_type = {.kind = Kind::Void, .name = kVoidTypeName};
  if (algorithm == ArrayAlgorithm::Some || algorithm == ArrayAlgorithm::Every) {
    method.return_type = {.kind = Kind::Bool, .name = kBoolTypeName};
  } else if (algorithm == ArrayAlgorithm::FindIndex) {
    method.return_type = {.kind = Kind::Int, .name = kIntTypeName};
  } else if (algorithm == ArrayAlgorithm::Map) {
    method.return_type = MakeArrayType(callback.return_type);
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    method.return_type = callback.return_type;
  } else if (algorithm == ArrayAlgorithm::Sort) {
    method.return_type = MakeArrayType(callback.parameters.front().type);
    method.optional_parameters.insert("compareFn");
  }
  std::string parameter = "callbackFn";
  if (algorithm == ArrayAlgorithm::Sort) {
    parameter = "compareFn";
  }
  method.AddParameter(std::move(parameter), MakeFunctionType(callback));
  if (algorithm == ArrayAlgorithm::Reduce) {
    method.AddParameter("initialValue", callback.return_type);
  }
  return method;
}

const FunctionDescriptor& BuiltinSignature(Builtin builtin) {
  if (builtin == Builtin::StringGraphemeSlice) {
    return kSignatures.at(Builtin::StringSlice);
  }
  if (builtin == Builtin::ArrayCapacity || builtin == Builtin::CollectionSize) {
    return kSignatures.at(Builtin::ArrayLength);
  }
  if (IsRegExpBuiltin(builtin)) {
    return RegExpBuiltinSignature(builtin);
  }
  if (IsByteBuiltin(builtin)) {
    return ByteBuiltinSignature(builtin);
  }
  if (builtin == Builtin::StringToLowerCase || builtin == Builtin::StringToUpperCase) {
    static const auto signature =
        MakeBuiltinSignature({.kind = Kind::String, .name = kStringTypeName}, {});
    return signature;
  }
  return kSignatures.at(builtin);
}

std::optional<Builtin> FindBuiltinProperty(Kind receiver, std::string_view name) {
  return FindMember(BuiltinProperties(receiver), name);
}

std::span<const std::pair<std::string_view, Builtin>> BuiltinProperties(Kind receiver) noexcept {
  using Member = std::pair<std::string_view, Builtin>;
  static constexpr auto kBytes = std::to_array<Member>({{"length", Builtin::ByteArrayLength}});
  static constexpr auto kData = std::to_array<Member>({{"byteLength", Builtin::ByteDataLength}});
  static constexpr auto kArray = std::to_array<Member>({
      {"length", Builtin::ArrayLength},
      {"capacity", Builtin::ArrayCapacity},
  });
  static constexpr auto kString = std::to_array<Member>({
      {"length", Builtin::StringLength},
      {"byteLength", Builtin::StringByteLength},
      {"graphemeLength", Builtin::StringGraphemeLength},
  });
  static constexpr auto kCollection = std::to_array<Member>({{"size", Builtin::CollectionSize}});
  if (receiver == Kind::RegExp || receiver == Kind::RegExpMatch) {
    return RegExpProperties(receiver);
  }
  if (receiver == Kind::ByteArray) {
    return kBytes;
  }
  if (receiver == Kind::ByteData) {
    return kData;
  }
  if (receiver == Kind::Array) {
    return kArray;
  }
  if (receiver == Kind::String) {
    return kString;
  }
  if (receiver == Kind::Set || receiver == Kind::Map) {
    return kCollection;
  }
  return {};
}

bool IsBuiltinName(std::string_view name) {
  return name == kDynamicTypeName || name == kIntTypeName || name == kDoubleTypeName ||
         name == kBoolTypeName || name == kStringTypeName || name == kVoidTypeName ||
         name == kSetTypeName || name == kMapTypeName || name == kFutureTypeName ||
         name == kCompleterTypeName || name == kTimerTypeName ||
         name == kCancellationTokenTypeName || name == kByteDataTypeName ||
         name == kRegExpMatchTypeName || name == kSleepFunctionName ||
         FindBuiltin(Kind::Invalid, name).has_value();
}

std::optional<Builtin> FindBuiltin(Kind receiver, std::string_view name) {
  return FindMember(BuiltinMethods(receiver), name);
}

std::span<const std::pair<std::string_view, Builtin>> BuiltinMethods(Kind receiver) noexcept {
  if (receiver == Kind::RegExp || receiver == Kind::RegExpMatch) {
    return RegExpMethods(receiver);
  }
  if (receiver == Kind::ByteArray || receiver == Kind::ByteData) {
    return ByteMethods(receiver);
  }
  if (receiver == Kind::String) {
    static constexpr auto methods = std::to_array<std::pair<std::string_view, Builtin>>({
        {"codePoints", Builtin::StringCodePoints},
        {"graphemes", Builtin::StringGraphemes},
        {"graphemeSlice", Builtin::StringGraphemeSlice},
        {"toLowerCase", Builtin::StringToLowerCase},
        {"toUpperCase", Builtin::StringToUpperCase},
        {"slice", Builtin::StringSlice},
        {"indexOf", Builtin::StringIndexOf},
        {"includes", Builtin::StringIncludes},
        {"startsWith", Builtin::StringStartsWith},
        {"endsWith", Builtin::StringEndsWith},
    });
    return methods;
  }
  if (receiver == Kind::Invalid) {
    static constexpr auto kConstructors = std::to_array<std::pair<std::string_view, Builtin>>({
        {kByteArrayTypeName, Builtin::NewByteArray},
        {kRegExpTypeName, Builtin::NewRegExp},
        {kMutableStringTypeName, Builtin::NewMutableString},
    });
    return kConstructors;
  }
  if (receiver == Kind::Int || receiver == Kind::Double) {
    static constexpr auto kNumeric = std::to_array<std::pair<std::string_view, Builtin>>({
        {"toInt", Builtin::NumericToInt},
        {"toDouble", Builtin::NumericToDouble},
        {"toString", Builtin::ScalarToString},
    });
    return kNumeric;
  }
  if (receiver == Kind::Bool) {
    static constexpr auto kBoolean = std::to_array<std::pair<std::string_view, Builtin>>({
        {"toString", Builtin::ScalarToString},
    });
    return kBoolean;
  }
  if (receiver == Kind::MutableString) {
    static constexpr auto kMutable = std::to_array<std::pair<std::string_view, Builtin>>({
        {"append", Builtin::MutableStringAppend},
        {"toString", Builtin::MutableStringToString},
    });
    return kMutable;
  }
  return {};
}

}  // namespace vela::frontend
