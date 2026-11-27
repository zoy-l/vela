#pragma once

#include <span>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/layout_description.h"
#include "runtime/value/object.h"

namespace vela::runtime {

class Isolate;

// 编译布局的名义类型在封闭和可重载程序中必须一致。
[[nodiscard]] NominalKind ClassNominalKind(const layout::Class& description) noexcept;

struct ProgramLayouts final {
  std::vector<ClassLayout*> classes;
  std::vector<const ArrayLayout*> arrays;
  std::vector<const CollectionLayout*> collections;
  std::vector<const InterfaceType*> interfaces;
  std::vector<TypeConstraint> types;
};

// 只在类型解析期间借用描述中的不可变名称和 Isolate 元数据；描述须覆盖缓存的使用期。
// 程序长期保留按编号排列的 ProgramLayouts，不保留这个解析索引；失败的布局事务不得提交它。
struct LayoutCache final {
  const CodeIdentity* code_identity = nullptr;
  absl::flat_hash_map<std::string_view, ClassLayout*> classes;
  absl::flat_hash_map<std::string_view, const ArrayLayout*> arrays;
  absl::flat_hash_map<std::string_view, const CollectionLayout*> collections;
  absl::flat_hash_map<std::string_view, const TypeSignature*> signatures;
  absl::flat_hash_map<std::string_view, const InterfaceType*> interfaces;
};

// 消费受信任的编译产物；文件装载边界负责验证编号和结构图。
// 调用者持有 LayoutTransaction，并在程序整体实例化成功后提交。
[[nodiscard]] ProgramLayouts InstantiateLayouts(const layout::Program& description,
                                                Isolate& isolate,
                                                LayoutCache& cache,
                                                std::span<const ClassLayoutOptions> classes = {});

// 与整程序实例化共用解析器；只请求一个根类型，不复制描述图或创建结果表。
[[nodiscard]] TypeConstraint InstantiateLayoutType(const layout::Program& description,
                                                   layout::Type type,
                                                   Isolate& isolate,
                                                   LayoutCache& cache);

}  // namespace vela::runtime
