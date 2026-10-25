#include "vm/register_file.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include "runtime/register_storage.h"
#include "runtime/value.h"

namespace vela::vm {

static_assert(std::is_trivially_destructible_v<runtime::RegisterPayload>);

RegisterFile::~RegisterFile() {
  ::operator delete(values_);
}

runtime::RegisterView RegisterFile::Acquire(std::size_t count) noexcept {
  if (count > capacity_) {
    auto* values = static_cast<runtime::RegisterPayload*>(
        ::operator new(count * runtime::kRegisterSlotBytes, std::nothrow));
    if (values == nullptr) {
      return {};
    }
    std::uninitialized_value_construct_n(values, count);
    void* tag_storage = std::span(values, count).last(0).data();
    auto* kinds = static_cast<runtime::ValueKind*>(tag_storage);
    std::uninitialized_fill_n(kinds, count, runtime::ValueKind::Null);
    ::operator delete(values_);
    values_ = values;
    capacity_ = count;
    return {{values_, count}, {kinds, count}};
  }
  void* tag_storage = std::span(values_, capacity_).last(0).data();
  auto kinds = std::span(static_cast<runtime::ValueKind*>(tag_storage), count);
  // 复用前清理旧引用，避免把上一调用的临时对象作为新帧的 GC 根保留。
  std::ranges::fill(kinds, runtime::ValueKind::Null);
  return {{values_, count}, kinds};
}

}  // namespace vela::vm
