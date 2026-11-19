#pragma once

#include <string_view>

#include "base/builtin.h"

namespace vela::compiler::native {

inline constexpr std::string_view kDevelopmentBuiltinSymbol = "vela_development_v1_builtin";

[[nodiscard]] constexpr bool IsDevelopmentBuiltin(Builtin builtin) noexcept {
  return builtin == Builtin::TestRegister || builtin == Builtin::TestInspect ||
         builtin == Builtin::TestSame || builtin == Builtin::TestControl ||
         builtin == Builtin::MockInspect;
}

}  // namespace vela::compiler::native
