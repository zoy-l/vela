#pragma once

#include <memory>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Triple.h>

namespace vela::compiler::jit {

// 只加载目标文件，不创建 TargetMachine、IR 编译层或优化流水线。
class CodeLinker final {
 public:
  CodeLinker(std::unique_ptr<llvm::orc::ExecutionSession> session,
             llvm::Triple triple,
             const llvm::DataLayout& layout,
             std::unique_ptr<llvm::jitlink::JITLinkMemoryManager> memory);
  CodeLinker(const CodeLinker&) = delete;
  CodeLinker& operator=(const CodeLinker&) = delete;
  CodeLinker(CodeLinker&&) = delete;
  CodeLinker& operator=(CodeLinker&&) = delete;
  ~CodeLinker();
  llvm::orc::ExecutionSession& getExecutionSession() { return *session_; }
  llvm::orc::ObjectLinkingLayer& getObjLinkingLayer() { return *objects_; }
  llvm::orc::JITDylib& getMainJITDylib() { return *runtime_; }
  const llvm::Triple& getTargetTriple() const { return triple_; }
  const llvm::DataLayout& getDataLayout() const { return layout_; }
  llvm::orc::SymbolStringPtr mangleAndIntern(llvm::StringRef name);
  llvm::Expected<llvm::orc::JITDylib&> createJITDylib(std::string name);
  llvm::Error addObjectFile(llvm::orc::ResourceTrackerSP tracker,
                            std::unique_ptr<llvm::MemoryBuffer> object);

 private:
  std::unique_ptr<llvm::orc::ExecutionSession> session_;
  llvm::Triple triple_;
  llvm::DataLayout layout_;
  llvm::orc::MangleAndInterner mangler_;
  std::unique_ptr<llvm::orc::ObjectLinkingLayer> objects_;
  llvm::orc::JITDylib* runtime_;
};

[[nodiscard]] llvm::Expected<std::unique_ptr<CodeLinker>> CreateEngine();
[[nodiscard]] llvm::Expected<std::unique_ptr<CodeLinker>> CreateEngine(
    const llvm::Triple& triple,
    const llvm::DataLayout& layout);

}  // namespace vela::compiler::jit
