#pragma once

#include "base/builtin.h"

namespace vela {

[[nodiscard]] constexpr bool IsRegExpBuiltin(Builtin builtin) noexcept {
  return builtin >= Builtin::NewRegExp && builtin <= Builtin::RegExpMatchNamedGroup;
}

[[nodiscard]] constexpr bool HasRegExpArrayLayout(Builtin builtin) noexcept {
  return builtin == Builtin::RegExpAllMatches || builtin == Builtin::RegExpMatchGroupNames ||
         builtin == Builtin::RegExpMatchGroups;
}

}  // namespace vela
