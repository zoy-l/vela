#pragma once

#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] const FunctionDescriptor& ByteBuiltinSignature(Builtin builtin);
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> ByteMethods(
    TypeDescriptor::Kind receiver);

}  // namespace vela::frontend
