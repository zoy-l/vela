#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "compiler/ir/ir.h"

namespace vela::ir {

// 寄存器编号由 Function::register_count 界定；同一次分析的集合共享这一容量。
class RegisterSet final {
 public:
  explicit RegisterSet(std::size_t count) : words_((count + kWordBits - 1) / kWordBits) {}

  void Insert(RegisterId reg) {
    words_.at(reg / kWordBits) |= std::uint64_t{1} << (reg % kWordBits);
  }
  void Erase(RegisterId reg) {
    words_.at(reg / kWordBits) &= ~(std::uint64_t{1} << (reg % kWordBits));
  }
  [[nodiscard]] bool Has(RegisterId reg) const {
    return (words_.at(reg / kWordBits) & (std::uint64_t{1} << (reg % kWordBits))) != 0;
  }
  void Clear() { std::ranges::fill(words_, 0); }
  void UnionWith(const RegisterSet& other) {
    for (std::size_t index = 0; index < words_.size(); ++index) {
      words_.at(index) |= other.words_.at(index);
    }
  }
  void Subtract(const RegisterSet& other) {
    for (std::size_t index = 0; index < words_.size(); ++index) {
      words_.at(index) &= ~other.words_.at(index);
    }
  }
  [[nodiscard]] bool IsEmpty() const {
    return std::ranges::all_of(words_, [](std::uint64_t word) { return word == 0; });
  }
  void Swap(RegisterSet& other) noexcept { words_.swap(other.words_); }
  bool operator==(const RegisterSet&) const = default;

  template <class Visitor>
  void ForEach(const Visitor& visitor) const {
    for (std::size_t index = 0; index < words_.size(); ++index) {
      auto remaining = words_.at(index);
      while (remaining != 0) {
        const auto bit = static_cast<std::size_t>(std::countr_zero(remaining));
        visitor(static_cast<RegisterId>((index * kWordBits) + bit));
        remaining &= remaining - 1;
      }
    }
  }

 private:
  static constexpr auto kWordBits = std::numeric_limits<std::uint64_t>::digits;
  std::vector<std::uint64_t> words_;
};

}  // namespace vela::ir
