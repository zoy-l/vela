#pragma once

#include <absl/container/flat_hash_map.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {

class BatchSymbols;

struct ImageOwnership final {
  ImageOwnership(std::shared_ptr<CodeResourceCounters> counts,
                 const runtime::NativeImage& image,
                 std::shared_ptr<native::NativeImageData> storage);
  ~ImageOwnership();
  ImageOwnership(const ImageOwnership&) = delete;
  ImageOwnership& operator=(const ImageOwnership&) = delete;
  ImageOwnership(ImageOwnership&&) = delete;
  ImageOwnership& operator=(ImageOwnership&&) = delete;
  std::shared_ptr<CodeResourceCounters> counters;
  std::size_t image_data_bytes = 0;
  std::size_t constant_payload_bytes = 0;
  std::size_t debug_payload_bytes = 0;
  std::shared_ptr<native::NativeImageData> data;
  // 描述表覆盖代码与延迟发射状态的销毁；旧版本直接边继续保活其原批次。
  std::shared_ptr<void> batch;
  std::vector<std::shared_ptr<void>> code;
};

absl::flat_hash_map<std::string, ReusableCode> RetainImageCode(
    BatchSymbols& interned,
    const llvm::orc::SymbolMap& resolved,
    const std::shared_ptr<void>& resource,
    CodeReusePlan& plan,
    ImageOwnership& owner);

}  // namespace vela::compiler::jit
