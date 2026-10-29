#pragma once

#include <expected>
#include <string>

#include "runtime/native_image.h"

namespace vela::runtime {

struct NativeContractViolation final {
  DeclarationId declaration;
  std::string message;
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

}  // namespace vela::runtime
