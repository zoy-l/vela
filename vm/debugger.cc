#include "vm/debugger.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "runtime/value.h"
#include "vm/debug_info.h"
#include "vm/execution.h"
#include "vm/program.h"

namespace vela::vm {

auto DebugFrame::name() const noexcept -> std::string_view {
  return frame_->function->name;
}

auto DebugFrame::location() const noexcept -> std::optional<DebugLocation> {
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

auto DebugFrame::register_count() const noexcept -> std::size_t {
  return frame_->function->register_count;
}

auto DebugFrame::ReadRegister(std::size_t index) const noexcept -> std::optional<runtime::Value> {
  const auto registers = frame_->frame.registers();
  if (index >= registers.size()) {
    return std::nullopt;
  }
  return registers.subspan(index).front().Read();
}

auto DebugContext::Frame(std::size_t depth) const noexcept -> std::optional<DebugFrame> {
  if (depth >= frame_count_) {
    return std::nullopt;
  }
  const auto& frame = std::span(frames_, frame_count_).subspan(frame_count_ - 1 - depth).front();
  const auto instruction =
      depth != 0 && frame.program_counter != 0 ? frame.program_counter - 1 : frame.program_counter;
  return DebugFrame(frame, instruction);
}

auto DebugContext::current_frame() const noexcept -> DebugFrame {
  const auto& frame = std::span(frames_, frame_count_).back();
  return {frame, frame.program_counter};
}

}  // namespace vela::vm
