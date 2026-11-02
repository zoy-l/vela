#include "benchmarks/jit_benchmark.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "base/file.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "base/testing/directory.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_function.h"

namespace vela::benchmarks {
namespace {

constexpr std::string_view kSource = R"(
external func start(): int = 'benchmark.start';
let values: int[] = [start()];
export func advance(): int {
  const value = values[0];
  values[0] = value + 1;
  return value;
}
)";

constexpr std::int64_t kFirstBindingValue = 40;
constexpr std::int64_t kSecondBindingValue = 100;

std::int64_t StartForty() noexcept {
  return kFirstBindingValue;
}
std::int64_t StartHundred() noexcept {
  return kSecondBindingValue;
}

class JitWorkloadTest : public test::DirectoryTest,
                        public testing::WithParamInterface<NativeCodeTier> {};

TEST_P(JitWorkloadTest, ReusesCodeWithIndependentBindingsGlobalsAndHeaps) {
  const auto path = directory() / "workload.vela";
  ASSERT_TRUE(WriteFileAtomically(path, std::as_bytes(std::span(kSource))));
  JitWorkload workload(PathText(path), GetParam());
  const auto program = workload.Prepare();
  ASSERT_TRUE(program) << program.error();
  const auto again = workload.Prepare();
  ASSERT_TRUE(again) << again.error();
  EXPECT_EQ(program->get(), again->get());

  runtime::Isolate forty;
  runtime::Isolate hundred;
  ASSERT_TRUE(forty.RegisterNative(runtime::NativeFunction::Bind<StartForty>("benchmark.start")));
  ASSERT_TRUE(
      hundred.RegisterNative(runtime::NativeFunction::Bind<StartHundred>("benchmark.start")));
  auto first = LoadJitWorkload(workload, forty.FreezeBindings());
  ASSERT_TRUE(first) << first.error();
  auto second = LoadJitWorkload(workload, hundred.FreezeBindings());
  ASSERT_TRUE(second) << second.error();
  EXPECT_EQ(first->program.get(), second->program.get());
  EXPECT_NE(first->isolate.get(), second->isolate.get());
  const auto initial = InvokeJitWorkload(*first, "advance");
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->AsInteger(), 40);
  const auto advanced = InvokeJitWorkload(*first, "advance");
  ASSERT_TRUE(advanced);
  EXPECT_EQ(advanced->AsInteger(), 41);
  first->isolate->heap().Collect();
  const auto independent = InvokeJitWorkload(*second, "advance");
  ASSERT_TRUE(independent);
  EXPECT_EQ(independent->AsInteger(), 100);
  first->isolate.reset();
  auto replacement = LoadJitWorkload(workload, forty.FreezeBindings());
  ASSERT_TRUE(replacement) << replacement.error();
  const auto restarted = InvokeJitWorkload(*replacement, "advance");
  ASSERT_TRUE(restarted);
  EXPECT_EQ(restarted->AsInteger(), 40);
  const auto surviving = InvokeJitWorkload(*second, "advance");
  ASSERT_TRUE(surviving);
  EXPECT_EQ(surviving->AsInteger(), 101);
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitWorkloadTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

class JitPreparationTest : public test::DirectoryTest {};

TEST_F(JitPreparationTest, PreparesOnlySelectedUniqueSuitesWithGoogleBenchmarkFilters) {
  constexpr std::string_view kPlainSource = "export func answer(): int { return 42; }";
  const auto path = directory() / "workload.vela";
  ASSERT_TRUE(WriteFileAtomically(path, std::as_bytes(std::span(kPlainSource))));
  const auto workload = std::make_shared<JitWorkload>(PathText(path), NativeCodeTier::Baseline);
  const auto missing = std::make_shared<JitWorkload>(PathText(directory() / "missing.vela"),
                                                     NativeCodeTier::Baseline);
  RegisterJitPreparation("preparation/one", workload);
  RegisterJitPreparation("preparation/two", workload);
  RegisterJitPreparation("preparation/missing", missing);
  const auto none = PrepareJitWorkloads("^no-such-benchmark$");
  ASSERT_TRUE(none);
  EXPECT_EQ(*none, 0);
  const auto selected = PrepareJitWorkloads("^preparation/(one|two)/real_time$");
  ASSERT_TRUE(selected) << selected.error();
  EXPECT_EQ(*selected, 1);
  const auto negative = PrepareJitWorkloads("-^preparation/missing/");
  ASSERT_TRUE(negative) << negative.error();
  EXPECT_EQ(*negative, 1);
  EXPECT_FALSE(PrepareJitWorkloads("["));
  EXPECT_FALSE(PrepareJitWorkloads("^preparation/missing/"));
}

}  // namespace
}  // namespace vela::benchmarks
