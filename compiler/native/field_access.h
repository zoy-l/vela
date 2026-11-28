#pragma once

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "base/declaration.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"

namespace vela::compiler::native {

struct FieldTarget final {
  ir::ClassId owner;
  ir::FieldId slot;
};

struct FieldStorage final {
  const frontend::TypeDescriptor* type;
  const ir::Class* owner;

  [[nodiscard]] bool HasInlineValue() const {
    return type->value_layout != nullptr &&
           (owner == nullptr || owner->kind != ir::Class::Kind::CaptureCell);
  }
};

// 编译计划借用同一份不可变 IR，通过类名和字段声明分别定位布局。
struct FieldTargets final {
  absl::flat_hash_map<DeclarationId, FieldTarget> declarations;
  absl::flat_hash_map<std::string_view, const ir::Class*> classes;

  [[nodiscard]] const ir::Class* FindClass(const frontend::TypeDescriptor& type) const;
  [[nodiscard]] FieldStorage Describe(const ir::Function& function,
                                      const ir::Instruction& instruction) const;
};

// 字段身份在 dynamic 擦除后仍保留；代码生成和缓存指纹共用同一个布局选择。
[[nodiscard]] FieldTargets FindFieldTargets(const ir::Program& program);

enum class FieldKindWrite : std::uint8_t { Required, Redundant };

// 同一基本块可复用标量或纯标量内联字段的标签；别名写入、寄存器改写和安全点使事实失效。
[[nodiscard]] std::vector<FieldKindWrite> FindFieldKindWrites(
    const ir::Function& function,
    const ir::Program& program,
    const FieldTargets& targets,
    std::span<const std::uint8_t> safepoints);

}  // namespace vela::compiler::native
