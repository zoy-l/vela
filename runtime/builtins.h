#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {

enum class BuiltinError : std::uint8_t {
  TypeError,
  OutOfMemory,
  IndexOutOfBounds,
  InvalidNumericConversion,
  ConcurrentModification,
  IntParseError,
  DoubleParseError,
  InvalidRadix,
};

enum class NumericConversion : std::uint8_t { ToInt, ToDouble };

[[nodiscard]] std::expected<Value, BuiltinError> ConvertNumber(NumericConversion conversion,
                                                               Value value);

// 参数数量由字节码加载验证；引用参数借用已保根槽位，结果立即存入目标槽位。
[[nodiscard]] std::expected<Value, BuiltinError> ExecuteBuiltin(Builtin builtin,
                                                                std::span<const Value> arguments,
                                                                Heap& heap);

}  // namespace vela::runtime
