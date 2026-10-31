#include "runtime/native_version.h"

#include <cstdint>
#include <optional>

#include "runtime/heap.h"
#include "runtime/intrusive_link_internal.h"
#include "runtime/isolate.h"
#include "runtime/native_program_state.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

NativeFunctionRoot::NativeFunctionRoot(HeapObject& state,
                                       std::uint32_t function,
                                       RootOwner owner,
                                       std::optional<std::uint32_t> instruction) noexcept
    : state_(Value::Object(&state)), function_(function), instruction_(instruction), root_(owner) {
  auto& program = Heap::NativeProgram(state);
  if (program.dispatch != nullptr) {
    retained_ = &program;
    root_.Attach(program.isolate->heap(), &state_, 1);
    RegisterDependency();
  }
}

NativeFunctionRoot::~NativeFunctionRoot() {
  UnregisterDependency();
}

void NativeFunctionRoot::RegisterDependency() noexcept {
  if (retained_ == nullptr || previous_link_ != nullptr) {
    return;
  }
  internal::PrependIntrusive(*this, retained_->function_roots, &NativeFunctionRoot::next_,
                             &NativeFunctionRoot::previous_link_);
}

void NativeFunctionRoot::UnregisterDependency() noexcept {
  internal::UnlinkIntrusive(*this, &NativeFunctionRoot::next_, &NativeFunctionRoot::previous_link_);
}

void NativeFunctionRoot::SetExecuting() noexcept {
  UnregisterDependency();
}

void NativeFunctionRoot::SetSuspended(std::uint32_t instruction) noexcept {
  instruction_ = instruction;
  RegisterDependency();
}

}  // namespace vela::runtime
