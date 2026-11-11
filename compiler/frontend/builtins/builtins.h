#pragma once

#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "base/array_algorithm.h"
#include "base/builtin.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {

struct BuiltinParameter final {
  std::string_view name;
  TypeDescriptor type;
};

[[nodiscard]] FunctionDescriptor MakeBuiltinSignature(
    TypeDescriptor result,
    std::initializer_list<BuiltinParameter> parameters);

[[nodiscard]] std::optional<Builtin> FindBuiltin(TypeDescriptor::Kind receiver,
                                                 std::string_view name);
[[nodiscard]] std::optional<Builtin> FindBuiltinProperty(TypeDescriptor::Kind receiver,
                                                         std::string_view name);
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> BuiltinMethods(
    TypeDescriptor::Kind receiver) noexcept;
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> BuiltinProperties(
    TypeDescriptor::Kind receiver) noexcept;
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> CollectionMethods(
    TypeDescriptor::Kind receiver) noexcept;
// 签名只描述显式参数；receiver 由检查与 lowering 独立传递。
[[nodiscard]] const FunctionDescriptor& BuiltinSignature(Builtin builtin);
[[nodiscard]] bool IsBuiltinName(std::string_view name);
[[nodiscard]] bool CanFormatString(const TypeDescriptor& type) noexcept;
[[nodiscard]] std::optional<ArrayAlgorithm> FindArrayAlgorithm(std::string_view name);
[[nodiscard]] std::span<const std::pair<std::string_view, ArrayAlgorithm>>
ArrayAlgorithms() noexcept;
[[nodiscard]] std::span<const std::pair<std::string_view, Builtin>> ArrayMethods() noexcept;
[[nodiscard]] std::optional<Builtin> FindArrayMethod(std::string_view name);
[[nodiscard]] FunctionDescriptor ArrayMethodSignature(Builtin builtin,
                                                      const TypeDescriptor& element);
struct CollectionMethod final {
  Builtin operation = Builtin::Count;
  FunctionDescriptor signature;
};
[[nodiscard]] std::optional<CollectionMethod> ResolveCollectionBuiltin(
    const TypeDescriptor& receiver,
    std::span<const TypeDescriptor> arguments,
    std::string_view name);
[[nodiscard]] FunctionDescriptor ArrayCallbackSignature(ArrayAlgorithm algorithm,
                                                        const TypeDescriptor& element,
                                                        const TypeDescriptor& accumulator);
[[nodiscard]] FunctionDescriptor ArrayAlgorithmSignature(ArrayAlgorithm algorithm,
                                                         const FunctionDescriptor& callback);

}  // namespace vela::frontend
