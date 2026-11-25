"""Standalone calculation host shared by GC-pressure AOT integration tests."""

HOST_TESTS = r'''
#include <gtest/gtest.h>
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"

extern "C" const vela::runtime::NativeImage* CALCULATION_IMAGE() noexcept;

void CollectCalculation(vela::runtime::NativeContext& context) noexcept {
  context.isolate().heap().Collect();
}

TEST(StandaloneCalculationAotTest, ExecutesUnderGcPressure) {
  vela::runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(
      vela::runtime::NativeFunction::Bind<CollectCalculation>("collect")));
  const auto instance = vela::runtime::NativeInstance::Instantiate(isolate, *CALCULATION_IMAGE());
  ASSERT_TRUE(instance) << instance.error().message;
  const auto entry = (*instance)->FindFunction("calculate");
  ASSERT_TRUE(entry);
  const auto result = vela::runtime::ExecuteNativeFunction(**instance, *entry);
  ASSERT_TRUE(result.Succeeded()) << result.message;
  EXPECT_EQ(result.value.AsInteger(), 42);
}
'''
