#pragma once

#include <expected>
#include <memory>
#include <utility>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"

namespace vela::runtime::internal {

// 帧的地址覆盖一次遍历；空闲帧解除自己的根，复用时先 Reset 再 Attach。
// Frame 提供 parent、Reset、Attach 和 Detach；调用方在池析构前归还所有活动帧。
template <class Frame>
class FramePool final {
 public:
  explicit FramePool(Heap& heap) noexcept : heap_(&heap) {}
  ~FramePool() {
    while (spare_ != nullptr) {
      auto* frame = spare_;
      spare_ = frame->parent;
      std::destroy_at(frame);
      heap_->FreeNative(frame);
    }
  }
  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;
  FramePool(FramePool&&) = delete;
  FramePool& operator=(FramePool&&) = delete;

  template <class... Args>
  [[nodiscard]] std::expected<Frame*, ExecutionStatus> Acquire(Args&&... args) {
    if (spare_ != nullptr) {
      auto* frame = spare_;
      spare_ = frame->parent;
      frame->Reset(std::forward<Args>(args)...);
      frame->Attach(*heap_);
      return frame;
    }
    auto* storage = static_cast<Frame*>(heap_->AllocateNative(sizeof(Frame)));
    if (storage == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    return std::construct_at(storage, *heap_, std::forward<Args>(args)...);
  }

  void Release(Frame& frame) noexcept {
    frame.Detach();
    frame.parent = spare_;
    spare_ = &frame;
  }

 private:
  Heap* heap_;
  Frame* spare_ = nullptr;
};

}  // namespace vela::runtime::internal
