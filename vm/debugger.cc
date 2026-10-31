#include "vm/debugger.h"

#include <cstddef>
#include <optional>
#include <string_view>

#include "runtime/value.h"
#include "vm/debug_info.h"
#include "vm/execution.h"
#include "vm/frame_slots.h"
#include "vm/program.h"

namespace vela::vm {

std::string_view DebugFrame::name() const noexcept {
  return frame_->function->name;
}

std::optional<DebugLocation> DebugFrame::location() const noexcept {
  const auto debug = frame_->function->View().debug;
  const auto* mapping = debug.Locate(instruction_);
  if (mapping == nullptr) {
    return std::nullopt;
  }
  const auto position = mapping->location;
  return DebugLocation{
      .path = debug.source_paths.subspan(position.source_id).front(),
      .position = position,
  };
}

std::size_t DebugFrame::register_count() const noexcept {
  return frame_->frame.registers().size();
}

std::optional<runtime::Value> DebugFrame::ReadRegister(std::size_t index) const noexcept {
  const auto registers = frame_->frame.registers();
  if (index >= registers.size()) {
    return std::nullopt;
  }
  return registers.subspan(index).front().Read();
}

std::optional<DebugFrame> DebugContext::Frame(std::size_t depth) const noexcept {
  if (depth >= frame_count_) {
    return std::nullopt;
  }
  const auto& frame = frames_->at(frame_count_ - 1 - depth);
  const auto instruction =
      depth != 0 && frame.program_counter != 0 ? frame.program_counter - 1 : frame.program_counter;
  return DebugFrame(frame, instruction);
}

DebugFrame DebugContext::current_frame() const noexcept {
  const auto& frame = frames_->at(frame_count_ - 1);
  return {frame, frame.program_counter};
}

}  // namespace vela::vm
