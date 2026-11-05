#include "base/declaration.h"
#include "runtime/heap/heap.h"

#include <absl/functional/function_ref.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "runtime/isolate/isolate.h"
#include "runtime/native/native_metadata.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_version.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

NativeProgramState::~NativeProgramState() {
  delete[] global_states;
  delete[] global_caches;
}

NativeProgramState::GlobalState& NativeProgramState::global_state(
    std::size_t index) const noexcept {
  return std::span(global_states, image->global_initializers.size)[index];
}

NativeGlobalCache& NativeProgramState::global_cache(std::size_t index) const noexcept {
  return std::span(global_caches, image->global_initializers.size)[index];
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
  NativeProgram(object).values[index] = value;
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

void Heap::VisitRootTypes(absl::FunctionRef<void(TypeConstraint)> visit) const {
  for (auto* root = roots_; root != nullptr; root = root->next_) {
    if (root->kind_ == RootRange::Kind::Types) {
      for (const auto type :
           std::span(static_cast<const TypeConstraint*>(root->data_), root->count_)) {
        visit(type);
      }
    }
  }
}

void Heap::TraceNativeMetadata(NativeMetadataTracer& tracer) const {
  VisitRootTypes([&tracer](TypeConstraint type) { tracer.Mark(type); });
  for (auto* object = objects_; object != nullptr; object = object->next) {
    switch (object->kind) {
      case ObjectKind::Instance:
      case ObjectKind::Host:
        tracer.Mark(object->layout);
        break;
      case ObjectKind::Array:
        tracer.Mark(object->array_layout());
        break;
      case ObjectKind::Collection:
        tracer.Mark(object->collection_layout());
        break;
      case ObjectKind::Future:
      case ObjectKind::Completer:
        tracer.Mark(object->future_signature());
        break;
      case ObjectKind::NativeProgram: {
        const auto& state = NativeProgram(*object);
        tracer.Mark(state.layouts);
        for (const auto& [description, type] : state.native_future_types) {
          tracer.Mark(type);
        }
        break;
      }
      default:
        break;
    }
  }
}

namespace {

template <class Append>
void VisitNativeType(TypeConstraint type,
                     std::vector<DeclarationId>& declarations,
                     const Append& append) {
  if (type.nominal != nullptr && type.nominal->declaration() != DeclarationId{}) {
    declarations.push_back(type.nominal->declaration());
  }
  if (type.interface != nullptr && type.interface->declaration != DeclarationId{}) {
    declarations.push_back(type.interface->declaration);
  }
  if (type.array != nullptr) {
    append(type.array->element);
  }
  if (type.collection != nullptr) {
    append(type.collection->keys.element);
    const auto& values = type.collection->values;
    if (values) {
      append(values->element);
    }
  }
  if (const auto* signature = type.signature) {
    if (signature->result) {
      append(*signature->result);
    }
    for (const auto& parameter : signature->parameters) {
      append(parameter.type);
    }
  }
}

}  // namespace

void Heap::AppendRetainedNative(std::vector<NativeRetainedFunction>& functions,
                                std::vector<DeclarationId>& declarations) const {
  absl::flat_hash_set<TypeConstraint> visited;
  std::vector<TypeConstraint> pending;
  const auto append = [&](TypeConstraint type) {
    if (visited.insert(type).second) {
      pending.push_back(type);
    }
  };
  VisitRootTypes(append);
  for (auto* object = objects_; object != nullptr; object = object->next) {
    append(TypeOf(Value::Object(object)));
    if (object->kind == ObjectKind::NativeProgram) {
      const auto& state = NativeProgram(*object);
      for (const auto& [description, type] : state.native_future_types) {
        append(type);
      }
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
  while (!pending.empty()) {
    const auto type = pending.back();
    pending.pop_back();
    VisitNativeType(type, declarations, append);
  }
}

}  // namespace vela::runtime
