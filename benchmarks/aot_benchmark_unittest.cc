#include "benchmarks/aot_benchmark_kernels.h"

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "base/native_program_mode.h"
#include "benchmarks/aot_benchmark_types.h"
#include "benchmarks/aot_native/entries.h"
#include "benchmarks/jit_benchmark.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

std::int64_t WorkloadSize() noexcept {
  return kAotWorkloadSize;
}

std::int64_t HostStep(std::int64_t value) noexcept {
  return value + 1;
}

void VerifyNumericResults(runtime::NativeInstance& instance) {
  const auto prepare = instance.FindFunction("prepareNumeric");
  const auto arithmetic = instance.FindFunction("arithmetic");
  const auto calls = instance.FindFunction("calls");
  const auto scalar = instance.FindFunction("entryScalar");
  ASSERT_TRUE(prepare);
  ASSERT_TRUE(arithmetic);
  ASSERT_TRUE(calls);
  ASSERT_TRUE(scalar);
  runtime::NativeExecution execution(instance);
  for (const auto seed : {kAotInputSeed, std::int64_t{7}}) {
    for (const std::int64_t size : {0, 1, 17, 1000, 4096, 65536}) {
      SCOPED_TRACE(testing::Message() << "seed=" << seed << ", size=" << size);
      const auto values = PrepareNumericValues({.size = size, .seed = seed});
      const std::array arguments{
          runtime::EncodeNativeValue(runtime::Value::Integer(size)),
          runtime::EncodeNativeValue(runtime::Value::Integer(seed)),
      };
      ASSERT_TRUE(runtime::InvokeNativeTarget(execution, *prepare, arguments));
      instance.isolate().heap().Collect();
      const auto squared = runtime::InvokeNativeTarget(execution, *arithmetic, {});
      ASSERT_TRUE(squared);
      ASSERT_TRUE(squared->IsInteger());
      EXPECT_EQ(squared->AsInteger(), CppArithmetic(values));
      const auto stepped = runtime::InvokeNativeTarget(execution, *calls, {});
      ASSERT_TRUE(stepped);
      ASSERT_TRUE(stepped->IsInteger());
      EXPECT_EQ(stepped->AsInteger(), CppCalls(values));
    }
  }
  for (const std::int64_t input : {0, 41, 123}) {
    const std::array arguments{runtime::EncodeNativeValue(runtime::Value::Integer(input))};
    const auto result = runtime::InvokeNativeTarget(execution, *scalar, arguments);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->IsInteger());
    EXPECT_EQ(result->AsInteger(), CppScalarEntry(input));
  }
}

TEST(AotBenchmarkTest, NumericManifestChecksumsMatchPreparedRuntimeData) {
  const auto first = PrepareNumericValues({.size = 8, .seed = kAotInputSeed});
  const std::vector<std::int64_t> expected{385, 236, 859, 254, 69, 224, 703, 274};
  EXPECT_EQ(first, expected);
  for (const auto& problem : kAotBenchmarkCases) {
    if (problem.kernel == AotKernel::Arithmetic || problem.kernel == AotKernel::Calls) {
      SCOPED_TRACE(problem.size);
      const auto values = PrepareNumericValues({.size = problem.size, .seed = kAotInputSeed});
      const auto result =
          problem.kernel == AotKernel::Arithmetic ? CppArithmetic(values) : CppCalls(values);
      EXPECT_EQ(result, problem.expected);
    }
  }
}

TEST(AotBenchmarkTest, OfflineEntriesReadRepreparedDataAcrossCollections) {
  runtime::Isolate isolate;
  ASSERT_TRUE(
      isolate.RegisterNative(runtime::NativeFunction::Bind<WorkloadSize>("benchmark.size")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<HostStep>("benchmark.step")));
  const auto instance = runtime::NativeInstance::Instantiate(isolate, *kAotBenchmarkImage);
  ASSERT_TRUE(instance);
  VerifyNumericResults(**instance);
}

class JitNumericBenchmarkTest : public testing::Test,
                                public testing::WithParamInterface<NativeCodeTier> {};

TEST_P(JitNumericBenchmarkTest, ReloadableEntriesReadRepreparedDataAcrossCollections) {
  runtime::Isolate registrations;
  ASSERT_TRUE(
      registrations.RegisterNative(runtime::NativeFunction::Bind<WorkloadSize>("benchmark.size")));
  ASSERT_TRUE(
      registrations.RegisterNative(runtime::NativeFunction::Bind<HostStep>("benchmark.step")));
  JitWorkload workload(VELA_AOT_BENCHMARK_SOURCE, GetParam());
  auto setup = LoadJitWorkload(workload, registrations.FreezeBindings());
  ASSERT_TRUE(setup) << setup.error();
  auto instance =
      runtime::NativeInstance::Borrow(setup->isolate->native_dispatch().current_state());
  VerifyNumericResults(instance);
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitNumericBenchmarkTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::benchmarks
