#include "compiler/jit/engine.h"

#include <cstddef>
#include <memory>
#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/Mangling.h>
#include <llvm/ExecutionEngine/Orc/MapperJITLinkMemoryManager.h>
#include <llvm/ExecutionEngine/Orc/MemoryMapper.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h>
#include <llvm/ExecutionEngine/Orc/SymbolStringPool.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Triple.h>

#include "compiler/jit/platform_support.h"
#include "compiler/jit/runtime_symbols.h"

namespace vela::compiler::jit {

CodeLinker::CodeLinker(std::unique_ptr<llvm::orc::ExecutionSession> session,
                       llvm::Triple triple,
                       const llvm::DataLayout& layout,
                       std::unique_ptr<llvm::jitlink::JITLinkMemoryManager> memory)
    : session_(std::move(session)),
      triple_(std::move(triple)),
      layout_(layout),
      mangler_(*session_, layout_),
      objects_(std::make_unique<llvm::orc::ObjectLinkingLayer>(*session_, std::move(memory))),
      runtime_(&session_->createBareJITDylib("vela_runtime")) {
  objects_->setOverrideObjectFlagsWithResponsibilityFlags(triple_.isOSBinFormatCOFF());
}

CodeLinker::~CodeLinker() {
  if (auto error = session_->endSession()) {
    session_->reportError(std::move(error));
  }
}

llvm::orc::SymbolStringPtr CodeLinker::mangleAndIntern(llvm::StringRef name) {
  return mangler_(name);
}

llvm::Expected<llvm::orc::JITDylib&> CodeLinker::createJITDylib(std::string name) {
  return session_->createJITDylib(std::move(name));
}

llvm::Error CodeLinker::addObjectFile(llvm::orc::ResourceTrackerSP tracker,
                                      std::unique_ptr<llvm::MemoryBuffer> object) {
  return objects_->add(std::move(tracker), std::move(object));
}

llvm::Expected<std::unique_ptr<CodeLinker>> CreateEngine(const llvm::Triple& triple,
                                                         const llvm::DataLayout& layout) {
  auto control = llvm::orc::SelfExecutorProcessControl::Create();
  if (!control) {
    return control.takeError();
  }
  // 在保留区内按系统页复用小代码对象，避免 Windows 每个对象提交至少 64 KiB。
  constexpr std::size_t kCodeReservationBytes = std::size_t{64} * 1024;
  auto memory =
      llvm::orc::MapperJITLinkMemoryManager::CreateWithMapper<llvm::orc::InProcessMemoryMapper>(
          kCodeReservationBytes);
  if (!memory) {
    return memory.takeError();
  }
  auto session = std::make_unique<llvm::orc::ExecutionSession>(std::move(*control));
  auto linker =
      std::make_unique<CodeLinker>(std::move(session), triple, layout, std::move(*memory));
  if (auto error = DefineRuntimeSymbols(*linker)) {
    return std::move(error);
  }
  if (auto error = InstallPlatformSupport(*linker)) {
    return std::move(error);
  }
  return linker;
}

}  // namespace vela::compiler::jit
