#include "vm/debug_info.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace vela::vm {

auto DebugInfo::Locate(std::size_t instruction) const noexcept -> const SourceMapping* {
  const auto next =
      std::ranges::upper_bound(mappings, instruction, {}, &SourceMapping::instruction);
  return next == mappings.begin() ? nullptr : &*std::prev(next);
}

}  // namespace vela::vm
