#include "compiler/native/root_store_optimizer.h"

#include <algorithm>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/test_support/native_pass_test_support.h"

namespace vela::compiler::native {
namespace {

TEST(NativeRootStoreTest, ReusesUnchangedStackRootsAcrossCallsButKeepsClearsAndEscapes) {
  llvm::LLVMContext context;
  auto module = test::ParseModule(R"(
target datalayout = "e-p:64:64"
declare void @collect(ptr)
define void @run(ptr %value, ptr %execution, i1 %replace) {
entry:
  %roots = alloca [32 x i8], align 16
  %kind = getelementptr i8, ptr %roots, i64 8
  %result = alloca i64
  store ptr %value, ptr %roots, !tbaa !2
  store i8 6, ptr %kind, !tbaa !2
  call void @collect(ptr %execution)
  br label %next
next:
  store ptr %value, ptr %roots, !tbaa !2
  store i8 6, ptr %kind, !tbaa !2
  call void @collect(ptr %result)
  store i8 0, ptr %kind, !tbaa !2
  call void @collect(ptr %execution)
  store ptr %value, ptr %roots, !tbaa !2
  store i8 6, ptr %kind, !tbaa !2
  call void @collect(ptr %roots)
  store ptr %value, ptr %roots, !tbaa !2
  store i8 6, ptr %kind, !tbaa !2
  br i1 %replace, label %cleared, label %retained
cleared:
  store i8 0, ptr %kind, !tbaa !2
  br label %merged
retained:
  br label %merged
merged:
  store ptr %value, ptr %roots, !tbaa !2
  store i8 6, ptr %kind, !tbaa !2
  ret void
}
!0 = !{!"Vela native storage"}
!1 = !{!"shadow roots", !0}
!2 = !{!1, !1, i64 0}
)",
                                  context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("run");
  const auto stores = [&] {
    return std::ranges::count_if(llvm::instructions(function), [](const auto& instruction) {
      return llvm::isa<llvm::StoreInst>(instruction);
    });
  };
  const auto before = stores();
  test::RunFunctionPasses(function, AddRootStorePasses);
  EXPECT_EQ(stores(), before);
  function.addFnAttr(kNativeReadOnlyRootsAttribute);
  test::RunFunctionPasses(function, AddRootStorePasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  // next 的重复 payload/tag、清空后的相同 payload、merge 中所有分支相同的 payload 可省。
  // tag 从 Null 恢复、显式地址逃逸后的两次写入及分支合流恢复必须保留。
  EXPECT_EQ(stores(), before - 4);
}

TEST(NativeRootStoreTest, PreservesWritesAfterUnknownOrOverlappingMemoryAndLoopBackedges) {
  llvm::LLVMContext context;
  auto module = test::ParseModule(R"(
target datalayout = "e-p:64:64"
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
define void @run(ptr %value, ptr %other, i1 %repeat) {
entry:
  %roots = alloca [32 x i8], align 16
  store ptr %value, ptr %roots, !tbaa !2
  store ptr %value, ptr %other
  store ptr %value, ptr %roots, !tbaa !2
  store i128 0, ptr %roots, !tbaa !2
  store ptr %value, ptr %roots, !tbaa !2
  call void @llvm.memset.p0.i64(ptr %roots, i8 0, i64 32, i1 false)
  store ptr %value, ptr %roots, !tbaa !2
  br label %loop
loop:
  store ptr %value, ptr %roots, !tbaa !2
  store volatile ptr %value, ptr %roots, !tbaa !2
  store ptr %value, ptr %roots, !tbaa !2
  br i1 %repeat, label %loop, label %done
done:
  ret void
}
!0 = !{!"Vela native storage"}
!1 = !{!"shadow roots", !0}
!2 = !{!1, !1, i64 0}
)",
                                  context);
  ASSERT_NE(module, nullptr);
  auto& function = *module->getFunction("run");
  function.addFnAttr(kNativeReadOnlyRootsAttribute);
  const auto count = [&] {
    return std::ranges::count_if(llvm::instructions(function), [](const auto& instruction) {
      return llvm::isa<llvm::StoreInst>(instruction);
    });
  };
  const auto before = count();
  test::RunFunctionPasses(function, AddRootStorePasses);
  EXPECT_FALSE(llvm::verifyModule(*module));
  EXPECT_EQ(count(), before);
}

}  // namespace
}  // namespace vela::compiler::native
