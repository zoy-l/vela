#include "compiler/native/function_emitter.h"

#include <cstddef>
#include <vector>

#include "compiler/ir/ir.h"
#include "runtime/native/native_abi.h"

namespace vela::compiler::native {

void FunctionEmitter::InitializeDebugValues() {
  if (debug_values_ == nullptr) {
    return;
  }
  builder_.CreateStore(
      debug_values_, ByteAddress(trace_frame_, offsetof(runtime::NativeTraceFrame, debug_values)));
  builder_.CreateStore(builder_.getInt64(source_.register_count),
                       ByteAddress(trace_frame_, offsetof(runtime::NativeTraceFrame, debug_count)));
}

void FunctionEmitter::StoreDebugValues() {
  if (debug_values_ == nullptr) {
    return;
  }
  debug_live_.assign(source_.register_count, 0);
  if (source_.native_symbol.empty()) {
    for (const auto reg : roots_.checkpoints.at(instruction_index_)) {
      debug_live_[reg] = 1;
    }
  } else {
    for (std::size_t index = 0; index < source_.parameter_count; ++index) {
      debug_live_[index] = 1;
    }
  }
  for (ir::RegisterId reg = 0; reg < source_.register_count; ++reg) {
    auto* slot = ByteAddress(debug_values_, reg * sizeof(runtime::NativeDebugValue));
    const bool available = debug_live_[reg] != 0;
    builder_.CreateStore(builder_.getInt8(available ? 1 : 0),
                         ByteAddress(slot, offsetof(runtime::NativeDebugValue, available)));
    if (available) {
      builder_.CreateStore(Encode(reg),
                           ByteAddress(slot, offsetof(runtime::NativeDebugValue, value)));
    }
  }
}

}  // namespace vela::compiler::native
