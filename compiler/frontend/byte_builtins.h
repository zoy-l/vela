#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] const FunctionDescriptor& ByteBuiltinSignature(Builtin builtin);
[[nodiscard]] std::optional<Builtin> FindByteBuiltin(TypeDescriptor::Kind receiver,
                                                     std::string_view name);
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> ByteMethods(
    TypeDescriptor::Kind receiver);

}  // namespace vela::frontend
