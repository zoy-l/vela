#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "base/process_exit.h"
#include "runtime/process/process_exit.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

TEST(ExitCodeTest, UsesExplicitIntResultAndVoidWithoutConversion) {
  EXPECT_EQ(ProcessExitCode(runtime::Value::Null()), 0);
  EXPECT_EQ(ProcessExitCode(runtime::Value::Integer(0)), 0);
  EXPECT_EQ(ProcessExitCode(runtime::Value::Integer(42)), 42);
  EXPECT_EQ(ProcessExitCode(runtime::Value::Integer(kMaximumExitCode)), kMaximumExitCode);
  EXPECT_FALSE(ProcessExitCode(runtime::Value::Integer(-1)));
  EXPECT_FALSE(ProcessExitCode(runtime::Value::Integer(kMaximumExitCode + 1)));
  EXPECT_FALSE(ProcessExitCode(runtime::Value::Integer(std::numeric_limits<std::int64_t>::max())));
  EXPECT_FALSE(ProcessExitCode(runtime::Value::Double(0.0)));
  EXPECT_FALSE(ProcessExitCode(runtime::Value::Boolean(false)));
}

}  // namespace
}  // namespace vela::runtime
