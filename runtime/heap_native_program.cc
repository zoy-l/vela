#include "runtime/heap.h"

#include <cassert>
#include <cstddef>
#include <memory>

#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

NativeProgramState::~NativeProgramState() {
  delete[] global_states;
}

HeapObject* Heap::AllocateNativeProgram(std::unique_ptr<NativeProgramState> state) noexcept {
  return AllocateObject(ObjectKind::NativeProgram, sizeof(HeapObject),
                        [&state](HeapObject& object) { object.native_payload = state.release(); });
}

NativeProgramState& Heap::NativeProgram(HeapObject& object) noexcept {
  assert(object.kind == ObjectKind::NativeProgram);
  return *static_cast<NativeProgramState*>(object.native_payload);
}

void Heap::SetNativeProgramSlot(HeapObject& object, std::size_t index, Value value) noexcept {
  NativeProgram(object).values.at(index) = value;
  WriteBarrier(object, value);
}

void Heap::SetNativeEnvironment(HeapObject& callable,
                                HeapObject& state,
                                std::uint32_t function) noexcept {
  assert(callable.kind == ObjectKind::Instance && callable.layout->callable());
  callable.native_payload = &state;
  // Instance 不使用容器长度；原生函数值以此保存创建时选定版本内的函数编号。
  callable.length = function;
  WriteBarrier(callable, Value::Object(&state));
}

std::uint32_t Heap::NativeFunctionIndex(const HeapObject& callable) noexcept {
  return callable.length;
}

HeapObject* Heap::NativeEnvironment(const HeapObject& callable) noexcept {
  if (callable.kind != ObjectKind::Instance || !callable.layout->callable()) {
    return nullptr;
  }
  return static_cast<HeapObject*>(callable.native_payload);
}

void Heap::MarkNativeProgram(HeapObject& object) noexcept {
  for (const auto value : NativeProgram(object).values) {
    MarkValue(value);
  }
}

void Heap::ReleaseNativeProgram(HeapObject& object) noexcept {
  const std::unique_ptr<NativeProgramState> state(
      static_cast<NativeProgramState*>(object.native_payload));
}

}  // namespace vela::runtime
