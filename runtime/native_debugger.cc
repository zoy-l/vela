#include "runtime/native_debugger.h"

#include <optional>
#include <span>
#include <string_view>

#include "base/declaration.h"
#include "base/native_program_mode.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"
#include "runtime/native_control.h"
#include "runtime/native_deoptimization.h"
#include "runtime/native_image.h"

namespace vela::runtime {

NativeDebugFrame::NativeDebugFrame(const NativeTraceFrame& frame) noexcept
    : image_(frame.image),
      function_(&image_->functions.values().subspan(frame.position.function).front()),
      instruction_(frame.position.instruction),
      checkpoint_(frame.checkpoint) {
  const auto locations = function_->locations.values();
  if (instruction_ < locations.size()) {
    location_ = locations.subspan(instruction_).front();
  }
}

NativeCodeTier NativeDebugFrame::tier() const noexcept {
  return checkpoint_ == nullptr ? NativeCodeTier::Baseline : checkpoint_->tier;
}

FunctionIdentity NativeDebugFrame::identity() const noexcept {
  return function_->identity;
}

std::string_view NativeDebugFrame::name() const noexcept {
  return function_->name.text();
}

std::string_view NativeDebugFrame::source_path() const noexcept {
  const auto paths = image_->source_paths.values();
  if (!location_ || location_->source_id >= paths.size()) {
    return {};
  }
  return paths.subspan(location_->source_id).front().text();
}

std::string_view NativeDebugFrame::source_text() const noexcept {
  const auto sources = image_->source_texts.values();
  if (!location_ || location_->source_id >= sources.size()) {
    return {};
  }
  return sources.subspan(location_->source_id).front().text();
}

NativeDebugContext::NativeDebugContext(NativeExecution& execution)
    : execution_(&execution), first_frame_(execution.current_frame()), top_frame_(*first_frame_) {}

std::span<const NativeDebugFrame> NativeDebugContext::frames() const {
  if (frames_.empty()) {
    for (const auto* frame = first_frame_; frame != nullptr; frame = frame->previous) {
      frames_.push_back(NativeDebugFrame(*frame));
    }
  }
  return frames_;
}

void NativeDebugContext::RequestStop() const noexcept {
  execution_->isolate().native_control().RequestStop();
}

}  // namespace vela::runtime
