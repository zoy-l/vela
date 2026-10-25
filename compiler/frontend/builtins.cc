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

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

struct ParameterSpec final {
  std::string_view name;
  Kind kind;
  std::string_view type;
};

auto Signature(Kind kind, std::string_view type, std::initializer_list<ParameterSpec> parameters)
    -> FunctionDescriptor {
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

const auto kSignatures = std::to_array<FunctionDescriptor>({
    Signature(Kind::String, "string", {}),
    Signature(Kind::String,
              "string",
              {
                  {.name = "separator", .kind = Kind::String, .type = "string"},
              }),
    Signature(Kind::StringBuilder, "StringBuilder", {}),
    Signature(Kind::Void, "void", {{.name = "text", .kind = Kind::String, .type = "string"}}),
    Signature(Kind::String, "string", {}),
    Signature(Kind::Int, "int", {}),
    Signature(Kind::Int, "int", {}),
    Signature(Kind::String,
              "string",
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
    Signature(Kind::String,
              "string",
              {
                  {.name = "start", .kind = Kind::Int, .type = "int"},
                  {.name = "end", .kind = Kind::Int, .type = "int"},
              }),
});
static_assert(kSignatures.size() == static_cast<std::size_t>(Builtin::CollectionLength));

}  // namespace

auto CanFormatString(const TypeDescriptor& type) noexcept -> bool {
  return type.kind == Kind::String || type.kind == Kind::Int || type.kind == Kind::Double ||
         type.kind == Kind::Bool || type.kind == Kind::Null;
}

auto ArrayAlgorithms() noexcept -> std::span<const std::pair<std::string_view, ArrayAlgorithm>> {
  static constexpr auto kAlgorithms = std::to_array<std::pair<std::string_view, ArrayAlgorithm>>({
      {"sort", ArrayAlgorithm::Sort},
      {"forEach", ArrayAlgorithm::ForEach},
      {"any", ArrayAlgorithm::Any},
      {"every", ArrayAlgorithm::Every},
      {"findIndex", ArrayAlgorithm::FindIndex},
      {"reduce", ArrayAlgorithm::Reduce},
      {"transform", ArrayAlgorithm::Transform},
  });
  return kAlgorithms;
}

auto FindSetOperation(std::string_view name) -> std::optional<Builtin> {
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

auto FindArrayAlgorithm(std::string_view name) -> std::optional<ArrayAlgorithm> {
  for (const auto& [label, algorithm] : ArrayAlgorithms()) {
    if (name == label) {
      return algorithm;
    }
  }
  return std::nullopt;
}

auto ArrayCallbackSignature(ArrayAlgorithm algorithm,
                            const TypeDescriptor& element,
                            const TypeDescriptor& accumulator) -> FunctionDescriptor {
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
  } else if (algorithm == ArrayAlgorithm::Transform) {
    callback.return_type = element;
  } else if (algorithm != ArrayAlgorithm::ForEach) {
    callback.return_type = {.kind = Kind::Bool, .name = "bool"};
  }
  parameter(element);
  return callback;
}

auto ArrayAlgorithmSignature(ArrayAlgorithm algorithm,
                             const TypeDescriptor& element,
                             const TypeDescriptor& accumulator) -> FunctionDescriptor {
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
  if (algorithm == ArrayAlgorithm::Any || algorithm == ArrayAlgorithm::Every) {
    method.return_type = {.kind = Kind::Bool, .name = "bool"};
  } else if (algorithm == ArrayAlgorithm::FindIndex) {
    method.return_type = {.kind = Kind::Int, .name = "int"};
  } else if (algorithm == ArrayAlgorithm::Reduce) {
    method.return_type = accumulator;
    method.parameter_indices.emplace("initial", 0);
    method.parameters.push_back({.name = "initial", .type = accumulator});
  }
  std::string parameter = "operation";
  if (algorithm == ArrayAlgorithm::Sort) {
    parameter = "compare";
  } else if (algorithm == ArrayAlgorithm::Any || algorithm == ArrayAlgorithm::Every ||
             algorithm == ArrayAlgorithm::FindIndex) {
    parameter = "predicate";
  }
  method.parameter_indices.emplace(parameter, static_cast<std::uint32_t>(method.parameters.size()));
  method.parameters.push_back(
      {.name = parameter, .type = {.kind = Kind::Function, .name = std::move(name)}});
  return method;
}

auto BuiltinSignature(Builtin builtin) -> const FunctionDescriptor& {
  return kSignatures.at(static_cast<std::size_t>(builtin));
}

auto IsBuiltinName(std::string_view name) -> bool {
  return name == "int" || name == "double" || name == "bool" || name == "string" ||
         name == "void" || name == "ok" || name == "err" || name == "Expected" || name == "Set" ||
         name == "Map" || FindBuiltin(Kind::Invalid, name).has_value();
}

auto FindBuiltin(Kind receiver, std::string_view name) -> std::optional<Builtin> {
  if (receiver == Kind::String) {
    constexpr auto methods = std::to_array<std::pair<std::string_view, Builtin>>({
        {"codePoints", Builtin::StringCodePoints},
        {"graphemes", Builtin::StringGraphemes},
        {"graphemeSlice", Builtin::StringGraphemeSlice},
        {"slice", Builtin::StringSlice},
        {"indexOf", Builtin::StringIndexOf},
        {"contains", Builtin::StringContains},
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
    if (name == "StringBuilder") {
      return Builtin::NewStringBuilder;
    }
  } else if ((receiver == Kind::Int || receiver == Kind::Double) && name == "toInt") {
    return Builtin::NumericToInt;
  } else if ((receiver == Kind::Int || receiver == Kind::Double) && name == "toDouble") {
    return Builtin::NumericToDouble;
  } else if ((receiver == Kind::Int || receiver == Kind::Double || receiver == Kind::Bool) &&
             name == "toString") {
    return Builtin::ScalarToString;
  } else if (receiver == Kind::StringBuilder) {
    if (name == "append") {
      return Builtin::BuilderAppend;
    }
    if (name == "toString") {
      return Builtin::BuilderToString;
    }
  }
  return std::nullopt;
}

}  // namespace vela::frontend
