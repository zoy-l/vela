#include "runtime/native/native_debugger.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_image.h"
#include "runtime/value/value.h"

namespace vela::runtime {

NativeDebugFrame::NativeDebugFrame(const NativeTraceFrame& frame) noexcept
    : function_(&frame.image->function(frame.position.function)),
      location_(function_->locations.values()[frame.position.instruction]),
      frame_(&frame) {}

std::size_t NativeDebugFrame::register_count() const noexcept {
  return static_cast<std::size_t>(frame_->debug_count);
}

std::optional<Value> NativeDebugFrame::ReadRegister(std::size_t index) const noexcept {
  if (index >= register_count()) {
    return std::nullopt;
  }
  const auto& value = std::span(frame_->debug_values, register_count()).subspan(index).front();
  return value.available != 0 ? std::optional(DecodeNativeValue(value.value)) : std::nullopt;
}

NativeCodeTier NativeDebugFrame::tier() const noexcept {
  const auto* checkpoint = frame_->checkpoint;
  return checkpoint == nullptr ? NativeCodeTier::Baseline : checkpoint->tier;
}

std::uint32_t NativeDebugFrame::instruction() const noexcept {
  return frame_->position.instruction;
}

FunctionIdentity NativeDebugFrame::identity() const noexcept {
  return function_->identity;
}

std::string_view NativeDebugFrame::name() const noexcept {
  return function_->name.text();
}

std::string_view NativeDebugFrame::source_path() const noexcept {
  return frame_->image->source_paths.values()[location_.source_id].text();
}

std::string_view NativeDebugFrame::source_text() const noexcept {
  return frame_->image->source_texts.values()[location_.source_id].text();
}

NativeDebugContext::NativeDebugContext(NativeExecution& execution)
    : execution_(&execution), top_frame_(*execution.current_frame()) {}

std::span<const NativeDebugFrame> NativeDebugContext::frames() const {
  if (frames_.empty()) {
    for (const auto* frame = execution_->current_frame(); frame != nullptr;
         frame = frame->previous) {
      frames_.push_back(NativeDebugFrame(*frame));
    }
  }
  return frames_;
}

void NativeDebugContext::RequestStop() const noexcept {
  execution_->isolate().native_control().RequestStop();
}

}  // namespace vela::runtime
