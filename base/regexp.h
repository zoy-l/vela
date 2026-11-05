#pragma once

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

}  // namespace vela
