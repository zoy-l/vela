#pragma once

#include <string>
#include <string_view>

#include "compiler/frontend/typing/type_checker.h"

namespace vela::tooling {

[[nodiscard]] std::string_view BuiltinDocumentation(frontend::TypeDescriptor::Kind receiver,
                                                    std::string_view name);
[[nodiscard]] std::string_view BuiltinTypeDetail(std::string_view name);
[[nodiscard]] std::string BuiltinDetail(const frontend::FunctionDescriptor& signature);

}  // namespace vela::tooling
