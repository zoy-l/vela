#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vela {

// 与原生聚合值共用的内联叶子表示；嵌套结构体在进入布局前已展开。
enum class InlineSlotKind : std::uint8_t {
  Boolean,
  Integer,
  Double,
  Reference,
  String,
  Dynamic,
  NullableBoolean,
  NullableInteger,
  NullableDouble,
};

inline constexpr std::size_t kInlineWordBytes = sizeof(std::uint64_t);
inline constexpr std::size_t kInlineTaggedBytes = 2 * kInlineWordBytes;

// 普通对象的字段需要至少一槽来承载初始化状态；struct 内部的空值不占槽。
[[nodiscard]] constexpr std::uint32_t InlineFieldSlotCount(std::uint32_t payload_slots,
                                                           bool nullable) noexcept {
  return std::max(1U, payload_slots + (nullable ? 1U : 0U));
}

[[nodiscard]] constexpr std::size_t InlineSlotAlignment(InlineSlotKind kind) noexcept {
  return kind == InlineSlotKind::Boolean || kind == InlineSlotKind::NullableBoolean
             ? 1
             : kInlineWordBytes;
}

[[nodiscard]] constexpr std::size_t InlineSlotSize(InlineSlotKind kind) noexcept {
  switch (kind) {
    case InlineSlotKind::Boolean:
      return 1;
    case InlineSlotKind::NullableBoolean:
      return 2;
    case InlineSlotKind::Dynamic:
    case InlineSlotKind::NullableInteger:
    case InlineSlotKind::NullableDouble:
      return kInlineTaggedBytes;
    default:
      return kInlineWordBytes;
  }
}

[[nodiscard]] constexpr std::size_t AlignInline(std::size_t offset,
                                                std::size_t alignment) noexcept {
  return ((offset + alignment - 1) / alignment) * alignment;
}

class InlineLayout final {
 public:
  struct Slot final {
    InlineSlotKind kind;
    std::size_t offset;
  };

  InlineLayout() = default;
  explicit InlineLayout(std::span<const InlineSlotKind> kinds);

  [[nodiscard]] std::span<const Slot> slots() const noexcept { return slots_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool HasReferences() const noexcept { return references_; }
  [[nodiscard]] std::size_t PayloadOffset(bool nullable) const noexcept {
    return nullable ? AlignInline(1, alignment_) : 0;
  }
  [[nodiscard]] std::size_t Stride(bool nullable) const noexcept {
    return std::max<std::size_t>(1, AlignInline(PayloadOffset(nullable) + size_, alignment_));
  }

 private:
  std::vector<Slot> slots_;
  std::size_t size_ = 0;
  std::size_t alignment_ = 1;
  bool references_ = false;
};

}  // namespace vela
