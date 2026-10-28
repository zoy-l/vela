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

NativeFrame::NativeFrame(Isolate& isolate, TracePosition position, NativeFrameSize size) noexcept
    : roots_(size.root_count),
      position_(position),
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

ExecutionStatus NativeExecution::Enter(std::uint32_t function, NativeFrameSize size) noexcept {
  if (frame_count_ == 0) {
    pending_exception_ = Value::Null();
    exception_allocation_failed_ = false;
  }
  if (frame_count_ == kMaxCallFrames) {
    return ExecutionStatus::StackOverflow;
  }
  auto& slot = frames_.at(frame_count_);
  const auto& frame = slot.emplace(isolate(), TracePosition{.function = function}, size);
  if (!frame.IsAllocated()) {
    slot.reset();
    return ExecutionStatus::OutOfMemory;
  }
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
  if (!frame) {
    std::unreachable();
  }
  return frame->roots();
}

void* NativeExecution::current_storage() const noexcept {
  const auto& frame = frames_.at(frame_count_ - 1);
  if (!frame) {
    std::unreachable();
  }
  return frame->storage();
}

std::uint32_t* NativeExecution::current_instruction() noexcept {
  auto& frame = frames_.at(frame_count_ - 1);
  if (!frame) {
    std::unreachable();
  }
  return frame->instruction_address();
}

extern "C" std::uint8_t vela_native_v1_frame_enter(NativeExecution* execution,
                                                   std::uint64_t root_count,
                                                   std::uint32_t function,
                                                   std::uint64_t storage_bytes) noexcept {
  if (execution == nullptr) {
    return static_cast<std::uint8_t>(ExecutionStatus::InvalidIsolate);
  }
  return static_cast<std::uint8_t>(
      execution->Enter(function, {
                                     .root_count = static_cast<std::size_t>(root_count),
                                     .storage_bytes = static_cast<std::size_t>(storage_bytes),
                                 }));
}

extern "C" void vela_native_v1_frame_exit(NativeExecution* execution) noexcept {
  execution->Exit();
}

extern "C" void* vela_native_v1_frame_storage(NativeExecution* execution) noexcept {
  return execution->current_storage();
}

extern "C" std::uint32_t* vela_native_v1_frame_instruction(NativeExecution* execution) noexcept {
  return execution->current_instruction();
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
