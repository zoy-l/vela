#pragma once

#include <span>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/layout_description.h"
#include "runtime/object.h"

namespace vela::runtime {

class Isolate;

struct ProgramLayouts final {
  std::vector<ClassLayout*> classes;
  std::vector<const ArrayLayout*> arrays;
  std::vector<const CollectionLayout*> collections;
  std::vector<const InterfaceType*> interfaces;
  std::vector<TypeConstraint> types;
};

// 会话借用 Isolate 的不可移动元数据；失败的布局事务不得提交此缓存。
struct LayoutCache final {
  const CodeIdentity* code_identity = nullptr;
  absl::flat_hash_map<std::string, ClassLayout*> classes;
  absl::flat_hash_map<std::string, const ArrayLayout*> arrays;
  absl::flat_hash_map<std::string, const CollectionLayout*> collections;
  absl::flat_hash_map<std::string, const TypeSignature*> signatures;
  absl::flat_hash_map<std::string, const InterfaceType*> interfaces;
};

// 消费受信任的编译产物；文件装载边界负责验证编号和结构图。
// 调用者持有 LayoutTransaction，并在程序整体实例化成功后提交。
[[nodiscard]] ProgramLayouts InstantiateLayouts(const layout::Program& description,
                                                Isolate& isolate,
                                                LayoutCache& cache,
                                                std::span<const ClassLayoutOptions> classes = {});

}  // namespace vela::runtime
