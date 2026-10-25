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
};

// 会话借用 isolate 的不可移动元数据；失败的编译事务不得提交此缓存。
struct LayoutCache final {
  absl::flat_hash_map<std::string, runtime::ClassLayout*> classes;
  absl::flat_hash_map<std::string, const runtime::ArrayLayout*> arrays;
  absl::flat_hash_map<std::string, const runtime::CollectionLayout*> collections;
};

[[nodiscard]] auto CreateClassLayouts(const ir::Program& program,
                                      runtime::Isolate& isolate,
                                      LayoutCache* cache = nullptr)
    -> std::vector<runtime::ClassLayout*>;

[[nodiscard]] auto CreateRuntimeLayouts(const ir::Program& program,
                                        const frontend::SemanticModel& model,
                                        std::span<runtime::ClassLayout* const> classes,
                                        runtime::Isolate& isolate,
                                        LayoutCache* cache = nullptr) -> RuntimeLayouts;

}  // namespace vela::compiler
