#include "vm/debugger.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "runtime/value.h"
#include "vm/debug_info.h"
#include "vm/execution.h"
#include "vm/program.h"

namespace vela::vm {

std::string_view DebugFrame::name() const noexcept {
  return frame_->function->name;
}

std::optional<DebugLocation> DebugFrame::location() const noexcept {
  const auto& function = *frame_->function;
  const auto source_paths = function.source_paths != nullptr
                                ? std::span<const std::string>(*function.source_paths)
                                : std::span<const std::string>();
  const DebugInfo debug{
      .name = function.name,
      .source_paths = source_paths,
      .mappings = function.source_mappings,
  };
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
  const auto& frame = std::span(frames_, frame_count_).subspan(frame_count_ - 1 - depth).front();
  const auto instruction =
      depth != 0 && frame.program_counter != 0 ? frame.program_counter - 1 : frame.program_counter;
  return DebugFrame(frame, instruction);
}

DebugFrame DebugContext::current_frame() const noexcept {
  const auto& frame = std::span(frames_, frame_count_).back();
  return {frame, frame.program_counter};
}

}  // namespace vela::vm
