#include "base/declaration.h"
#include "runtime/heap.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "runtime/isolate.h"
#include "runtime/native_metadata.h"
#include "runtime/native_program_state.h"
#include "runtime/native_version.h"
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
  if (callable.kind != ObjectKind::Instance || (callable.layout->callable() == nullptr)) {
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
  auto* state = static_cast<NativeProgramState*>(object.native_payload);
  state->retired_next = retired_programs_;
  retired_programs_ = state;
}

void Heap::ReleaseRetiredNativePrograms() noexcept {
  while (retired_programs_ != nullptr) {
    const std::unique_ptr<NativeProgramState> state(retired_programs_);
    retired_programs_ = state->retired_next;
  }
}

void Heap::AccumulateNativeLayouts(LayoutCounts& counts) const noexcept {
  for (auto* object = objects_; object != nullptr; object = object->next) {
    if (object->kind == ObjectKind::NativeProgram) {
      const auto& state = NativeProgram(*object);
      counts.classes += state.owned_classes.size();
      counts.nominal_types += state.owned_nominals.size();
      counts.code_identities += state.owned_code_identities.size();
    }
  }
}

void Heap::TraceNativeMetadata(NativeMetadataTracer& tracer) const {
  for (auto* object = objects_; object != nullptr; object = object->next) {
    tracer.Mark(object->layout);
    if (object->kind == ObjectKind::NativeProgram) {
      tracer.Mark(NativeProgram(*object).layouts);
    } else {
      tracer.Mark(TypeOf(Value::Object(object)));
    }
  }
}

void Heap::AppendRetainedNativeFunctions(std::vector<NativeRetainedFunction>& functions) const {
  for (auto* object = objects_; object != nullptr; object = object->next) {
    if (object->kind == ObjectKind::NativeProgram) {
      const auto& state = NativeProgram(*object);
      for (auto* root = state.function_roots; root != nullptr; root = root->next_) {
        functions.push_back({
            .image = state.image,
            .function = root->function_,
            .instruction = root->instruction_,
        });
      }
    } else if (auto* environment = NativeEnvironment(*object)) {
      const auto& state = NativeProgram(*environment);
      if (state.dispatch != nullptr) {
        functions.push_back({.image = state.image, .function = NativeFunctionIndex(*object)});
      }
    }
  }
}

void Heap::AppendRetainedNativeTypes(std::vector<DeclarationId>& declarations) const {
  absl::flat_hash_set<TypeConstraint> visited;
  std::vector<TypeConstraint> pending;
  for (auto* object = objects_; object != nullptr; object = object->next) {
    pending.push_back(TypeOf(Value::Object(object)));
  }
  while (!pending.empty()) {
    const auto type = pending.back();
    pending.pop_back();
    if (!visited.insert(type).second) {
      continue;
    }
    if (type.nominal != nullptr && type.nominal->declaration() != DeclarationId{}) {
      declarations.push_back(type.nominal->declaration());
    }
    if (type.array != nullptr) {
      pending.push_back(type.array->element);
    }
    if (type.collection != nullptr) {
      pending.push_back(type.collection->keys.element);
      const auto& values = type.collection->values;
      if (values) {
        pending.push_back(values->element);
      }
    }
    if (const auto* signature = type.signature) {
      if (signature->result) {
        pending.push_back(*signature->result);
      }
      for (const auto& parameter : signature->parameters) {
        pending.push_back(parameter.type);
      }
    }
  }
}

}  // namespace vela::runtime
