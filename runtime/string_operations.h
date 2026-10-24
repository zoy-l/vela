#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {

enum class StringError : std::uint8_t { TypeError, OutOfMemory, IndexOutOfBounds };

// builtin 及参数数量由字节码验证保证。参数借用已注册的根槽位，结果立即存回根槽。
[[nodiscard]] auto ExecuteStringBuiltin(Builtin builtin,
                                        std::span<const Value> arguments,
                                        Heap& heap) -> std::expected<Value, StringError>;

}  // namespace vela::runtime
