#include "runtime/native_control.h"

#include <cstdint>

#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"

namespace vela::runtime {

void NativeControl::RequestStop() noexcept {
  stop_requested_.store(true, std::memory_order_release);
}

void NativeControl::EnterHost() noexcept {
  ++host_borrows_;
}

void NativeControl::ExitHost() noexcept {
  --host_borrows_;
}

bool NativeControl::HasHostBorrow() const noexcept {
  return host_borrows_ != 0;
}

void NativeControl::EnterInitialization() noexcept {
  ++initializations_;
}

void NativeControl::ExitInitialization() noexcept {
  --initializations_;
}

bool NativeControl::CanPublish() const noexcept {
  return !IsStopRequested() && !HasHostBorrow() && initializations_ == 0;
}

bool NativeControl::IsStopRequested() const noexcept {
  return stop_requested_.load(std::memory_order_acquire);
}

NativeInitializationScope::NativeInitializationScope(NativeControl& control) noexcept
    : control_(&control) {
  control_->EnterInitialization();
}

NativeInitializationScope::~NativeInitializationScope() {
  control_->ExitInitialization();
}

ExecutionStatus NativeControl::Poll() const noexcept {
  return IsStopRequested() && !HasHostBorrow() ? ExecutionStatus::ExecutionStopped
                                               : ExecutionStatus::Returned;
}

extern "C" std::uint8_t vela_native_v3_safepoint(NativeExecution* execution) noexcept {
  return static_cast<std::uint8_t>(execution->isolate().native_control().Poll());
}

extern "C" void vela_native_v3_host_enter(NativeExecution* execution) noexcept {
  execution->isolate().native_control().EnterHost();
}

extern "C" void vela_native_v3_host_exit(NativeExecution* execution) noexcept {
  execution->isolate().native_control().ExitHost();
}

}  // namespace vela::runtime
