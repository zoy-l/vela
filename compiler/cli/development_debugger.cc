#include "compiler/cli/development_debugger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "base/native_program_mode.h"
#include "base/path.h"
#include "compiler/cli/console_input.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "runtime/native_control.h"
#include "runtime/native_debugger.h"

namespace vela::tooling {
namespace {

constexpr std::string_view kSourceCommand = ".source";
constexpr std::string_view kBreakCommand = ".break";

}  // namespace

DevelopmentDebugger::DevelopmentDebugger(compiler::DevelopmentSession& session,
                                         std::ostream& errors,
                                         std::function<void()> poll)
    : session_(&session), errors_(&errors), poll_(std::move(poll)) {
  session_->SetDebugger(this);
}

DevelopmentDebugger::~DevelopmentDebugger() {
  session_->SetDebugger(nullptr);
}

DebugCommandResult DevelopmentDebugger::Command(std::string_view command) {
  if (command == ".pause") {
    pause_requested_ = true;
    *errors_ << "pause requested\n";
  } else if (command == ".continue" || command == ".step") {
    if (paused_ == nullptr) {
      *errors_ << "no paused script frame\n";
    } else {
      continuing_ = true;
      if (command == ".step") {
        const auto& frame = paused_->top_frame();
        step_from_ = Position{
            .identity = frame.identity(),
            .instruction = frame.instruction(),
            .depth = paused_->frames().size(),
        };
      }
    }
  } else if (command == ".stack") {
    Stack();
  } else if (command == ".source" || command.starts_with(".source ")) {
    Source(command.size() > kSourceCommand.size() ? command.substr(kSourceCommand.size() + 1)
                                                  : std::string_view());
  } else if (command == ".break" || command.starts_with(".break ")) {
    Break(command.size() > kBreakCommand.size() ? command.substr(kBreakCommand.size() + 1)
                                                : std::string_view());
  } else {
    return DebugCommandResult::Unknown;
  }
  return DebugCommandResult::Handled;
}

void DevelopmentDebugger::Break(std::string_view arguments) {
  if (arguments == "clear") {
    breakpoints_.clear();
    last_break_.reset();
    *errors_ << "breakpoints cleared\n";
    return;
  }
  if (arguments.empty()) {
    for (const auto& breakpoint : breakpoints_) {
      *errors_ << "breakpoint " << breakpoint.path << ':' << breakpoint.line << '\n';
    }
    return;
  }
  const auto separator = arguments.find_last_of(' ');
  const auto line = ParseConsoleIndex(
      separator == std::string_view::npos ? arguments : arguments.substr(separator + 1));
  if (!line || *line == 0) {
    *errors_ << "usage: .break [MODULE] LINE | .break clear\n";
    return;
  }
  const auto snapshot = session_->snapshot();
  const auto path = separator == std::string_view::npos
                        ? snapshot->roots.front()
                        : PathText(Utf8Path(arguments.substr(0, separator)).lexically_normal());
  if (!snapshot->sources.contains(path)) {
    *errors_ << "breakpoint module is not loaded: " << path << '\n';
    return;
  }
  if (!std::ranges::any_of(breakpoints_, [&](const auto& breakpoint) {
        return breakpoint.path == path && breakpoint.line == *line;
      })) {
    breakpoints_.push_back({.path = path, .line = *line});
  }
  *errors_ << "breakpoint " << path << ':' << *line << " (current and future revisions)\n";
}

void DevelopmentDebugger::PrintFrame(std::size_t index,
                                     const runtime::NativeDebugFrame& frame) const {
  *errors_ << '#' << index << ' ' << frame.name() << " revision " << frame.identity().revision
           << " at " << frame.source_path();
  if (const auto location = frame.location()) {
    *errors_ << ':' << location->line << ':' << location->column;
  }
  *errors_ << (frame.tier() == NativeCodeTier::Optimized ? " [optimized]\n" : " [baseline]\n");
}

void DevelopmentDebugger::Stack() const {
  if (paused_ == nullptr) {
    *errors_ << "no paused script frame\n";
    return;
  }
  std::size_t index = 0;
  for (const auto& frame : paused_->frames()) {
    PrintFrame(index++, frame);
  }
}

void DevelopmentDebugger::Source(std::string_view argument) const {
  if (paused_ == nullptr) {
    *errors_ << "no paused script frame\n";
    return;
  }
  const auto index =
      argument.empty() ? std::optional<std::uint32_t>(0) : ParseConsoleIndex(argument);
  const auto frames = paused_->frames();
  if (!index || *index >= frames.size()) {
    *errors_ << "usage: .source [FRAME_INDEX]\n";
    return;
  }
  const auto& frame = frames.subspan(*index).front();
  PrintFrame(*index, frame);
  auto source = frame.source_text();
  std::uint32_t line = 1;
  while (!source.empty()) {
    const auto end = source.find('\n');
    *errors_ << line++ << " | " << source.substr(0, end) << '\n';
    if (end == std::string_view::npos) {
      break;
    }
    source.remove_prefix(end + 1);
  }
}

bool DevelopmentDebugger::IsBreakpoint(const runtime::NativeDebugFrame& frame) const {
  const auto location = frame.location();
  if (!location || frame.identity().revision != session_->snapshot()->revision) {
    return false;
  }
  const auto path = frame.source_path();
  return std::ranges::any_of(breakpoints_, [&](const auto& breakpoint) {
    return breakpoint.path == path && breakpoint.line == location->line;
  });
}

void DevelopmentDebugger::AtSafepoint(const runtime::NativeDebugContext& context) noexcept {
  if (!pause_requested_ && !step_from_ && breakpoints_.empty()) {
    return;
  }
  const auto& frame = context.top_frame();
  const Position position{
      .identity = frame.identity(),
      .instruction = frame.instruction(),
      .depth = step_from_ ? context.frames().size() : 0,
  };
  const auto breakpoint = IsBreakpoint(frame);
  const auto source_location = frame.location();
  const BreakLocation location{
      .identity = frame.identity(),
      .line = source_location ? source_location->line : 0,
      .generation = session_->generation(),
  };
  const auto hit = breakpoint && last_break_ != location;
  if (!breakpoint) {
    last_break_.reset();
  }
  if (!pause_requested_ && (!step_from_ || *step_from_ == position) && !hit) {
    return;
  }
  pause_requested_ = false;
  step_from_.reset();
  last_break_ = breakpoint ? std::optional(location) : std::nullopt;
  continuing_ = false;
  paused_ = &context;
  *errors_ << "paused ";
  PrintFrame(0, frame);
  while (!continuing_ && !session_->isolate().native_control().IsStopRequested()) {
    poll_();
  }
  paused_ = nullptr;
}

}  // namespace vela::tooling
