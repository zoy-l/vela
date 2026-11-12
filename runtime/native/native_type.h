#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "base/layout_description.h"
#include "runtime/value/object.h"

namespace vela::runtime {

class Isolate;
struct LayoutCache;

struct NativeHostIdentity final {
  std::string_view key;
  const void* identity = nullptr;
};

// 借用 codec 的静态规范描述；无 type 表示 void，不绑定任何 Isolate 的本地身份。
struct NativeType final {
  std::optional<layout::Type> type = std::nullopt;
  const layout::Program* description = nullptr;
  std::span<const NativeHostIdentity> hosts;

  [[nodiscard]] bool IsVoid() const noexcept { return !type; }
  [[nodiscard]] bool IsHost() const noexcept;
  [[nodiscard]] bool IsNullable() const noexcept { return type && type->nullable; }
  [[nodiscard]] std::string_view host_key() const noexcept;
  [[nodiscard]] const void* cpp_identity() const noexcept;
  [[nodiscard]] const layout::Program& layouts() const noexcept;
  [[nodiscard]] std::string ContractKey() const;
  [[nodiscard]] bool operator==(const NativeType& other) const;
};

// 无脚本名义类型的契约可直接绑定 Isolate；脚本声明必须由显式实例类型能力解析。
[[nodiscard]] std::expected<TypeConstraint, ExecutionStatus> ResolveBuiltinNativeType(
    Isolate& isolate,
    NativeType type);

[[nodiscard]] TypeConstraint InstantiateNativeType(Isolate& isolate,
                                                   const layout::Program& layouts,
                                                   const layout::Type& type,
                                                   LayoutCache& cache);

}  // namespace vela::runtime
