#include "compiler/test_support/execution_expectations.h"

#include <gtest/gtest.h>

#include "compiler/test_support/program_test_support.h"
#include "runtime/native/native_instance.h"

namespace vela::compiler::test {

void ExpectAnswer(runtime::NativeInstance& instance) {
  const auto result = ExecuteFutureInteger(instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 42);
}

}  // namespace vela::compiler::test
