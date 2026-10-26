#include "compiler/frontend/regexp_builtins.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;
using Member = std::pair<std::string_view, Builtin>;

constexpr std::size_t Index(Builtin builtin) {
  return static_cast<std::size_t>(builtin) - static_cast<std::size_t>(Builtin::NewRegExp);
}

FunctionDescriptor Signature(TypeDescriptor result,
                             std::initializer_list<FunctionDescriptor::Parameter> parameters) {
  FunctionDescriptor signature;
  signature.return_type = std::move(result);
  for (const auto& parameter : parameters) {
    signature.parameter_indices.emplace(parameter.name,
                                        static_cast<std::uint32_t>(signature.parameters.size()));
    signature.parameters.push_back(parameter);
  }
  return signature;
}

}  // namespace

const FunctionDescriptor& RegExpBuiltinSignature(Builtin builtin) {
  static const auto signatures = [] {
    std::array<FunctionDescriptor, Index(Builtin::Count)> result;
    const TypeDescriptor integer{.kind = Kind::Int, .name = "int"};
    const TypeDescriptor boolean{.kind = Kind::Bool, .name = "bool"};
    const TypeDescriptor text{.kind = Kind::String, .name = "string"};
    const TypeDescriptor nullable_text{.kind = Kind::String, .name = "string?", .nullable = true};
    const TypeDescriptor pattern{.kind = Kind::RegExp, .name = "RegExp"};
    const TypeDescriptor match{.kind = Kind::RegExpMatch, .name = "RegExpMatch"};
    const TypeDescriptor nullable_match{
        .kind = Kind::RegExpMatch,
        .name = "RegExpMatch?",
        .nullable = true,
    };
    result.at(Index(Builtin::NewRegExp)) =
        Signature(pattern, {
                               {.name = "source", .type = text},
                               {.name = "multiLine", .type = boolean},
                               {.name = "caseSensitive", .type = boolean},
                               {.name = "dotAll", .type = boolean},
                           });
    result.at(Index(Builtin::NewRegExp)).optional_parameters = {
        "multiLine",
        "caseSensitive",
        "dotAll",
    };
    result.at(Index(Builtin::RegExpEscape)) = Signature(text, {{.name = "text", .type = text}});
    result.at(Index(Builtin::RegExpPattern)) = Signature(text, {});
    for (const auto property : {
             Builtin::RegExpIsMultiLine,
             Builtin::RegExpIsCaseSensitive,
             Builtin::RegExpIsDotAll,
             Builtin::RegExpIsUnicode,
         }) {
      result.at(Index(property)) = Signature(boolean, {});
    }
    for (const auto method : {
             Builtin::RegExpHasMatch,
             Builtin::RegExpFirstMatch,
             Builtin::RegExpAllMatches,
             Builtin::RegExpMatchAsPrefix,
             Builtin::RegExpStringMatch,
         }) {
      auto return_type = nullable_match;
      if (method == Builtin::RegExpHasMatch) {
        return_type = boolean;
      } else if (method == Builtin::RegExpAllMatches) {
        return_type = MakeArrayType(match);
      } else if (method == Builtin::RegExpStringMatch) {
        return_type = nullable_text;
      }
      auto signature = Signature(return_type, {{.name = "input", .type = text}});
      if (method == Builtin::RegExpAllMatches || method == Builtin::RegExpMatchAsPrefix) {
        signature.parameter_indices.emplace("start", 1);
        signature.parameters.push_back({.name = "start", .type = integer});
        signature.optional_parameters.insert("start");
      }
      result.at(Index(method)) = std::move(signature);
    }
    for (const auto property :
         {Builtin::RegExpMatchStart, Builtin::RegExpMatchEnd, Builtin::RegExpMatchGroupCount}) {
      result.at(Index(property)) = Signature(integer, {});
    }
    result.at(Index(Builtin::RegExpMatchInput)) = Signature(text, {});
    result.at(Index(Builtin::RegExpMatchPattern)) = Signature(pattern, {});
    result.at(Index(Builtin::RegExpMatchGroupNames)) = Signature(MakeArrayType(text), {});
    result.at(Index(Builtin::RegExpMatchGroup)) =
        Signature(nullable_text, {{.name = "index", .type = integer}});
    result.at(Index(Builtin::RegExpMatchNamedGroup)) =
        Signature(nullable_text, {{.name = "name", .type = text}});
    result.at(Index(Builtin::RegExpMatchGroups)) = Signature(
        MakeArrayType(nullable_text), {{.name = "indices", .type = MakeArrayType(integer)}});
    return result;
  }();
  return signatures.at(Index(builtin));
}

std::span<const Member> RegExpMethods(Kind receiver) {
  static constexpr auto pattern = std::to_array<Member>({
      {"hasMatch", Builtin::RegExpHasMatch},
      {"firstMatch", Builtin::RegExpFirstMatch},
      {"allMatches", Builtin::RegExpAllMatches},
      {"matchAsPrefix", Builtin::RegExpMatchAsPrefix},
      {"stringMatch", Builtin::RegExpStringMatch},
  });
  static constexpr auto match = std::to_array<Member>({
      {"group", Builtin::RegExpMatchGroup},
      {"groups", Builtin::RegExpMatchGroups},
      {"namedGroup", Builtin::RegExpMatchNamedGroup},
  });
  if (receiver == Kind::RegExp) {
    return pattern;
  }
  return receiver == Kind::RegExpMatch ? std::span<const Member>(match) : std::span<const Member>();
}

std::span<const Member> RegExpProperties(Kind receiver) {
  static constexpr auto pattern = std::to_array<Member>({
      {"pattern", Builtin::RegExpPattern},
      {"isMultiLine", Builtin::RegExpIsMultiLine},
      {"isCaseSensitive", Builtin::RegExpIsCaseSensitive},
      {"isDotAll", Builtin::RegExpIsDotAll},
      {"isUnicode", Builtin::RegExpIsUnicode},
  });
  static constexpr auto match = std::to_array<Member>({
      {"start", Builtin::RegExpMatchStart},
      {"end", Builtin::RegExpMatchEnd},
      {"input", Builtin::RegExpMatchInput},
      {"pattern", Builtin::RegExpMatchPattern},
      {"groupCount", Builtin::RegExpMatchGroupCount},
      {"groupNames", Builtin::RegExpMatchGroupNames},
  });
  if (receiver == Kind::RegExp) {
    return pattern;
  }
  return receiver == Kind::RegExpMatch ? std::span<const Member>(match) : std::span<const Member>();
}

std::optional<Builtin> FindRegExpBuiltin(Kind receiver, std::string_view name) {
  if (receiver == Kind::Invalid && name == "RegExp") {
    return Builtin::NewRegExp;
  }
  for (const auto& [member, builtin] : RegExpMethods(receiver)) {
    if (member == name) {
      return builtin;
    }
  }
  return std::nullopt;
}

}  // namespace vela::frontend
