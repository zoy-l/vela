#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] const FunctionDescriptor& RegExpBuiltinSignature(Builtin builtin);
[[nodiscard]] std::optional<Builtin> FindRegExpBuiltin(TypeDescriptor::Kind receiver,
                                                       std::string_view name);
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> RegExpMethods(
    TypeDescriptor::Kind receiver);
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> RegExpProperties(
    TypeDescriptor::Kind receiver);

}  // namespace vela::frontend
