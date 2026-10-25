#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] auto FindBuiltin(TypeDescriptor::Kind receiver, std::string_view name)
    -> std::optional<Builtin>;
// 签名只描述显式参数；receiver 由检查与 lowering 独立传递。
[[nodiscard]] auto BuiltinSignature(Builtin builtin) -> const FunctionDescriptor&;
[[nodiscard]] auto IsBuiltinName(std::string_view name) -> bool;
[[nodiscard]] auto CanFormatString(const TypeDescriptor& type) noexcept -> bool;
[[nodiscard]] auto FindArrayAlgorithm(std::string_view name) -> std::optional<ArrayAlgorithm>;
[[nodiscard]] auto ArrayAlgorithms() noexcept
    -> std::span<const std::pair<std::string_view, ArrayAlgorithm>>;
[[nodiscard]] auto FindSetOperation(std::string_view name) -> std::optional<Builtin>;
[[nodiscard]] auto ArrayCallbackSignature(ArrayAlgorithm algorithm,
                                          const TypeDescriptor& element,
                                          const TypeDescriptor& accumulator) -> FunctionDescriptor;
[[nodiscard]] auto ArrayAlgorithmSignature(ArrayAlgorithm algorithm,
                                           const TypeDescriptor& element,
                                           const TypeDescriptor& accumulator) -> FunctionDescriptor;

}  // namespace vela::frontend
