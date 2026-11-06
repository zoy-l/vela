#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include <llvm/Support/Allocator.h>

#include "compiler/ir/ir.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::native {

class NativeImageBuilder;

// AOT 和 JIT 共用描述生产者。描述表不依赖 LLVM 常量或机器码批次，所有借用字节
// 由本对象拥有；JIT 只在发布前填写链接完成的入口，AOT 将同一描述序列化到目标文件。
class NativeImageData final {
 public:
  [[nodiscard]] const runtime::NativeImage* image() const noexcept { return &image_; }
  [[nodiscard]] std::size_t bytes() const noexcept {
    return sizeof(image_) + storage_.getBytesAllocated() +
           (functions_.size() * sizeof(runtime::NativeFunctionDescription));
  }
  void BindEntries(std::span<const runtime::NativeEntry> entries,
                   std::span<const runtime::NativeTypedEntry> typed_entries);

 private:
  friend class NativeImageBuilder;
  llvm::BumpPtrAllocator storage_;
  std::vector<runtime::NativeFunctionDescription> functions_;
  runtime::NativeImage image_;
};

[[nodiscard]] std::shared_ptr<NativeImageData> DescribeNativeImage(const ir::Program& program);

}  // namespace vela::compiler::native
