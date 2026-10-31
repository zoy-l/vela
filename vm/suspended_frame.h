#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "runtime/async/future.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/register_storage.h"
#include "runtime/value/value.h"
#include "vm/execution.h"
#include "vm/program.h"

namespace vela::vm::internal {

// 每次 async 调用只保留一个结果和激活；固定存储保证已排队 RootRange 的地址稳定。
class SuspendedFrame final : public std::enable_shared_from_this<SuspendedFrame> {
 public:
  SuspendedFrame(runtime::Isolate& isolate,
                 Program program,
                 runtime::Value output,
                 std::size_t register_count,
                 std::shared_ptr<DebugSession> debug_session);
  ~SuspendedFrame();
  SuspendedFrame(const SuspendedFrame&) = delete;
  SuspendedFrame& operator=(const SuspendedFrame&) = delete;
  SuspendedFrame(SuspendedFrame&&) = delete;
  SuspendedFrame& operator=(SuspendedFrame&&) = delete;

  void Suspend(SuspensionPoint point, runtime::Value source, runtime::RegisterView registers);
  void ReleaseSaved();
  [[nodiscard]] SuspensionPoint point() const { return point_; }
  [[nodiscard]] std::span<const std::uint8_t> indices() const { return indices_; }
  [[nodiscard]] std::span<const runtime::Value> saved() const {
    return std::span(values_).first(indices_.size());
  }

 private:
  void Resume(const runtime::FutureResult& completion);
  [[nodiscard]] std::size_t SavedBytes() const noexcept;
  [[nodiscard]] runtime::Future& result();

  runtime::Isolate* isolate_;
  Program program_;
  SuspensionPoint point_;
  std::vector<std::uint8_t> indices_;
  std::shared_ptr<DebugSession> debug_session_;
  std::vector<runtime::Value> values_;
};

}  // namespace vela::vm::internal
