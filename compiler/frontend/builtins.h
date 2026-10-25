#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "compiler/frontend/type_checker.h"

namespace vela::frontend {

[[nodiscard]] std::optional<Builtin> FindBuiltin(TypeDescriptor::Kind receiver,
                                                 std::string_view name);
[[nodiscard]] std::optional<Builtin> FindBuiltinProperty(TypeDescriptor::Kind receiver,
                                                         std::string_view name);
// 签名只描述显式参数；receiver 由检查与 lowering 独立传递。
[[nodiscard]] const FunctionDescriptor& BuiltinSignature(Builtin builtin);
[[nodiscard]] bool IsBuiltinName(std::string_view name);
[[nodiscard]] bool CanFormatString(const TypeDescriptor& type) noexcept;
[[nodiscard]] std::optional<ArrayAlgorithm> FindArrayAlgorithm(std::string_view name);
[[nodiscard]] std::span<const std::pair<std::string_view, ArrayAlgorithm>>
ArrayAlgorithms() noexcept;
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> ArrayMethods() noexcept;
[[nodiscard]] FunctionDescriptor ArrayMethodSignature(Builtin builtin,
                                                      const TypeDescriptor& receiver,
                                                      const TypeDescriptor& element);
[[nodiscard]] std::optional<Builtin> FindSetOperation(std::string_view name);
[[nodiscard]] FunctionDescriptor ArrayCallbackSignature(ArrayAlgorithm algorithm,
                                                        const TypeDescriptor& element,
                                                        const TypeDescriptor& accumulator);
[[nodiscard]] FunctionDescriptor ArrayAlgorithmSignature(ArrayAlgorithm algorithm,
                                                         const TypeDescriptor& element,
                                                         const TypeDescriptor& accumulator);

}  // namespace vela::frontend
