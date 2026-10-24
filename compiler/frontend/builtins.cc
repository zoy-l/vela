#include "compiler/frontend/builtins.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

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
    function.parameter_order.emplace_back(parameter.name);
    function.parameters.emplace(
        parameter.name,
        TypeDescriptor{.kind = parameter.kind, .name = std::string(parameter.type)});
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
});
static_assert(kSignatures.size() == static_cast<std::size_t>(Builtin::Count));

}  // namespace

auto BuiltinSignature(Builtin builtin) -> const FunctionDescriptor& {
  return kSignatures.at(static_cast<std::size_t>(builtin));
}

auto FindBuiltin(Kind receiver, std::string_view name) -> std::optional<Builtin> {
  if (receiver == Kind::Invalid) {
    if (name == "join") {
      return Builtin::Join;
    }
    if (name == "StringBuilder") {
      return Builtin::NewStringBuilder;
    }
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
  } else if (receiver == Kind::String) {
    if (name == "slice") {
      return Builtin::StringSlice;
    }
    if (name == "indexOf") {
      return Builtin::StringIndexOf;
    }
    if (name == "contains") {
      return Builtin::StringContains;
    }
    if (name == "startsWith") {
      return Builtin::StringStartsWith;
    }
    if (name == "endsWith") {
      return Builtin::StringEndsWith;
    }
  }
  return std::nullopt;
}

}  // namespace vela::frontend
