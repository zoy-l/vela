#pragma once

#include "runtime/isolate.h"

namespace vela::runtime {

// 只覆盖编译/加载阶段尚未产生实例的新增布局；Commit 后才允许产物与实例逃逸。
class LayoutTransaction final {
 public:
  explicit LayoutTransaction(Isolate& isolate) noexcept
      : isolate_(&isolate), initial_(isolate.layout_counts()) {}
  ~LayoutTransaction();
  LayoutTransaction(const LayoutTransaction&) = delete;
  auto operator=(const LayoutTransaction&) -> LayoutTransaction& = delete;
  LayoutTransaction(LayoutTransaction&&) = delete;
  auto operator=(LayoutTransaction&&) -> LayoutTransaction& = delete;
  void Commit() noexcept { committed_ = true; }

 private:
  Isolate* isolate_;
  LayoutCounts initial_;
  bool committed_ = false;
};

}  // namespace vela::runtime
