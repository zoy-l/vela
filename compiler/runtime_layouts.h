#pragma once

#include <absl/container/flat_hash_map.h>
#include <span>
#include <string>
#include <vector>

#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"
#include "runtime/isolate.h"
#include "runtime/object.h"

namespace vela::compiler {

struct RuntimeLayouts final {
  std::vector<const runtime::ArrayLayout*> arrays;
  std::vector<const runtime::CollectionLayout*> collections;
  std::vector<runtime::TypeConstraint> types;
};

// 会话借用 isolate 的不可移动元数据；失败的编译事务不得提交此缓存。
struct LayoutCache final {
  const runtime::CodeIdentity* code_identity = nullptr;
  absl::flat_hash_map<std::string, runtime::ClassLayout*> classes;
  absl::flat_hash_map<std::string, const runtime::ArrayLayout*> arrays;
  absl::flat_hash_map<std::string, const runtime::CollectionLayout*> collections;
  absl::flat_hash_map<std::string, const runtime::TypeSignature*> signatures;
  absl::flat_hash_map<std::string, const runtime::InterfaceType*> interfaces;
};

[[nodiscard]] std::vector<const runtime::InterfaceType*> CreateInterfaceTypes(
    const frontend::SemanticModel& model,
    runtime::Isolate& isolate,
    LayoutCache* cache = nullptr);

[[nodiscard]] std::vector<runtime::ClassLayout*> CreateClassLayouts(
    const ir::Program& program,
    std::span<const runtime::InterfaceType* const> interface_types,
    runtime::Isolate& isolate,
    LayoutCache* cache = nullptr);

[[nodiscard]] RuntimeLayouts CreateRuntimeLayouts(
    const ir::Program& program,
    const frontend::SemanticModel& model,
    std::span<runtime::ClassLayout* const> classes,
    std::span<const runtime::InterfaceType* const> interfaces,
    runtime::Isolate& isolate,
    LayoutCache* cache = nullptr);

}  // namespace vela::compiler
