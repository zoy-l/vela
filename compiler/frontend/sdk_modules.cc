#include "compiler/frontend/sdk_modules.h"

#include <optional>
#include <string_view>

#include "base/builtin.h"
#include "compiler/frontend/ast.h"
#include "compiler/sdk_sources.h"

namespace vela::frontend {

std::optional<std::string_view> SdkModuleSource(std::string_view identity) noexcept {
  if (identity == "sdk:test") {
    return internal::kTestSource;
  }
  return std::nullopt;
}

void BindSdkDeclarations(std::string_view identity, Program& program) {
  if (identity != "sdk:test") {
    return;
  }
  for (auto& function : program.functions) {
    if (function.name == "_publish") {
      function.intrinsic = Builtin::TestRegister;
      function.native_symbol.clear();
    } else if (function.name == "_inspect") {
      function.intrinsic = Builtin::TestInspect;
      function.native_symbol.clear();
    } else if (function.name == "_same") {
      function.intrinsic = Builtin::TestSame;
      function.native_symbol.clear();
    } else if (function.name == "_control") {
      function.intrinsic = Builtin::TestControl;
      function.native_symbol.clear();
    } else if (function.name == "test" || function.name == "group") {
      function.optional_parameters = {"skip", "timeout", "tags"};
    } else if (function.name == "expect" || function.name == "expectLater") {
      function.optional_parameters = {"reason"};
    } else if (function.name == "expectAsync0" || function.name == "expectAsync1") {
      function.optional_parameters = {"count", "max"};
    }
  }
}

}  // namespace vela::frontend
