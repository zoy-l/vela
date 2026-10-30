#include "compiler/jit/compiler.h"

#include "compiler/source_session.h"

#include <basetsd.h>
#include <windows.h>  // IWYU pragma: keep

#include <winnt.h>

#include <bit>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace vela::compiler::jit {
namespace {

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
