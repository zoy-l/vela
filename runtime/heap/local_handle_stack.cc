#include "runtime/heap/local_handle_stack.h"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/value/value.h"

namespace vela::runtime {

std::expected<void, ExecutionStatus> LocalHandleStack::Grow(Value value) noexcept {
  auto* next = current_->next;
  if (next == nullptr) {
    // 扩容可以收集；调用者交来的值尚未写入句柄栈，先独立保根。
    const RootRange source(*heap_, &value, 1, std::nullopt);
    auto* memory = heap_->AllocateNative(sizeof(Page));
    if (memory == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    next = std::construct_at(static_cast<Page*>(memory));
    next->previous = current_;
    current_->next = next;
  }
  current_->top = top_;
  current_ = next;
  top_ = next->top;
  return {};
}

void LocalHandleStack::RewindPages() noexcept {
  while (top_ == current_->segments.data() && current_->previous != nullptr) {
    current_->top = top_;
    current_ = current_->previous;
    top_ = TrimEmptySegments(current_->segments.data(), current_->top);
  }
}

void LocalHandleStack::ReleasePages() noexcept {
  auto* page = inline_page_.next;
  inline_page_.next = nullptr;
  current_ = &inline_page_;
  top_ = inline_page_.segments.data();
  while (page != nullptr) {
    auto* next = page->next;
    std::destroy_at(page);
    heap_->FreeNative(page);
    page = next;
  }
}

void LocalHandleStack::Trace() noexcept {
  for (auto* page = &inline_page_; page != nullptr; page = page->next) {
    auto* top = page == current_ ? top_ : page->top;
    for (const auto& segment : std::span(page->segments.data(), top)) {
      for (const auto value : std::span(segment.values).first(segment.count)) {
        heap_->MarkValue(value);
      }
    }
  }
}

void LocalHandleStack::AppendExternalRoots(std::vector<ExternalResourceInfo>& result) const {
  if (scopes_ != 0) {
    // 同一个 Isolate 句柄栈只有一个资源身份，references 只统计持有段的作用域。
    result.push_back(
        {.kind = ExternalResourceKind::RootRange, .identity = identity_, .references = scopes_});
  }
}

}  // namespace vela::runtime
