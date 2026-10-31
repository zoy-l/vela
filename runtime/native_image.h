#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "base/source_location.h"
#include "runtime/native_abi.h"

namespace vela::runtime {

inline constexpr std::uint32_t kNoNativeIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kNativeOptionalParameter = 1;
inline constexpr std::uint32_t kNativeRestParameter = 2;
inline constexpr std::uint32_t kNativeAsyncFunction = 1;
inline constexpr std::uint32_t kNativeExportedFunction = 2;
inline constexpr std::uint32_t kNativeTypedFunction = 4;
// 内部入口返回 enum 的标量载荷；进入 runtime Value 表示时需要盒装。
inline constexpr std::uint32_t kNativeEnumResult = 8;
inline constexpr std::uint32_t kNativeGeneratedConstructor = 16;
inline constexpr std::uint32_t kNativePrivateFunction = 32;
inline constexpr std::uint32_t kNativePrivateField = 1;

// 这些记录是生成代码可见的内部 ABI：仅包含固定宽度字段、重定位指针和长度。
// 描述表及所有借用字节均不可变，必须覆盖引用它们的程序状态、闭包与续延的生命周期。
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

struct NativeFieldDescription final {
  DeclarationId declaration;
  std::uint32_t type = 0;
  std::uint32_t flags = 0;
  std::uint32_t initializer = kNoNativeIndex;
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
  DeclarationId declaration;
  NativeTable<NativeFieldDescription> fields;
  DeclarationId base_class;
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

struct NativeConstructionDescription final {
  DeclarationId type;
  NativeTable<DeclarationId> supplied;
};

struct NativeBindingDependency final {
  DeclarationId target;
  DeclarationId receiver;
};

struct NativeDependencyBlock final {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  NativeTable<std::uint32_t> successors;
};

struct NativeFunctionDependencies final {
  NativeTable<DeclarationId> fields;
  NativeTable<DeclarationId> globals;
  NativeTable<std::uint32_t> versions;
  NativeTable<NativeConstructionDescription> constructions;
  DeclarationId lexical_class;
  NativeTable<NativeBindingDependency> bindings;
  NativeTable<DeclarationId> types;
  // 按 fields、globals、versions、constructions、bindings 的顺序对应每条依赖的原 IR 位置。
  NativeTable<std::uint32_t> positions;
  NativeTable<NativeDependencyBlock> blocks;
};

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
  FunctionIdentity identity;
  const NativeFunctionDependencies* dependencies = nullptr;
};

struct NativeMemberDescription final {
  NativeText name;
  std::uint32_t receiver_type = 0;
  std::uint32_t target = 0;
  std::uint32_t type = kNoNativeIndex;
  std::uint32_t setter = kNoNativeIndex;
  std::uint32_t kind = 0;
};

struct NativeDeclarationDescription final {
  DeclarationId identity;
  DeclarationId owner;
  std::uint32_t kind = 0;
  NativeText name;
  NativeTable<NativeText> type_parameters;
  NativeTable<NativeText> enum_members;
  NativeText generic_signature;
  DeclarationId definition;
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
  std::uint64_t revision = 0;
  NativeTable<NativeText> source_texts;
  NativeTable<NativeDeclarationDescription> declarations;
  NativeTable<DeclarationId> interface_declarations;
  NativeTable<DeclarationId> global_declarations;
  NativeTable<DeclarationId> selector_declarations;
  std::uint32_t native_mode = static_cast<std::uint32_t>(NativeProgramMode::Closed);
  NativeTable<std::uint32_t> global_flags;
};

[[nodiscard]] std::string_view NativeRuntimeAbiIdentity() noexcept;
[[nodiscard]] std::string_view NativeTargetIdentity() noexcept;

}  // namespace vela::runtime
