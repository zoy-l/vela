#include "compiler/frontend/byte_builtins.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/byte_access.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;

constexpr std::size_t Index(Builtin builtin) {
  return static_cast<std::size_t>(builtin) - static_cast<std::size_t>(Builtin::NewByteArray);
}

}  // namespace

const FunctionDescriptor& ByteBuiltinSignature(Builtin builtin) {
  static const auto signatures = [] {
    std::array<FunctionDescriptor, Index(Builtin::ByteDataSetFloat64) + 1> result;
    const TypeDescriptor integer{.kind = Kind::Int, .name = kIntTypeName};
    const TypeDescriptor number{.kind = Kind::Double, .name = kDoubleTypeName};
    const TypeDescriptor nothing{.kind = Kind::Void, .name = kVoidTypeName};
    const TypeDescriptor bytes{.kind = Kind::ByteArray, .name = kByteArrayTypeName};
    const TypeDescriptor data{.kind = Kind::ByteData, .name = kByteDataTypeName};
    const TypeDescriptor endian{.kind = Kind::Enum, .name = kEndianTypeName};
    result.at(Index(Builtin::NewByteArray)) =
        MakeBuiltinSignature(bytes, {{.name = "length", .type = integer}});
    result.at(Index(Builtin::ByteArrayFrom)) = MakeBuiltinSignature(
        bytes, {{.name = "values", .type = {.kind = Kind::Array, .name = kIntArrayTypeName}}});
    result.at(Index(Builtin::ByteArrayLength)) = MakeBuiltinSignature(integer, {});
    result.at(Index(Builtin::ByteArrayGetIndex)) = MakeBuiltinSignature(
        integer, {{.name = "bytes", .type = bytes}, {.name = "index", .type = integer}});
    result.at(Index(Builtin::ByteArraySetIndex)) =
        MakeBuiltinSignature(nothing, {
                                          {.name = "bytes", .type = bytes},
                                          {.name = "index", .type = integer},
                                          {.name = "value", .type = integer},
                                      });
    result.at(Index(Builtin::ByteArraySlice)) = MakeBuiltinSignature(
        bytes, {{.name = "start", .type = integer}, {.name = "end", .type = integer}});
    result.at(Index(Builtin::ByteArraySlice)).optional_parameters = {"start", "end"};
    result.at(Index(Builtin::ByteDataView)) =
        MakeBuiltinSignature(data, {
                                       {.name = "bytes", .type = bytes},
                                       {.name = "offset", .type = integer},
                                       {.name = "length", .type = integer},
                                   });
    result.at(Index(Builtin::ByteDataView)).optional_parameters = {"offset", "length"};
    result.at(Index(Builtin::ByteDataLength)) = MakeBuiltinSignature(integer, {});
    for (const auto& access : kByteAccesses) {
      const auto value = access.number == ByteNumber::Float ? number : integer;
      auto getter = MakeBuiltinSignature(value, {{.name = "offset", .type = integer}});
      auto setter = MakeBuiltinSignature(
          nothing, {{.name = "offset", .type = integer}, {.name = "value", .type = value}});
      for (auto* signature : {&getter, &setter}) {
        if (access.width != 1) {
          signature->AddParameter("endian", endian,
                                  FunctionDescriptor::ParameterPresence::Optional);
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

}  // namespace vela::frontend
