#include "runtime/native/native_stack.h"

#include <windows.h>  // IWYU pragma: keep

#include <minwindef.h>
#include <winbase.h>

#include <cstddef>
#include <memory>

#include <gtest/gtest.h>

#include "runtime/native/native_control.h"

namespace vela::runtime {
namespace {

struct FiberTestState final {
  const NativeControlState* thread_control;
  void* original_fiber;
};

void WINAPI CheckFiberStack(void* parameter) {
  const auto& state = *static_cast<FiberTestState*>(parameter);
  {
    NativeControl fiber_control;
    const auto limit = CurrentNativeStackLimit();
    EXPECT_TRUE(limit);
    EXPECT_EQ(CachedNativeStackLimit(*state.thread_control),
              NativeControlState::kUnknownStackLimit);
    EXPECT_EQ(CurrentNativeStackLimit(*state.thread_control), limit);
    EXPECT_EQ(CachedNativeStackLimit(fiber_control.state()),
              NativeControlState::kUnknownStackLimit);
    EXPECT_EQ(CurrentNativeStackLimit(fiber_control.state()), limit);
  }
  SwitchToFiber(state.original_fiber);
}

TEST(NativeStackTest, NativeStackCacheTracksThreadAndFiberSwitches) {
  NativeControl control;
  const auto thread_limit = CurrentNativeStackLimit();
  ASSERT_TRUE(thread_limit);
  ASSERT_EQ(CachedNativeStackLimit(control.state()), *thread_limit);

  const auto convert_back = [](void*) { EXPECT_NE(ConvertFiberToThread(), 0); };
  const std::unique_ptr<void, decltype(convert_back)> original(ConvertThreadToFiber(nullptr),
                                                               convert_back);
  ASSERT_NE(original, nullptr);
  FiberTestState state{.thread_control = &control.state(), .original_fiber = original.get()};
  constexpr auto kFiberStackBytes = std::size_t{256} * 1024;
  const std::unique_ptr<void, decltype(&DeleteFiber)> fiber(
      CreateFiberEx(0, kFiberStackBytes, 0, CheckFiberStack, &state), &DeleteFiber);
  ASSERT_NE(fiber, nullptr);
  SwitchToFiber(fiber.get());
  EXPECT_EQ(CachedNativeStackLimit(control.state()), *thread_limit);
}

}  // namespace
}  // namespace vela::runtime
