#include "runtime/native_abi.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <utility>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/exception.h"
#include "runtime/isolate.h"
#include "runtime/native_instance.h"
#include "runtime/value.h"

namespace vela::runtime {

NativeFrame::NativeFrame(Isolate& isolate, NativeFrameSize size) noexcept
    : roots_(size.root_count),
      storage_bytes_(size.storage_bytes),
      storage_(storage_bytes_ == 0 ? nullptr : ::operator new(storage_bytes_, std::nothrow)) {
  if (IsAllocated() && size.root_count != 0) {
    const auto roots = roots_.values();
    root_range_.Attach(isolate.heap(), roots.data(), roots.size());
  }
}

NativeFrame::~NativeFrame() {
  ::operator delete(storage_);
}

NativeExecution::NativeExecution(NativeInstance& instance) noexcept
    : instance_(&instance), exception_root_(isolate().heap(), &pending_exception_, 1) {}

Isolate& NativeExecution::isolate() const noexcept {
  return instance_->isolate();
}

NativeExecution::~NativeExecution() {
  while (frame_count_ != 0) {
    Exit();
  }
}

NativeInstanceScope::NativeInstanceScope(NativeExecution& execution,
                                         NativeInstance& instance) noexcept
    : execution_(&execution), previous_(execution.instance_) {
  execution.instance_ = &instance;
}

NativeInstanceScope::~NativeInstanceScope() {
  execution_->instance_ = previous_;
}

ExecutionStatus NativeExecution::PrepareEntry() noexcept {
  if (frame_count_ == 0) {
    pending_exception_ = Value::Null();
    exception_allocation_failed_ = false;
  }
  if (frame_count_ == kMaxCallFrames) {
    return ExecutionStatus::StackOverflow;
  }
  return ExecutionStatus::Returned;
}

ExecutionStatus NativeExecution::Enter(std::uint32_t function, NativeFrameSize size) noexcept {
  const auto prepared = PrepareEntry();
  if (prepared != ExecutionStatus::Returned) {
    return prepared;
  }
  if (size.root_count != 0 || size.storage_bytes != 0) {
    auto& slot = frames_.at(frame_count_);
    const auto& frame = slot.emplace(isolate(), size);
    if (!frame.IsAllocated()) {
      slot.reset();
      return ExecutionStatus::OutOfMemory;
    }
  }
  positions_.at(frame_count_) = {.function = function};
  ++frame_count_;
  return ExecutionStatus::Returned;
}

void NativeExecution::Exit() noexcept {
  --frame_count_;
  frames_.at(frame_count_).reset();
}

std::span<Value> NativeExecution::current_roots() noexcept {
  if (frame_count_ == 0) {
    return {};
  }
  auto& frame = frames_.at(frame_count_ - 1);
  return frame ? frame->roots() : std::span<Value>();
}

void* NativeExecution::current_storage() const noexcept {
  const auto& frame = frames_.at(frame_count_ - 1);
  return frame ? frame->storage() : nullptr;
}

std::uint32_t* NativeExecution::current_instruction() noexcept {
  return &positions_.at(frame_count_ - 1).instruction;
}

extern "C" std::uint8_t vela_native_v1_frame_enter(NativeExecution* execution,
                                                   std::uint64_t root_count,
                                                   std::uint32_t function,
                                                   std::uint64_t storage_bytes,
                                                   NativeFrameAccess* access) noexcept {
  if (execution == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidIsolate);
  }
  const auto status =
      execution->Enter(function, {
                                     .root_count = static_cast<std::size_t>(root_count),
                                     .storage_bytes = static_cast<std::size_t>(storage_bytes),
                                 });
  if (status == ExecutionStatus::Returned) {
    *access = {
        .instruction = execution->current_instruction(),
        .roots = execution->current_roots().data(),
        .storage = execution->current_storage(),
    };
  }
  return static_cast<std::uint8_t>(status);
}

extern "C" void vela_native_v1_frame_exit(NativeExecution* execution) noexcept {
  execution->Exit();
}

extern "C" std::uint8_t vela_native_v1_leaf_enter(NativeExecution* execution) noexcept {
  return static_cast<std::uint8_t>(execution != nullptr ? execution->PrepareEntry()
                                                        : ExecutionStatus::InvalidIsolate);
}

extern "C" std::uint8_t vela_native_v1_leaf_failure(NativeExecution* execution,
                                                    const TracePosition* position,
                                                    std::uint8_t status) {
  const auto entered = execution->Enter(position->function, {});
  if (entered != ExecutionStatus::Returned) {
    return static_cast<std::uint8_t>(entered);
  }
  *execution->current_instruction() = position->instruction;
  const auto failure = execution->HandleFailure(static_cast<ExecutionStatus>(status));
  execution->Exit();
  return static_cast<std::uint8_t>(failure);
}

NativeValue EncodeNativeValue(Value value) noexcept {
  NativeValue result;
  switch (value.kind()) {
    case ValueKind::Null:
      return result;
    case ValueKind::Boolean:
      result.kind = NativeValueKind::Boolean;
      result.payload = value.AsBoolean() ? 1 : 0;
      break;
    case ValueKind::Integer:
      result.kind = NativeValueKind::Integer;
      result.payload = std::bit_cast<std::uint64_t>(value.AsInteger());
      break;
    case ValueKind::Double:
      result.kind = NativeValueKind::Double;
      result.payload = std::bit_cast<std::uint64_t>(value.AsDouble());
      break;
    case ValueKind::Object:
      result.kind = NativeValueKind::Object;
      result.payload = std::bit_cast<std::uint64_t>(value.AsObject());
      break;
    case ValueKind::String:
      result.kind = NativeValueKind::String;
      result.payload = std::bit_cast<std::uint64_t>(value.AsString());
      break;
  }
  return result;
}

Value DecodeNativeValue(const NativeValue& value) noexcept {
  switch (value.kind) {
    case NativeValueKind::Null:
      return Value::Null();
    case NativeValueKind::Boolean:
      return Value::Boolean(value.payload != 0);
    case NativeValueKind::Integer:
      return Value::Integer(std::bit_cast<std::int64_t>(value.payload));
    case NativeValueKind::Double:
      return Value::Double(std::bit_cast<double>(value.payload));
    case NativeValueKind::Object:
      return Value::Object(std::bit_cast<HeapObject*>(value.payload));
    case NativeValueKind::String:
      return Value::String(std::bit_cast<StringObject*>(value.payload));
  }
  std::unreachable();
}

}  // namespace vela::runtime
