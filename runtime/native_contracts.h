#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/declaration.h"
#include "runtime/native_image.h"
#include "runtime/native_version.h"

namespace vela::runtime {

enum class NativeContractStatus : std::uint8_t { RequiresReload, NeedsCompilation };

struct NativeContractViolation final {
  DeclarationId declaration;
  std::string message;
  std::optional<FunctionIdentity> dependency = std::nullopt;
  NativeContractStatus status = NativeContractStatus::RequiresReload;
};

// 比较语义类型及命名实参契约；产物内索引、源码名字和机器地址不构成类型身份。
[[nodiscard]] bool IsSameNativeType(const NativeImage& left_image,
                                    const NativeTypeDescription& left,
                                    const NativeImage& right_image,
                                    const NativeTypeDescription& right);
[[nodiscard]] std::expected<void, std::string> CheckNativeCallContract(
    const NativeImage& previous_image,
    const NativeFunctionDescription& previous,
    const NativeImage& candidate_image,
    const NativeFunctionDescription& candidate);
[[nodiscard]] std::expected<void, NativeContractViolation> CheckNativeNominalContracts(
    const NativeImage& previous,
    const NativeImage& candidate);
[[nodiscard]] std::expected<void, NativeContractViolation> CheckRetainedNativeContracts(
    std::span<const NativeRetainedFunction> retained,
    const NativeImage* previous,
    const NativeImage& candidate);
[[nodiscard]] std::vector<DeclarationId> RetainedNativeDeclarations(
    std::span<const NativeRetainedFunction> functions);
[[nodiscard]] std::expected<void, NativeContractViolation> CheckRetainedNativeTypes(
    std::span<const DeclarationId> declarations,
    const NativeImage& candidate);

}  // namespace vela::runtime
