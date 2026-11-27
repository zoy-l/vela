#include "base/inline_layout.h"

#include <algorithm>
#include <span>

namespace vela {

InlineLayout::InlineLayout(std::span<const InlineSlotKind> kinds) {
  slots_.reserve(kinds.size());
  for (const auto kind : kinds) {
    const auto alignment = InlineSlotAlignment(kind);
    alignment_ = std::max(alignment_, alignment);
    size_ = AlignInline(size_, alignment);
    slots_.push_back({.kind = kind, .offset = size_});
    size_ += InlineSlotSize(kind);
    references_ |= kind == InlineSlotKind::Reference || kind == InlineSlotKind::String ||
                   kind == InlineSlotKind::Dynamic;
  }
  size_ = AlignInline(size_, alignment_);
}

}  // namespace vela
