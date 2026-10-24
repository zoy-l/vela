#include "vm/register_file.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

#include "runtime/value.h"

namespace vela::vm {

static_assert(std::is_trivially_destructible_v<runtime::Value>);

RegisterFile::~RegisterFile() {
  ::operator delete(values_);
}

auto RegisterFile::Acquire(std::size_t count) noexcept -> std::span<runtime::Value> {
  if (count > capacity_) {
    auto* values =
        static_cast<runtime::Value*>(::operator new(count * sizeof(runtime::Value), std::nothrow));
    if (values == nullptr) {
      return {};
    }
    std::uninitialized_value_construct_n(values, count);
    ::operator delete(values_);
    values_ = values;
    capacity_ = count;
    return {values_, count};
  }
  auto registers = std::span(values_, count);
  // 复用前清理旧引用，避免把上一调用的临时对象作为新帧的 GC 根保留。
  std::ranges::fill(registers, runtime::Value::Null());
  return registers;
}

}  // namespace vela::vm
