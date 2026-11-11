#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "base/execution_status.h"
#include "base/reflection.h"
#include "runtime/reflection/reflection_index.h"

namespace vela::runtime {

enum class TypeDescriptionDetail : std::uint8_t { Identity, Complete };

struct ReflectedSignatureParameter final {
  ReflectedType type;
  bool is_optional = false;
  bool is_rest = false;
};

struct ReflectedSignature final {
  std::vector<ReflectedSignatureParameter> parameters;
  ReflectedType return_type;
};

struct ReflectedTypeDescription final {
  std::string name;
  std::optional<std::string> module;
  reflection::TypeKind kind = reflection::TypeKind::Null;
  bool is_nullable = false;
  bool is_registered = false;
  std::vector<ReflectedType> type_arguments;
  std::optional<ReflectedType> base_type;
  std::vector<ReflectedType> interfaces;
  std::optional<ReflectedSignature> signature;
};

// 结构类型借用调用时已保根的类型图；绑定层须在释放借用前建立自己的类型根。
[[nodiscard]] std::expected<ReflectedTypeDescription, ExecutionStatus> DescribeReflectedType(
    ReflectedType type,
    const ReflectionIndex* index,
    TypeDescriptionDetail detail = TypeDescriptionDetail::Complete);

}  // namespace vela::runtime
