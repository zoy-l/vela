#pragma once

#include <absl/container/flat_hash_set.h>

#include "runtime/value/object.h"

namespace vela::runtime {

struct ProgramLayouts;

// 只有可重载实例化创建的稳定元数据参与回收；宿主/封闭程序创建的元数据保持 Isolate 寿命。
struct NativeMetadataSet final {
  absl::flat_hash_set<const ClassLayout*> classes;
  absl::flat_hash_set<const NominalType*> nominals;
  absl::flat_hash_set<const ArrayLayout*> arrays;
  absl::flat_hash_set<const CollectionLayout*> collections;
  absl::flat_hash_set<const TypeSignature*> signatures;
  absl::flat_hash_set<const InterfaceType*> interfaces;
  [[nodiscard]] bool IsEmpty() const noexcept;
};

// 对堆清扫后仍存活的布局图做一次闭包遍历，不保活版本代码，不改变字段或名义身份。
class NativeMetadataTracer final {
 public:
  void Mark(const ClassLayout* layout);
  void Mark(const NominalType* nominal);
  void Mark(const ArrayLayout* layout);
  void Mark(const CollectionLayout* layout);
  void Mark(const TypeSignature* signature);
  void Mark(const InterfaceType* interface);
  void Mark(TypeConstraint type);
  void Mark(const ProgramLayouts& layouts);
  [[nodiscard]] const NativeMetadataSet& reached() const noexcept { return reached_; }

 private:
  NativeMetadataSet reached_;
};

}  // namespace vela::runtime
