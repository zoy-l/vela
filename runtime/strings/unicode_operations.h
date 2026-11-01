#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;

// text 为合法 UTF-8；目标数量必须等于 grapheme 数量，返回各段的字节终点。
[[nodiscard]] std::expected<void, ExecutionStatus> FillGraphemeBoundaries(
    std::string_view text,
    std::span<std::size_t> boundaries);

[[nodiscard]] std::expected<Value, ExecutionStatus>
ExecuteUnicodeBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap);

}  // namespace vela::runtime
