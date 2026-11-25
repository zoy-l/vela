#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

// 槽位连续且地址稳定；作用域按段拥有槽位，支持向外层存储和非 LIFO 释放。
// GC 只扫描各段已发布的前缀，已释放段在栈顶回收前也不会保活旧值。
class LocalHandleStack final {
 public:
  // 元数据紧邻首槽，避免常见的单值作用域为段头再触碰整个值区之后的缓存行。
  struct Segment final {
    static constexpr std::size_t kCapacity = 16;
    Segment* previous = nullptr;
    std::size_t count = 0;
    std::array<Value, kCapacity> values;
  };

  LocalHandleStack(Heap& heap, std::uint64_t identity) noexcept
      : heap_(&heap), identity_(identity) {}
  void Enter() noexcept { ++scopes_; }
  [[clang::always_inline]] void Leave(Segment* segment) noexcept {
    if (segment != nullptr) {
      auto* page = current_;
      for (; segment != nullptr; segment = segment->previous) {
        segment->count = 0;
      }
      auto* top = top_;
      while (top != page->segments.data() && std::prev(top)->count == 0) {
        top = std::prev(top);
      }
      top_ = top;
      if (page != &inline_page_ && top == page->segments.data()) [[unlikely]] {
        RewindPages();
      }
    }
    if (--scopes_ == 0 && inline_page_.next != nullptr) [[unlikely]] {
      ReleasePages();
    }
  }
  [[nodiscard, clang::always_inline]] std::expected<Value*, ExecutionStatus> StoreRooted(
      Segment*& segment,
      Value value) noexcept {
    if (segment == nullptr || segment->count == Segment::kCapacity) {
      auto* next = top_;
      if (next == std::span(current_->segments).last(0).data()) [[unlikely]] {
        const auto grown = Grow(value);
        if (!grown) {
          return std::unexpected(grown.error());
        }
        next = top_;
      }
      top_ = std::next(next);
      next->previous = segment;
      // 未使用段及退栈后的段都没有活跃槽；首槽无需读取再递增旧 count。
      next->count = 1;
      next->values.front() = value;
      segment = next;
      return &next->values.front();
    }
    auto& slot = segment->values[segment->count++];
    slot = value;
    return &slot;
  }
  void Trace() noexcept;
  void AppendExternalRoots(std::vector<ExternalResourceInfo>& result) const;

 private:
  // 四个常见嵌套作用域无需额外分配；每次扩容仍经过 Heap 的预算与 GC 边界。
  struct Page final {
    std::array<Segment, 4> segments;
    // 当前页只使用 top_；切换页面时才保存该页的边界。
    Segment* top = segments.data();
    Page* previous = nullptr;
    Page* next = nullptr;
  };
  [[nodiscard]] std::expected<void, ExecutionStatus> Grow(Value value) noexcept;
  void RewindPages() noexcept;
  void ReleasePages() noexcept;

  Heap* heap_;
  std::uint64_t identity_;
  std::size_t scopes_ = 0;
  Page inline_page_;
  Page* current_ = &inline_page_;
  Segment* top_ = inline_page_.segments.data();
};

}  // namespace vela::runtime
