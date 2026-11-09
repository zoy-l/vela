#pragma once

#include <array>
#include <cstddef>

#include "base/builtin.h"

namespace vela {

struct RegExpOptions final {
  bool multi_line = false;
  bool case_sensitive = true;
  bool dot_all = false;
};

[[nodiscard]] constexpr bool IsRegExpBuiltin(Builtin builtin) noexcept {
  return builtin >= Builtin::NewRegExp && builtin <= Builtin::RegExpMatchNamedGroup;
}

[[nodiscard]] constexpr bool HasRegExpArrayLayout(Builtin builtin) noexcept {
  return builtin == Builtin::RegExpAllMatches || builtin == Builtin::RegExpMatchGroupNames ||
         builtin == Builtin::RegExpMatchGroups;
}

// 分类及运行时子分派使用闭合连续区间；重排或插入成员必须同时更新该契约。
static_assert([] {
  constexpr auto kOperations = std::to_array<Builtin>({
      Builtin::NewRegExp,          Builtin::RegExpEscape,          Builtin::RegExpPattern,
      Builtin::RegExpIsMultiLine,  Builtin::RegExpIsCaseSensitive, Builtin::RegExpIsDotAll,
      Builtin::RegExpIsUnicode,    Builtin::RegExpHasMatch,        Builtin::RegExpFirstMatch,
      Builtin::RegExpAllMatches,   Builtin::RegExpMatchAsPrefix,   Builtin::RegExpStringMatch,
      Builtin::RegExpMatchStart,   Builtin::RegExpMatchEnd,        Builtin::RegExpMatchInput,
      Builtin::RegExpMatchPattern, Builtin::RegExpMatchGroupCount, Builtin::RegExpMatchGroupNames,
      Builtin::RegExpMatchGroup,   Builtin::RegExpMatchGroups,     Builtin::RegExpMatchNamedGroup,
  });
  auto expected = static_cast<std::size_t>(Builtin::NewRegExp);
  for (const auto operation : kOperations) {
    if (static_cast<std::size_t>(operation) != expected++) {
      return false;
    }
  }
  return true;
}());

}  // namespace vela
