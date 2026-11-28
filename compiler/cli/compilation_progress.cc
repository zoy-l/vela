#include "compiler/cli/compilation_progress.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "base/path.h"
#include "compiler/cli/standard_streams.h"
#include "compiler/frontend/modules/source_progress.h"

namespace vela::tooling {
namespace {

constexpr auto kAnimationInterval = std::chrono::milliseconds(100);
constexpr std::string_view kAnimationFrames = "|/-\\";

}  // namespace

CompilationProgressDisplay::CompilationProgressDisplay(std::ostream& output)
    : CompilationProgressDisplay(output,
                                 &output == &std::cerr && IsInteractiveStandardError()
                                     ? CompilationProgressOutput::Interactive
                                     : CompilationProgressOutput::Plain) {}

CompilationProgressDisplay::CompilationProgressDisplay(std::ostream& output,
                                                       CompilationProgressOutput mode)
    : output_(output), mode_(mode) {
  std::error_code error;
  directory_ = std::filesystem::current_path(error);
  Write(mode_ == CompilationProgressOutput::Interactive ? "loading |" : "loading");
  if (mode_ == CompilationProgressOutput::Interactive) {
    animation_ = std::jthread([this] { Animate(); });
  }
}

CompilationProgressDisplay::~CompilationProgressDisplay() {
  Finish();
}

void CompilationProgressDisplay::Write(std::string_view text) {
  if (mode_ == CompilationProgressOutput::Plain) {
    output_ << text << '\n';
  } else {
    const auto width = text.size();
    output_ << '\r' << text;
    if (width_ > width) {
      output_ << std::string(width_ - width, ' ') << '\r' << text;
    }
    width_ = width;
  }
  output_.flush();
}

void CompilationProgressDisplay::Clear() {
  if (mode_ == CompilationProgressOutput::Interactive) {
    output_ << '\r' << std::string(width_, ' ') << '\r';
    output_.flush();
    width_ = 0;
  }
}

void CompilationProgressDisplay::Report(const frontend::SourceProgress& progress) {
  const std::scoped_lock lock(mutex_);
  if (!active_) {
    return;
  }
  if (progress.kind == frontend::SourceProgressKind::Loading) {
    if (!loading_) {
      loading_ = true;
      frame_ = 0;
      Write(mode_ == CompilationProgressOutput::Interactive ? "loading |" : "loading");
      changed_.notify_one();
    }
    return;
  }
  loading_ = false;
  auto path = PathText(Utf8Path(progress.path).lexically_proximate(directory_));
  for (auto& character : path) {
    if (static_cast<unsigned char>(character) < ' ' || character == '\x7f') {
      character = '?';
    }
  }
  const auto completed = progress.completed;
  const auto total = progress.total;
  const auto percent = completed * 100 / total;
  Write("[" + std::to_string(completed) + "/" + std::to_string(total) + " · " +
        std::to_string(percent) + "%] " + path);
  changed_.notify_one();
}

void CompilationProgressDisplay::Animate() {
  std::unique_lock lock(mutex_);
  while (active_) {
    if (!loading_) {
      changed_.wait(lock, [this] { return !active_ || loading_; });
      continue;
    }
    if (changed_.wait_for(lock, kAnimationInterval, [this] { return !active_ || !loading_; })) {
      continue;
    }
    frame_ = (frame_ + 1) % kAnimationFrames.size();
    Write(std::string("loading ") + kAnimationFrames[frame_]);
  }
}

void CompilationProgressDisplay::Finish() {
  {
    const std::scoped_lock lock(mutex_);
    if (!active_) {
      return;
    }
    active_ = false;
    Clear();
  }
  changed_.notify_one();
  if (animation_.joinable()) {
    animation_.join();
  }
}

}  // namespace vela::tooling
