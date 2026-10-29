#include "vm/suspended_frame.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/execution_limits.h"
#include "runtime/heap.h"
#include "vm/register_file.h"

namespace vela::vm::internal {
namespace {

constexpr std::size_t kSuspensionReferences = 3;

}  // namespace

SuspendedFrame::SuspendedFrame(runtime::Isolate& isolate,
                               Program program,
                               runtime::Value output,
                               std::size_t register_count,
                               std::shared_ptr<DebugSession> debug_session)
    : isolate_(&isolate),
      program_(std::move(program)),
      debug_session_(std::move(debug_session)),
      values_(register_count + kSuspensionReferences) {
  indices_.reserve(register_count);
  if (const auto* globals = program_.globals()) {
    values_.at(register_count) = globals->storage;
  }
  values_.at(register_count + 1) = output;
  isolate.scheduler().AddSuspendedFrame(SavedBytes());
}

SuspendedFrame::~SuspendedFrame() {
  isolate_->scheduler().RemoveSuspendedFrame(SavedBytes());
}

std::size_t SuspendedFrame::SavedBytes() const noexcept {
  return sizeof(SuspendedFrame) + values_.capacity() * sizeof(runtime::Value) + indices_.capacity();
}

runtime::Future& SuspendedFrame::result() {
  return *runtime::Heap::GetFuture(*values_.at(values_.size() - 2).AsObject());
}

void SuspendedFrame::Suspend(SuspensionPoint point,
                             runtime::Value source,
                             runtime::RegisterView registers) {
  const auto* input = source.IsObject() ? runtime::Heap::GetFuture(*source.AsObject()) : nullptr;
  auto future =
      input != nullptr ? input->Rooted() : runtime::Future::Value(source, &isolate_->scheduler());
  future.AttachHeap(isolate_->heap());
  point_ = point;
  indices_.clear();
  for (std::size_t index = 0; index < registers.size(); ++index) {
    const auto value = registers.subspan(index).front().Read();
    if (!value.IsNull()) {
      values_.at(indices_.size()) = value;
      indices_.push_back(static_cast<std::uint8_t>(index));
    }
  }
  values_.back() = source;
  auto self = shared_from_this();
  // 只通过托管指针引用结果；持有强 Future 句柄会与 retained 激活形成所有权环。
  const std::shared_ptr<std::vector<runtime::Value>> references(self, &values_);
  result().KeepAlive(references);
  future.ObserveResult(
      [self](const runtime::FutureResult& completion) { self->Resume(completion); }, references);
}

void SuspendedFrame::ReleaseSaved() {
  const auto values = std::span(values_);
  std::ranges::fill(values.first(values.size() - kSuspensionReferences), runtime::Value::Null());
  values.back() = runtime::Value::Null();
}

void SuspendedFrame::Resume(const runtime::FutureResult& completion) {
  std::array<RegisterFile, kMaxCallFrames> registers;
  Execution execution(*isolate_, program_, registers, debug_session_);
  if (const auto outcome = execution.ResumeFrame(*this, completion)) {
    const runtime::RootRange root(isolate_->heap(), &outcome->value, 1,
                                  runtime::RootOwner::Runtime);
    Execution::CompleteFuture(*isolate_, result(), *outcome);
  }
}

}  // namespace vela::vm::internal
