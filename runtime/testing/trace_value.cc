#include "runtime/testing/trace_value.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "runtime/heap/heap.h"

namespace vela::runtime {

std::expected<Value, ExecutionStatus> CaptureTestTrace(Heap& heap,
                                                       std::span<const FutureTraceFrame> frames) {
  BinaryWriter writer;
  writer.Write(static_cast<std::uint64_t>(frames.size()));
  for (const auto& frame : frames) {
    writer.Write(frame.function);
    writer.Write(frame.source_path);
    writer.Write(frame.line);
    writer.Write(frame.column);
    writer.Write(static_cast<std::uint64_t>(frame.instruction));
    writer.Write(frame.identity.declaration.value);
    writer.Write(frame.identity.revision);
    writer.Write(frame.identity.local);
  }
  const auto bytes = writer.bytes();
  auto* object = heap.AllocateByteArray(bytes.size());
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  std::memcpy(Heap::ByteArrayBytes(*object).data(), bytes.data(), bytes.size());
  return Value::Object(object);
}

std::expected<std::vector<FutureTraceFrame>, ExecutionStatus> ReadTestTrace(Value value) {
  if (!value.IsObject() || !Heap::IsByteArray(*value.AsObject())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  BinaryReader reader(std::as_bytes(Heap::ByteArrayBytes(*value.AsObject())));
  constexpr auto kMinimumFrameBytes = 5 * sizeof(std::uint64_t) + 3 * sizeof(std::uint32_t);
  const auto count = reader.Count(kMinimumFrameBytes);
  if (!count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::vector<FutureTraceFrame> frames;
  frames.reserve(*count);
  for (std::size_t index = 0; index < *count; ++index) {
    auto function = reader.Text();
    auto path = reader.Text();
    const auto fields = reader.Record<std::uint32_t, std::uint32_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint32_t>();
    if (!function || !path || !fields) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto [line, column, instruction, declaration, revision, local] = *fields;
    frames.push_back({
        .function = std::move(*function),
        .source_path = std::move(*path),
        .line = line,
        .column = column,
        .instruction = static_cast<std::size_t>(instruction),
        .identity = {.declaration = {.value = declaration}, .revision = revision, .local = local},
    });
  }
  if (reader.remaining() != 0) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return frames;
}

}  // namespace vela::runtime
