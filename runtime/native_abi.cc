#include "runtime/native_abi.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
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

NativeFrame::NativeFrame(Isolate& isolate, NativeFrameSize size, Value* stack_roots) noexcept
    : allocated_roots_(stack_roots == nullptr ? size.root_count : 0),
      roots_(stack_roots == nullptr ? allocated_roots_.values()
                                    : std::span(stack_roots, size.root_count)),
      storage_bytes_(size.storage_bytes),
      storage_(storage_bytes_ == 0 ? nullptr : ::operator new(storage_bytes_, std::nothrow)) {
  if (IsAllocated() && size.root_count != 0) {
    if (stack_roots != nullptr) {
      std::uninitialized_value_construct_n(stack_roots, size.root_count);
    }
    root_range_.Attach(isolate.heap(), roots_.data(), roots_.size());
  }
}

NativeFrame::~NativeFrame() {
  ::operator delete(storage_);
}

NativeExecution::NativeExecution(NativeInstance& instance) noexcept
    : instance_(&instance),
      isolate_(&instance.isolate()),
      exception_root_(isolate().heap(), &pending_exception_, 1) {}

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

ExecutionStatus NativeExecution::Enter(NativeTraceFrame& frame, std::uint32_t function) noexcept {
  const auto prepared = PrepareEntry();
  if (prepared != ExecutionStatus::Returned) {
    return prepared;
  }
  frame = {.position = {.function = function}, .previous = current_frame_};
  current_frame_ = &frame;
  ++frame_count_;
  return ExecutionStatus::Returned;
}

void NativeExecution::Exit() noexcept {
  --frame_count_;
  current_frame_ = current_frame_->previous;
}

extern "C" std::uint8_t vela_native_v1_frame_storage(NativeExecution* execution,
                                                     std::uint64_t root_count,
                                                     std::uint64_t storage_bytes,
                                                     Value* stack_roots,
                                                     NativeFrame* frame,
                                                     NativeFrameAccess* access) noexcept {
  std::construct_at(frame, execution->isolate(),
                    NativeFrameSize{.root_count = static_cast<std::size_t>(root_count),
                                    .storage_bytes = static_cast<std::size_t>(storage_bytes)},
                    stack_roots);
  if (!frame->IsAllocated()) {
    std::destroy_at(frame);
    return static_cast<std::uint8_t>(ExecutionStatus::OutOfMemory);
  }
  *access = {.roots = frame->roots().data(), .storage = frame->storage()};
  return static_cast<std::uint8_t>(ExecutionStatus::Returned);
}

extern "C" void vela_native_v1_frame_release(NativeFrame* frame) noexcept {
  std::destroy_at(frame);
}

extern "C" std::uint8_t vela_native_v1_leaf_enter(NativeExecution* execution) noexcept {
  return static_cast<std::uint8_t>(execution != nullptr ? execution->PrepareEntry()
                                                        : ExecutionStatus::InvalidIsolate);
}

extern "C" std::uint8_t vela_native_v1_leaf_failure(NativeExecution* execution,
                                                    const TracePosition* position,
                                                    std::uint8_t status) {
  NativeTraceFrame frame;
  const auto entered = execution->Enter(frame, position->function);
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
