#include "compiler/frontend/builtins.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/regexp.h"
#include "compiler/frontend/byte_builtins.h"
#include "compiler/frontend/regexp_builtins.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

struct ParameterSpec final {
  std::string_view name;
  Kind kind;
  std::string_view type;
};

FunctionDescriptor Signature(Kind kind,
                             std::string_view type,
                             std::initializer_list<ParameterSpec> parameters) {
  FunctionDescriptor function;
  function.return_type = {.kind = kind, .name = std::string(type)};
  for (const auto& parameter : parameters) {
    function.parameter_indices.emplace(parameter.name,
                                       static_cast<std::uint32_t>(function.parameters.size()));
    function.parameters.push_back({
        .name = std::string(parameter.name),
        .type = {.kind = parameter.kind, .name = std::string(parameter.type)},
    });
  }
  return function;
}

const auto kSignatures = [] {
  auto signatures = std::to_array<FunctionDescriptor>({
      Signature(Kind::String, "string", {}),
      Signature(Kind::String, "string",
                {
                    {.name = "separator", .kind = Kind::String, .type = "string"},
                }),
      Signature(Kind::MutableString, "String", {}),
      Signature(Kind::Void, "void", {{.name = "text", .kind = Kind::String, .type = "string"}}),
      Signature(Kind::String, "string", {}),
      Signature(Kind::Int, "int", {}),
      Signature(Kind::Int, "int", {}),
      Signature(Kind::String, "string",
                {
                    {.name = "start", .kind = Kind::Int, .type = "int"},
                    {.name = "end", .kind = Kind::Int, .type = "int"},
                }),
      Signature(Kind::Int, "int", {{.name = "text", .kind = Kind::String, .type = "string"}}),
      Signature(Kind::Bool, "bool", {{.name = "text", .kind = Kind::String, .type = "string"}}),
      Signature(Kind::Bool, "bool", {{.name = "prefix", .kind = Kind::String, .type = "string"}}),
      Signature(Kind::Bool, "bool", {{.name = "suffix", .kind = Kind::String, .type = "string"}}),
      Signature(Kind::String, "string", {{.name = "index", .kind = Kind::Int, .type = "int"}}),
      Signature(Kind::Int, "int", {}),
      Signature(Kind::Double, "double", {}),
      Signature(Kind::Int, "int", {}),
      Signature(Kind::Array, "int[]", {}),
      Signature(Kind::Array, "string[]", {}),
      Signature(Kind::Int, "int", {}),
      Signature(Kind::String, "string",
                {
                    {.name = "start", .kind = Kind::Int, .type = "int"},
                    {.name = "end", .kind = Kind::Int, .type = "int"},
                }),
  });
  signatures.at(static_cast<std::size_t>(Builtin::ArrayJoin)).optional_parameters = {"separator"};
  return signatures;
}();
static_assert(kSignatures.size() == static_cast<std::size_t>(Builtin::CollectionSize));

}  // namespace

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
  signature.return_type = {.kind = Kind::Void, .name = "void"};
  const TypeDescriptor integer{.kind = Kind::Int, .name = "int"};
  const auto parameter = [&signature](std::string_view name, const TypeDescriptor& type) -> void {
    signature.parameter_indices.emplace(name,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back({.name = std::string(name), .type = type});
  };
  switch (builtin) {
    case Builtin::ArraySlice:
      signature.return_type = receiver;
      parameter("start", integer);
      parameter("end", integer);
      signature.optional_parameters = {"start", "end"};
      break;
    case Builtin::ArrayPush:
    case Builtin::ArrayUnshift:
      signature.return_type = integer;
      parameter("items", element);
      signature.parameters.back().rest = true;
      break;
    case Builtin::ArrayPop:
    case Builtin::ArrayShift:
      signature.return_type = MakeNullableType(element);
      break;
    case Builtin::ArraySplice:
      signature.return_type = receiver;
      parameter("start", integer);
      parameter("deleteCount", integer);
      parameter("items", element);
      signature.parameters.back().rest = true;
      signature.optional_parameters = {"start", "deleteCount"};
      break;
    case Builtin::ArrayFill:
      signature.return_type = receiver;
      parameter("value", element);
      parameter("start", integer);
      parameter("end", integer);
      signature.optional_parameters = {"start", "end"};
      break;
    case Builtin::ArrayReserve:
      parameter("capacity", integer);
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
  callback.return_type = {.kind = Kind::Void, .name = "void"};
  const auto parameter = [&callback](const TypeDescriptor& type) -> void {
    const auto key = std::to_string(callback.parameters.size());
    callback.parameter_indices.emplace(key, static_cast<std::uint32_t>(callback.parameters.size()));
    callback.parameters.push_back({.name = key, .type = type});
  };
  if (algorithm == ArrayAlgorithm::Sort) {
    parameter(element);
    callback.return_type = {.kind = Kind::Int, .name = "int"};
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    parameter(accumulator);
    callback.return_type = accumulator;
  } else if (algorithm == ArrayAlgorithm::Map) {
    callback.return_type = accumulator;
  } else if (algorithm != ArrayAlgorithm::ForEach) {
    callback.return_type = {.kind = Kind::Bool, .name = "bool"};
  }
  parameter(element);
  if (algorithm != ArrayAlgorithm::Sort) {
    parameter({.kind = Kind::Int, .name = "int"});
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
  name += ") => " + callback.return_type.name;
  FunctionDescriptor method;
  method.return_type = {.kind = Kind::Void, .name = "void"};
  if (algorithm == ArrayAlgorithm::Some || algorithm == ArrayAlgorithm::Every) {
    method.return_type = {.kind = Kind::Bool, .name = "bool"};
  } else if (algorithm == ArrayAlgorithm::FindIndex) {
    method.return_type = {.kind = Kind::Int, .name = "int"};
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
  method.parameter_indices.emplace(parameter, static_cast<std::uint32_t>(method.parameters.size()));
  method.parameters.push_back(
      {.name = parameter, .type = {.kind = Kind::Function, .name = std::move(name)}});
  if (algorithm == ArrayAlgorithm::Reduce) {
    method.parameter_indices.emplace("initialValue", 1);
    method.parameters.push_back({.name = "initialValue", .type = accumulator});
  }
  return method;
}

const FunctionDescriptor& BuiltinSignature(Builtin builtin) {
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
    static const auto signature = Signature(Kind::String, "string", {});
    return signature;
  }
  return kSignatures.at(static_cast<std::size_t>(builtin));
}

std::optional<Builtin> FindBuiltinProperty(Kind receiver, std::string_view name) {
  for (const auto& [member, builtin] : RegExpProperties(receiver)) {
    if (member == name) {
      return builtin;
    }
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
  return name == "dynamic" || name == "int" || name == "double" || name == "bool" ||
         name == "string" || name == "void" || name == "Set" || name == "Map" || name == "Future" ||
         name == "Completer" || name == "Timer" || name == "CancellationToken" ||
         name == "ByteData" || name == "RegExpMatch" || name == "sleep" ||
         FindBuiltin(Kind::Invalid, name).has_value();
}

std::optional<Builtin> FindBuiltin(Kind receiver, std::string_view name) {
  if (const auto builtin = FindRegExpBuiltin(receiver, name)) {
    return builtin;
  }
  if (const auto builtin = FindByteBuiltin(receiver, name)) {
    return builtin;
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
    for (const auto& [method, builtin] : methods) {
      if (name == method) {
        return builtin;
      }
    }
    return std::nullopt;
  }
  if (receiver == Kind::Invalid) {
    if (name == "String") {
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
