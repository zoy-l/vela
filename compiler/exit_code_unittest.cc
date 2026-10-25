#include <cstdint>
#include <limits>

#include "gtest/gtest.h"

#include "compiler/exit_code.h"
#include "runtime/value.h"

namespace vela::compiler {
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
}  // namespace vela::compiler
