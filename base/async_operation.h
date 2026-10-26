#pragma once

#include <cstddef>
#include <cstdint>

namespace vela {

// 脚本异步操作使用固定实参窗口；泛型事实由前端绑定后传给 IR。
enum class AsyncOperation : std::uint8_t {
  Microtask,
  Delayed,
  Then,
  CatchError,
  WhenComplete,
  Timeout,
  Wait,
  Race,
  NewCompleter,
  CompleterFuture,
  CompleterIsCompleted,
  CompleterComplete,
  CompleterError,
  TimerAfter,
  TimerPeriodic,
  TimerCancel,
  NewCancellationToken,
  CancellationCancel,
  CancellationIsCancelled,
  ValidateFuture,
  Count,
};

[[nodiscard]] constexpr std::size_t AsyncArity(AsyncOperation operation) noexcept {
  switch (operation) {
    case AsyncOperation::NewCompleter:
    case AsyncOperation::NewCancellationToken:
      return 0;
    case AsyncOperation::Microtask:
    case AsyncOperation::Race:
    case AsyncOperation::CompleterFuture:
    case AsyncOperation::CompleterIsCompleted:
    case AsyncOperation::TimerCancel:
    case AsyncOperation::CancellationCancel:
    case AsyncOperation::CancellationIsCancelled:
      return 1;
    case AsyncOperation::Delayed:
    case AsyncOperation::WhenComplete:
    case AsyncOperation::CompleterComplete:
    case AsyncOperation::CompleterError:
    case AsyncOperation::TimerAfter:
    case AsyncOperation::TimerPeriodic:
    case AsyncOperation::ValidateFuture:
      return 2;
    case AsyncOperation::CatchError:
      return 3;
    case AsyncOperation::Then:
    case AsyncOperation::Timeout:
    case AsyncOperation::Wait:
      return 4;
    case AsyncOperation::Count:
      return 0;
  }
  return 0;
}

}  // namespace vela
