#include "benchmarks/native_class_host.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <new>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "vela/error.h"

namespace vela::benchmarks {

NativeClassHost::NativeClassHost(std::int64_t initial) noexcept : value_(initial) {
  ++counts_.constructed;
}

NativeClassHost::~NativeClassHost() noexcept {
  ++counts_.destroyed;
}

[[clang::noinline]] void NativeClassHost::Empty() const noexcept {
  auto value = value_;
  benchmark::DoNotOptimize(value);
}

[[clang::noinline]] std::int64_t NativeClassHost::Integer(std::int64_t value) const noexcept {
  return value + value_;
}

[[clang::noinline]] double NativeClassHost::Floating(double value) const noexcept {
  constexpr double kFraction = 0.5;
  return value + static_cast<double>(value_) + kFraction;
}

[[clang::noinline]] bool NativeClassHost::Boolean(bool value) const noexcept {
  return value != (value_ != 0);
}

[[clang::noinline]] std::int64_t NativeClassHost::Nullable(
    const NativeClassHost* other) const noexcept {
  return other != nullptr ? other->value_ + value_ : 0;
}

[[clang::noinline]] void NativeClassHost::Write(std::int64_t value) noexcept {
  value_ = value;
}

[[clang::noinline]] std::int64_t NativeClassHost::Value() const noexcept {
  return value_;
}

[[clang::noinline]] void NativeClassHost::SetValue(std::int64_t value) noexcept {
  value_ = value;
}

[[clang::noinline]] std::int64_t NativeClassHost::Static(std::int64_t value) noexcept {
  return value + 1;
}

std::unique_ptr<NativeClassHost> NativeClassHost::Unique(std::int64_t initial) noexcept {
  return std::unique_ptr<NativeClassHost>(new (std::nothrow) NativeClassHost(initial));
}

std::shared_ptr<NativeClassHost> NativeClassHost::Shared(std::int64_t initial) noexcept {
  return std::make_shared<NativeClassHost>(initial);
}

std::shared_ptr<NativeClassHost> NativeClassHost::Repeated() noexcept {
  static const auto kRepeated = Shared(1);
  return kRepeated;
}

std::expected<std::int64_t, vela::Error> NativeClassHost::Failure(bool fail) const noexcept {
  if (fail) {
    return std::unexpected(vela::Error{
        .status = vela::ExecutionStatus::NativeFailure,
        .message = "native benchmark failure",
        .stack_trace = {},
    });
  }
  return value_;
}

std::int64_t NativeClassHost::Collect(runtime::NativeContext& context,
                                      std::int64_t delta) const noexcept {
  context.isolate().heap().Collect();
  return Integer(delta);
}

std::int64_t NativeClassHost::Reenter(std::int64_t value) const noexcept {
  // 基准所有者在首次执行前安装借用的执行上下文；计时路径不增加一次动态查找。
  return reentry_(reentry_state_, value);
}

void NativeClassHost::SetReentry(Reentry callback, void* state) noexcept {
  reentry_ = callback;
  reentry_state_ = state;
}

NativeClassHost::Counts NativeClassHost::AllocationCounts() noexcept {
  return counts_;
}

[[clang::noinline]] std::int64_t NativeClassIntegerPointer(const NativeClassHost* object,
                                                           std::int64_t value) noexcept {
  return object->Integer(value);
}

[[clang::noinline]] std::int64_t NativeClassIntegerAdapter(const NativeClassHost& object,
                                                           std::int64_t value) noexcept {
  return object.Integer(value);
}

}  // namespace vela::benchmarks
