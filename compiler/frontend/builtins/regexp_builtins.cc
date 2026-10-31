#include "compiler/frontend/builtins/regexp_builtins.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include "base/builtin.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

using Kind = TypeDescriptor::Kind;
using Member = std::pair<std::string_view, Builtin>;

constexpr std::size_t Index(Builtin builtin) {
  return static_cast<std::size_t>(builtin) - static_cast<std::size_t>(Builtin::NewRegExp);
}

struct MemberGroups final {
  std::span<const Member> pattern;
  std::span<const Member> match;
};

std::span<const Member> SelectMembers(Kind receiver, MemberGroups members) noexcept {
  if (receiver == Kind::RegExp) {
    return members.pattern;
  }
  return receiver == Kind::RegExpMatch ? members.match : std::span<const Member>();
}

}  // namespace

const FunctionDescriptor& RegExpBuiltinSignature(Builtin builtin) {
  static const auto signatures = [] {
    std::array<FunctionDescriptor, Index(Builtin::Count)> result;
    const TypeDescriptor integer{.kind = Kind::Int, .name = kIntTypeName};
    const TypeDescriptor boolean{.kind = Kind::Bool, .name = kBoolTypeName};
    const TypeDescriptor text{.kind = Kind::String, .name = kStringTypeName};
    const TypeDescriptor nullable_text{
        .kind = Kind::String,
        .name = kNullableStringTypeName,
        .nullable = true,
    };
    const TypeDescriptor pattern{.kind = Kind::RegExp, .name = kRegExpTypeName};
    const TypeDescriptor match{.kind = Kind::RegExpMatch, .name = kRegExpMatchTypeName};
    const TypeDescriptor nullable_match{
        .kind = Kind::RegExpMatch,
        .name = kNullableRegExpMatchTypeName,
        .nullable = true,
    };
    result.at(Index(Builtin::NewRegExp)) =
        MakeBuiltinSignature(pattern, {
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
    result.at(Index(Builtin::RegExpEscape)) =
        MakeBuiltinSignature(text, {{.name = "text", .type = text}});
    result.at(Index(Builtin::RegExpPattern)) = MakeBuiltinSignature(text, {});
    for (const auto property : {
             Builtin::RegExpIsMultiLine,
             Builtin::RegExpIsCaseSensitive,
             Builtin::RegExpIsDotAll,
             Builtin::RegExpIsUnicode,
         }) {
      result.at(Index(property)) = MakeBuiltinSignature(boolean, {});
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
      auto signature = MakeBuiltinSignature(return_type, {{.name = "input", .type = text}});
      if (method == Builtin::RegExpAllMatches || method == Builtin::RegExpMatchAsPrefix) {
        signature.AddParameter("start", integer, FunctionDescriptor::ParameterPresence::Optional);
      }
      result.at(Index(method)) = std::move(signature);
    }
    for (const auto property :
         {Builtin::RegExpMatchStart, Builtin::RegExpMatchEnd, Builtin::RegExpMatchGroupCount}) {
      result.at(Index(property)) = MakeBuiltinSignature(integer, {});
    }
    result.at(Index(Builtin::RegExpMatchInput)) = MakeBuiltinSignature(text, {});
    result.at(Index(Builtin::RegExpMatchPattern)) = MakeBuiltinSignature(pattern, {});
    result.at(Index(Builtin::RegExpMatchGroupNames)) =
        MakeBuiltinSignature(MakeArrayType(text), {});
    result.at(Index(Builtin::RegExpMatchGroup)) =
        MakeBuiltinSignature(nullable_text, {{.name = "index", .type = integer}});
    result.at(Index(Builtin::RegExpMatchNamedGroup)) =
        MakeBuiltinSignature(nullable_text, {{.name = "name", .type = text}});
    result.at(Index(Builtin::RegExpMatchGroups)) = MakeBuiltinSignature(
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
  return SelectMembers(receiver, {.pattern = pattern, .match = match});
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
  return SelectMembers(receiver, {.pattern = pattern, .match = match});
}

}  // namespace vela::frontend
