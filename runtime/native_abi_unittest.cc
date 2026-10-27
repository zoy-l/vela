#include "runtime/native_abi.h"

#include <bit>
#include <cstdint>

#include "gtest/gtest.h"

#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/value.h"

namespace vela::runtime {

TEST(NativeAbiTest, CheckedHelpersPreserveLanguageFailures) {
  constexpr std::int64_t kExpectedSum = 12;
  NativeValue left;
  NativeValue right;
  NativeValue result;
  ASSERT_EQ(vela_native_store_int(&left, 7), static_cast<std::uint8_t>(ExecutionStatus::Returned));
  ASSERT_EQ(vela_native_store_int(&right, 5), static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(vela_native_int_binary(&result, &left, &right, 0),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(result.kind, NativeValueKind::Integer);
  EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), kExpectedSum);
  ASSERT_EQ(vela_native_store_int(&right, 0), static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(vela_native_int_binary(&result, &left, &right, 3),
            static_cast<std::uint8_t>(ExecutionStatus::DivisionByZero));
}

TEST(NativeAbiTest, NativeFramesRegisterTaggedRoots) {
  constexpr std::int64_t kRootValue = 11;
  Isolate isolate;
  NativeContext context(isolate);
  ASSERT_EQ(vela_native_frame_enter(&context, 1),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  ASSERT_EQ(context.frame_count(), 1U);
  context.current_roots().front() = Value::Integer(kRootValue);
  vela_native_frame_exit(&context);
  EXPECT_EQ(context.frame_count(), 0U);
}

}  // namespace vela::runtime
