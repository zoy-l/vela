#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/value.h"

namespace vela::runtime {

class Heap;
class ClassLayout;

inline constexpr std::uint32_t kErrorFieldCount = 2;
inline constexpr std::uint32_t kErrorMessageField = 0;
inline constexpr std::uint32_t kErrorCodeField = 1;

struct ErrorFields final {
  std::string_view message;
  std::string_view code;
};

[[nodiscard]] std::expected<Value, ExecutionStatus> CreateError(Heap& heap,
                                                                const ClassLayout& layout,
                                                                ErrorFields fields);

}  // namespace vela::runtime
