#include "compiler/frontend/builtins.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/regexp.h"
#include "compiler/frontend/byte_builtins.h"
#include "compiler/frontend/regexp_builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_name.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

std::optional<Builtin> FindMember(std::span<const std::pair<std::string_view, Builtin>> members,
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
  auto signatures = std::to_array<FunctionDescriptor>({
      MakeBuiltinSignature(text, {}),
      MakeBuiltinSignature(text, {{.name = "separator", .type = text}}),
      MakeBuiltinSignature({.kind = Kind::MutableString, .name = kMutableStringTypeName}, {}),
      MakeBuiltinSignature({.kind = Kind::Void, .name = kVoidTypeName},
                           {{.name = "text", .type = text}}),
      MakeBuiltinSignature(text, {}),
      MakeBuiltinSignature(integer, {}),
      MakeBuiltinSignature(integer, {}),
      MakeBuiltinSignature(text,
                           {{.name = "start", .type = integer}, {.name = "end", .type = integer}}),
      MakeBuiltinSignature(integer, {{.name = "text", .type = text}}),
      MakeBuiltinSignature(boolean, {{.name = "text", .type = text}}),
      MakeBuiltinSignature(boolean, {{.name = "prefix", .type = text}}),
      MakeBuiltinSignature(boolean, {{.name = "suffix", .type = text}}),
      MakeBuiltinSignature(text, {{.name = "index", .type = integer}}),
      MakeBuiltinSignature(integer, {}),
      MakeBuiltinSignature({.kind = Kind::Double, .name = kDoubleTypeName}, {}),
      MakeBuiltinSignature(integer, {}),
      MakeBuiltinSignature({.kind = Kind::Array, .name = kIntArrayTypeName}, {}),
      MakeBuiltinSignature({.kind = Kind::Array, .name = kStringArrayTypeName}, {}),
      MakeBuiltinSignature(integer, {}),
  });
  signatures.at(static_cast<std::size_t>(Builtin::ArrayJoin)).optional_parameters = {"separator"};
  return signatures;
}();
static_assert(kSignatures.size() == static_cast<std::size_t>(Builtin::StringGraphemeSlice));

}  // namespace

FunctionDescriptor MakeBuiltinSignature(TypeDescriptor result,
                                        std::initializer_list<BuiltinParameter> parameters) {
  FunctionDescriptor signature;
  signature.return_type = std::move(result);
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

std::optional<Builtin> FindSetOperation(std::string_view name) {
  if (name == "union") {
    return Builtin::SetUnion;
  }
  if (name == "intersection") {
    return Builtin::SetIntersection;
  }
  if (name == "difference") {
    return Builtin::SetDifference;
  }
  return std::nullopt;
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
  for (const auto& [label, algorithm] : ArrayAlgorithms()) {
    if (name == label) {
      return algorithm;
    }
  }
  return std::nullopt;
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
                                           const TypeDescriptor& element,
                                           const TypeDescriptor& accumulator) {
  const auto callback = ArrayCallbackSignature(algorithm, element, accumulator);
  std::string name = "(";
  for (const auto& parameter : callback.parameters) {
    if (name.back() != '(') {
      name += ", ";
    }
    name += parameter.type.name;
  }
  name += ") => ";
  name += callback.return_type.name;
  FunctionDescriptor method;
  method.return_type = {.kind = Kind::Void, .name = kVoidTypeName};
  if (algorithm == ArrayAlgorithm::Some || algorithm == ArrayAlgorithm::Every) {
    method.return_type = {.kind = Kind::Bool, .name = kBoolTypeName};
  } else if (algorithm == ArrayAlgorithm::FindIndex) {
    method.return_type = {.kind = Kind::Int, .name = kIntTypeName};
  } else if (algorithm == ArrayAlgorithm::Map) {
    method.return_type = MakeArrayType(accumulator);
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    method.return_type = accumulator;
  } else if (algorithm == ArrayAlgorithm::Sort) {
    method.return_type = MakeArrayType(element);
    method.optional_parameters.insert("compareFn");
  }
  std::string parameter = "callbackFn";
  if (algorithm == ArrayAlgorithm::Sort) {
    parameter = "compareFn";
  }
  method.AddParameter(std::move(parameter),
                      {.kind = Kind::Function, .name = InternTypeName(std::move(name))});
  if (algorithm == ArrayAlgorithm::Reduce) {
    method.AddParameter("initialValue", accumulator);
  }
  return method;
}

const FunctionDescriptor& BuiltinSignature(Builtin builtin) {
  if (builtin == Builtin::StringGraphemeSlice) {
    return kSignatures.at(static_cast<std::size_t>(Builtin::StringSlice));
  }
  if (builtin == Builtin::ArrayCapacity || builtin == Builtin::CollectionSize) {
    return kSignatures.at(static_cast<std::size_t>(Builtin::ArrayLength));
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
  return kSignatures.at(static_cast<std::size_t>(builtin));
}

std::optional<Builtin> FindBuiltinProperty(Kind receiver, std::string_view name) {
  if (const auto builtin = FindMember(RegExpProperties(receiver), name)) {
    return builtin;
  }
  if (receiver == Kind::ByteArray && name == "length") {
    return Builtin::ByteArrayLength;
  }
  if (receiver == Kind::ByteData && name == "byteLength") {
    return Builtin::ByteDataLength;
  }
  if (receiver == Kind::Array) {
    if (name == "length") {
      return Builtin::ArrayLength;
    }
    if (name == "capacity") {
      return Builtin::ArrayCapacity;
    }
  } else if (receiver == Kind::String) {
    if (name == "length") {
      return Builtin::StringLength;
    }
    if (name == "byteLength") {
      return Builtin::StringByteLength;
    }
    if (name == "graphemeLength") {
      return Builtin::StringGraphemeLength;
    }
  } else if ((receiver == Kind::Set || receiver == Kind::Map) && name == "size") {
    return Builtin::CollectionSize;
  }
  return std::nullopt;
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
  if (receiver == Kind::RegExp || receiver == Kind::RegExpMatch) {
    return FindMember(RegExpMethods(receiver), name);
  }
  if (receiver == Kind::ByteArray || receiver == Kind::ByteData) {
    return FindMember(ByteMethods(receiver), name);
  }
  if (receiver == Kind::String) {
    constexpr auto methods = std::to_array<std::pair<std::string_view, Builtin>>({
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
    return FindMember(methods, name);
  }
  if (receiver == Kind::Invalid) {
    if (name == kByteArrayTypeName) {
      return Builtin::NewByteArray;
    }
    if (name == kRegExpTypeName) {
      return Builtin::NewRegExp;
    }
    if (name == kMutableStringTypeName) {
      return Builtin::NewMutableString;
    }
  } else if ((receiver == Kind::Int || receiver == Kind::Double) && name == "toInt") {
    return Builtin::NumericToInt;
  } else if ((receiver == Kind::Int || receiver == Kind::Double) && name == "toDouble") {
    return Builtin::NumericToDouble;
  } else if ((receiver == Kind::Int || receiver == Kind::Double || receiver == Kind::Bool) &&
             name == "toString") {
    return Builtin::ScalarToString;
  } else if (receiver == Kind::MutableString) {
    if (name == "append") {
      return Builtin::MutableStringAppend;
    }
    if (name == "toString") {
      return Builtin::MutableStringToString;
    }
  }
  return std::nullopt;
}

}  // namespace vela::frontend
