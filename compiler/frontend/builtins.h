#pragma once

#include <optional>
#include <string_view>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] auto FindBuiltin(TypeDescriptor::Kind receiver, std::string_view name)
    -> std::optional<Builtin>;
// 签名只描述显式参数；receiver 由检查与 lowering 独立传递。
[[nodiscard]] auto BuiltinSignature(Builtin builtin) -> const FunctionDescriptor&;
[[nodiscard]] auto IsBuiltinName(std::string_view name) -> bool;

}  // namespace vela::frontend
