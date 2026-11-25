#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/value/value.h"

namespace vela::runtime {

class Heap;
class StringObject;

[[nodiscard]] std::expected<StringObject*, ExecutionStatus>
SliceStringBytes(Heap& heap, const StringObject& string, std::int64_t start, std::int64_t end);
[[nodiscard]] StringObject* TrimString(Heap& heap, StringObject& string);

[[nodiscard]] std::expected<Value, ExecutionStatus>
ExecuteStringByteBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap);

}  // namespace vela::runtime
