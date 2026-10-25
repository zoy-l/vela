#include "compiler/frontend/builtins.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
                  {.name = "parts", .kind = Kind::Array, .type = "string[]"},
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
    if (name == "join") {
      return Builtin::Join;
    }
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
