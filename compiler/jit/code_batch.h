#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>

#include "compiler/jit/code_resources.h"

namespace vela::compiler::jit {

// 批次在所有入口和直接调用边退休后释放 ORC 资源；按需物化只增加已发射函数数。
struct CodeBatch final {
  CodeBatch(std::shared_ptr<CodeLinker> source,
            llvm::orc::JITDylib& library,
            std::shared_ptr<CodeResourceCounters> counts,
            std::size_t count);
  ~CodeBatch();
  CodeBatch(const CodeBatch&) = delete;
  CodeBatch& operator=(const CodeBatch&) = delete;
  CodeBatch(CodeBatch&&) = delete;
  CodeBatch& operator=(CodeBatch&&) = delete;
  std::shared_ptr<CodeLinker> jit;
  llvm::orc::JITDylib& dylib;
  llvm::orc::ResourceTrackerSP tracker;
  std::shared_ptr<CodeResourceCounters> counters;
  std::atomic_size_t functions;
};

class BatchSymbols final {
 public:
  explicit BatchSymbols(CodeLinker& jit) : jit_(&jit) {}
  llvm::orc::SymbolStringPtr Get(std::string_view name);

 private:
  CodeLinker* jit_;
  absl::flat_hash_map<std::string, llvm::orc::SymbolStringPtr> symbols_;
};

}  // namespace vela::compiler::jit
