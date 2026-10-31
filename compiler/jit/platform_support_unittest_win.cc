#include "compiler/jit/compiler.h"

#include "compiler/development/source_session.h"
#include "runtime/isolate.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_image.h"
#include "runtime/value.h"

#include <basetsd.h>
#include <windows.h>  // IWYU pragma: keep

#include <handleapi.h>
#include <minwindef.h>
#include <processthreadsapi.h>
#include <synchapi.h>
#include <winbase.h>
#include <winnt.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"

namespace vela::compiler::jit {
namespace {

struct SmallStackRun final {
  std::shared_ptr<const runtime::NativeImage> image;
  ExecutionStatus overflow = ExecutionStatus::NativeFailure;
  std::int64_t calls = 0;
  std::int64_t answer = 0;
};

DWORD WINAPI RunOnSmallStack(void* parameter) {
  auto& run = *static_cast<SmallStackRun*>(parameter);
  runtime::Isolate isolate;
  auto prepared = isolate.native_dispatch().Prepare(0, run.image);
  if (!prepared || !isolate.native_dispatch().Publish(std::move(*prepared))) {
    return 1;
  }
  const auto work = isolate.native_dispatch().FindEntry("main.vela", "work");
  const auto count = isolate.native_dispatch().FindEntry("main.vela", "calls");
  const auto answer = isolate.native_dispatch().FindEntry("main.vela", "answer");
  if (!work || !count || !answer) {
    return 2;
  }
  const auto overflow = work->Invoke({});
  run.overflow = overflow ? ExecutionStatus::Returned : overflow.error();
  const auto counted = count->Invoke({});
  const auto recovered = answer->Invoke({});
  if (!counted || !recovered) {
    return 3;
  }
  run.calls = counted->AsInteger();
  run.answer = recovered->AsInteger();
  return 0;
}

class JitStackTest : public testing::TestWithParam<NativeCodeTier> {};

TEST_P(JitStackTest, ReportsPhysicalStackExhaustionBeforeTheLogicalLimitAndRemainsUsable) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  const auto candidate = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = R"(
let count = 0;
func descend(): int { count++; return descend() + 1; }
export func work(): int { return descend(); }
export func calls(): int { return count; }
export func answer(): int { return 42; }
)",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(candidate);
  const auto code = (*compiler)->Compile(candidate->snapshot().program, {.tier = GetParam()});
  ASSERT_TRUE(code) << code.error().front().message;
  SmallStackRun run{.image = code->image};
  constexpr std::size_t kStackBytes = std::size_t{128} * 1024;
  auto* const thread = CreateThread(nullptr, kStackBytes, RunOnSmallStack, &run,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  ASSERT_NE(thread, nullptr);
  const auto waited = WaitForSingleObject(thread, INFINITE);
  DWORD exit_code = 1;
  const auto read_exit = GetExitCodeThread(thread, &exit_code);
  EXPECT_NE(CloseHandle(thread), 0);
  ASSERT_EQ(waited, WAIT_OBJECT_0);
  ASSERT_NE(read_exit, 0);
  ASSERT_EQ(exit_code, 0U);
  EXPECT_EQ(run.overflow, ExecutionStatus::StackOverflow);
  EXPECT_GT(run.calls, 0);
  EXPECT_LT(run.calls, kMaxCallFrames - 1);
  EXPECT_EQ(run.answer, 42);
}

INSTANTIATE_TEST_SUITE_P(NativeTiers,
                         JitStackTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

TEST(JitPlatformTest, CoffUnwindTablesAreRegisteredBeforePublicationAndRetiredWithTheCode) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler);
  SourceSession source("main.vela");
  const auto candidate = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "export func work(value: int): int { return value * 2; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(candidate);
  auto code = (*compiler)->Compile(candidate->snapshot().program);
  ASSERT_TRUE(code) << code.error().front().message;
  const auto entry = code->image->functions.values().front().entry;
  const auto address = std::bit_cast<std::uintptr_t>(entry);
  DWORD64 base = 0;
  const auto* table = RtlLookupFunctionEntry(address, &base, nullptr);
  ASSERT_NE(table, nullptr);
  EXPECT_LE(base + table->BeginAddress, address);
  EXPECT_GT(base + table->EndAddress, address);
  code->image.reset();
  EXPECT_EQ((*compiler)->resource_counts().batches, 0U);
  EXPECT_EQ(RtlLookupFunctionEntry(address, &base, nullptr), nullptr);
}

}  // namespace
}  // namespace vela::compiler::jit
