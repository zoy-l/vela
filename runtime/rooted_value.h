#pragma once

#include <memory>

#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::runtime {

// 句柄可复制/移动；共享不可变的稳定根槽。所属 Isolate 必须比所有副本更晚销毁。
// 与 Isolate 一样仅在所属线程使用；读取的裸 Value 不独立延长对象生命周期。
class RootedValue final {
 public:
  RootedValue(Isolate& isolate, Value value);

  [[nodiscard]] Value value() const noexcept;

 private:
  struct Storage final {
    Storage(Heap& heap, Value source)
        : value(source), root(heap, &value, 1, RootOwner::RootedValue) {}
    Value value;
    RootRange root;
  };
  std::shared_ptr<const Storage> storage_;
};

}  // namespace vela::runtime
