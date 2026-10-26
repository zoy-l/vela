#include "compiler/frontend/byte_builtins.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

struct Parameter final {
  std::string_view name;
  TypeDescriptor type;
};

FunctionDescriptor Signature(TypeDescriptor result, std::initializer_list<Parameter> parameters) {
  FunctionDescriptor signature;
  signature.return_type = std::move(result);
  for (const auto& parameter : parameters) {
    signature.parameter_indices.emplace(parameter.name,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back({.name = std::string(parameter.name), .type = parameter.type});
  }
  return signature;
}

constexpr std::size_t Index(Builtin builtin) {
  return static_cast<std::size_t>(builtin) - static_cast<std::size_t>(Builtin::NewByteArray);
}

}  // namespace

const FunctionDescriptor& ByteBuiltinSignature(Builtin builtin) {
  static const auto signatures = [] {
    std::array<FunctionDescriptor, Index(Builtin::ByteDataSetFloat64) + 1> result;
    const TypeDescriptor integer{.kind = Kind::Int, .name = "int"};
    const TypeDescriptor number{.kind = Kind::Double, .name = "double"};
    const TypeDescriptor nothing{.kind = Kind::Void, .name = "void"};
    const TypeDescriptor bytes{.kind = Kind::ByteArray, .name = "ByteArray"};
    const TypeDescriptor data{.kind = Kind::ByteData, .name = "ByteData"};
    const TypeDescriptor endian{.kind = Kind::Enum, .name = "Endian"};
    result.at(Index(Builtin::NewByteArray)) =
        Signature(bytes, {{.name = "length", .type = integer}});
    result.at(Index(Builtin::ByteArrayFrom)) =
        Signature(bytes, {{.name = "values", .type = {.kind = Kind::Array, .name = "int[]"}}});
    result.at(Index(Builtin::ByteArrayLength)) = Signature(integer, {});
    result.at(Index(Builtin::ByteArraySlice)) =
        Signature(bytes, {{.name = "start", .type = integer}, {.name = "end", .type = integer}});
    result.at(Index(Builtin::ByteArraySlice)).optional_parameters = {"start", "end"};
    result.at(Index(Builtin::ByteDataView)) =
        Signature(data, {
                            {.name = "bytes", .type = bytes},
                            {.name = "offset", .type = integer},
                            {.name = "length", .type = integer},
                        });
    result.at(Index(Builtin::ByteDataView)).optional_parameters = {"offset", "length"};
    result.at(Index(Builtin::ByteDataLength)) = Signature(integer, {});
    for (const auto& access : kByteAccesses) {
      const auto value = access.number == ByteNumber::Float ? number : integer;
      auto getter = Signature(value, {{.name = "offset", .type = integer}});
      auto setter = Signature(
          nothing, {{.name = "offset", .type = integer}, {.name = "value", .type = value}});
      for (auto* signature : {&getter, &setter}) {
        if (access.width != 1) {
          signature->parameter_indices.emplace(
              "endian", static_cast<std::uint32_t>(signature->parameters.size()));
          signature->parameters.push_back({.name = "endian", .type = endian});
          signature->optional_parameters.insert("endian");
        }
      }
      result.at(Index(access.get)) = std::move(getter);
      result.at(Index(access.set)) = std::move(setter);
    }
    return result;
  }();
  return signatures.at(Index(builtin));
}

std::span<const std::pair<std::string_view, Builtin>> ByteMethods(Kind receiver) {
  if (receiver == Kind::ByteArray) {
    static constexpr auto methods = std::to_array<std::pair<std::string_view, Builtin>>({
        {"slice", Builtin::ByteArraySlice},
    });
    return methods;
  }
  if (receiver == Kind::ByteData) {
    static constexpr auto methods = [] {
      std::array<std::pair<std::string_view, Builtin>, kByteAccesses.size() * 2> result;
      for (std::size_t index = 0; index < kByteAccesses.size(); ++index) {
        const auto& access = kByteAccesses.at(index);
        result.at(index * 2) = {access.getter, access.get};
        result.at((index * 2) + 1) = {access.setter, access.set};
      }
      return result;
    }();
    return methods;
  }
  return {};
}

std::optional<Builtin> FindByteBuiltin(Kind receiver, std::string_view name) {
  if (receiver == Kind::Invalid && name == "ByteArray") {
    return Builtin::NewByteArray;
  }
  for (const auto& [member, builtin] : ByteMethods(receiver)) {
    if (member == name) {
      return builtin;
    }
  }
  return std::nullopt;
}

}  // namespace vela::frontend
