#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "base/source_location.h"
#include "runtime/native_abi.h"

namespace vela::runtime {

inline constexpr std::uint32_t kNoNativeIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kNativeOptionalParameter = 1;
inline constexpr std::uint32_t kNativeRestParameter = 2;
inline constexpr std::uint32_t kNativeAsyncFunction = 1;
inline constexpr std::uint32_t kNativeExportedFunction = 2;
inline constexpr std::uint32_t kNativeTypedFunction = 4;

// 这些记录是生成代码可见的内部 ABI：仅包含固定宽度字段、重定位指针和长度。
// 目标文件静态链接后保持只读；描述表及所有借用字节必须覆盖实例的生命周期。
struct NativeText final {
  const char* data = nullptr;
  std::uint64_t size = 0;
  [[nodiscard]] std::string_view text() const noexcept { return {data, size}; }
};

template <class T>
struct NativeTable final {
  const T* data = nullptr;
  std::uint64_t size = 0;
  [[nodiscard]] std::span<const T> values() const noexcept { return {data, size}; }
};

struct NativeTypeDescription final {
  std::uint32_t kind = 0;
  std::uint32_t nullable = 0;
  std::uint32_t index = 0;
};

struct NativeClassDescription final {
  NativeText name;
  NativeTable<std::uint32_t> reference_fields;
  NativeTable<std::uint32_t> interfaces;
  NativeTable<std::uint32_t> dispatch_targets;
  std::uint32_t field_count = 0;
  std::uint32_t kind = 0;
  std::uint32_t callable_function = kNoNativeIndex;
  std::uint32_t callable_mode = 0;
  std::uint32_t callable_type = kNoNativeIndex;
  std::uint32_t expected_type = kNoNativeIndex;
};

struct NativeArrayDescription final {
  NativeText name;
  NativeTypeDescription element;
};

struct NativeCollectionDescription final {
  NativeText name;
  NativeTypeDescription key;
  NativeTypeDescription value;
  std::uint32_t has_value = 0;
};

struct NativeSignatureParameter final {
  NativeTypeDescription type;
  std::uint32_t flags = 0;
};

struct NativeSignatureDescription final {
  NativeText name;
  NativeTable<NativeSignatureParameter> parameters;
  NativeTypeDescription result;
  std::uint32_t has_result = 0;
};

struct NativeCallParameter final {
  NativeText name;
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
};

using NativeEntry = std::uint8_t (*)(NativeExecution*,
                                     const NativeValue*,
                                     std::uint64_t,
                                     NativeValue*) noexcept;

struct NativeFunctionDescription final {
  NativeText name;
  NativeText native_symbol;
  NativeTable<NativeCallParameter> parameters;
  NativeTable<SourceLocation> locations;
  NativeEntry entry = nullptr;
  std::uint32_t parameter_count = 0;
  std::uint32_t result_type = kNoNativeIndex;
  std::uint32_t hidden_parameters = 0;
  std::uint32_t flags = 0;
};

struct NativeMemberDescription final {
  NativeText name;
  std::uint32_t receiver_type = 0;
  std::uint32_t target = 0;
  std::uint32_t type = kNoNativeIndex;
  std::uint32_t setter = kNoNativeIndex;
  std::uint32_t kind = 0;
};

struct NativeImage final {
  std::uint32_t abi_version = kNativeAbiVersion;
  std::uint32_t record_bytes = sizeof(NativeImage);
  NativeText abi_identity;
  NativeText target;
  NativeTable<NativeText> interfaces;
  NativeTable<NativeClassDescription> classes;
  NativeTable<NativeArrayDescription> arrays;
  NativeTable<NativeCollectionDescription> collections;
  NativeTable<NativeSignatureDescription> signatures;
  NativeTable<NativeTypeDescription> types;
  NativeTable<std::uint32_t> array_types;
  NativeTable<std::uint32_t> collection_types;
  NativeTable<NativeText> strings;
  NativeTable<NativeText> source_paths;
  NativeTable<NativeFunctionDescription> functions;
  NativeTable<NativeMemberDescription> members;
  NativeTable<std::uint32_t> global_initializers;
};

[[nodiscard]] std::string_view NativeRuntimeAbiIdentity() noexcept;
[[nodiscard]] std::string_view NativeTargetIdentity() noexcept;

}  // namespace vela::runtime
